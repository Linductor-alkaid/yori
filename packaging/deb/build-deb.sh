#!/usr/bin/env bash
# Yori 运行时 deb 打包（M7 发布 / v0.1.0）。
#
# 用法：packaging/deb/build-deb.sh <out-dir> [version]
#   out-dir  ：deb 输出目录（自动创建）
#   version  ：缺省从源码树 CMakeLists.txt 的 project(VERSION) 解析
#
# 脚本自建独立构建树（Release、CMAKE_INSTALL_PREFIX=/usr）：systemd unit 的
# ExecStart 在 configure 阶段固化安装前缀，必须以 deb 的真实前缀（/usr）
# 重新 configure，不能复用源码安装（/usr/local）的构建树。
#
# 产物：yori_<version>_<arch>.deb——仅含运行时文件（yori/yorid、systemd
# unit、文档）；开发库与头文件不进入运行时包。安装后由 postinst 创建
# yori 系统组、注入 --socket-group drop-in 并启用启动服务（RULE-10：
# 移除/停止服务不终止训练，状态目录不随卸载删除）。
set -euo pipefail

OUT_DIR=${1:?usage: build-deb.sh <out-dir> [version]}
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${SOURCE_DIR}/build/deb-package"
VERSION=${3:-$(sed -n 's/^  VERSION \(.*\)$/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)}
ARCH=$(dpkg --print-architecture)
DEB_NAME="yori_${VERSION}_${ARCH}.deb"

for tool in cmake ninja dpkg-deb; do
  command -v "${tool}" >/dev/null || { echo "build-deb: missing tool: ${tool}" >&2; exit 1; }
done

# 独立构建树：deb 前缀 /usr（unit 的 ExecStart 随此前缀固化）。
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DYORI_FETCH_DEPENDENCIES=OFF >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

STAGE=$(mktemp -d)
trap 'rm -rf "${STAGE}"' EXIT

# 安装到暂存区（--prefix 覆盖真实落点；内容前缀已是 /usr）。
cmake --install "${BUILD_DIR}" --prefix "${STAGE}/usr" >/dev/null

# 运行时包裁剪：移除开发产物（头文件、静态库、CMake 包配置）。
rm -rf "${STAGE}/usr/include"
find "${STAGE}/usr" -type f \( -name 'libyori*.a' -o -name 'yori-config*.cmake' \) -delete
find "${STAGE}/usr" -type d -name cmake -empty -delete
find "${STAGE}/usr/lib" "${STAGE}/usr/libexec" -type d -empty -delete 2>/dev/null || true
# 目录权限收敛（mktemp 的 0700 不应进入包内）。
find "${STAGE}" -type d -exec chmod 0755 {} +

# 二进制去符号（发行惯例；保留可执行权限）。
find "${STAGE}/usr/bin" -maxdepth 1 -type f -print0 | xargs -0 -r strip --strip-unneeded

mkdir -p "${STAGE}/DEBIAN" "${OUT_DIR}"

cat > "${STAGE}/DEBIAN/control" <<EOF
Package: yori
Version: ${VERSION}
Section: admin
Priority: optional
Architecture: ${ARCH}
Depends: libsqlite3-0
Maintainer: Linductor-alkaid <linductor-alkaid@users.noreply.github.com>
Description: single-node multi-user GPU training job queue, scheduler and supervisor
 Yori queues, schedules and supervises GPU training jobs submitted by multiple
 Linux users on a single multi-GPU server. Jobs run as the submitting user;
 the daemon (yorid) owns the authoritative global FIFO queue and GPU leases,
 survives daemon restarts via identity-verified adoption, and never kills
 running training on its own shutdown.
Homepage: https://github.com/Linductor-alkaid/yori
EOF

cat > "${STAGE}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
# 安装/升级后：创建连接组、注入 socket 属主 drop-in（DEC-010）并启用启动
# 服务。无 NVIDIA 驱动的机器上服务启动失败不阻塞安装（yorid 依赖 NVML）。
set -e

YORI_GROUP=yori
if ! getent group "${YORI_GROUP}" >/dev/null; then
  addgroup --system "${YORI_GROUP}"
fi
GID=$(getent group "${YORI_GROUP}" | cut -d: -f3)

mkdir -p /etc/systemd/system/yori.service.d
cat > /etc/systemd/system/yori.service.d/yori.conf <<DROPIN
[Service]
ExecStart=
ExecStart=/usr/bin/yorid --socket-group ${GID}
DROPIN

if [ -d /run/systemd/system ]; then
  systemctl daemon-reload
  systemctl enable yori.service >/dev/null 2>&1 || true
  systemctl start yori.service || echo \
    "yori: service failed to start (no NVIDIA driver / NVML unavailable?); see: journalctl -u yori" >&2
fi

echo "yori: add users to the '${YORI_GROUP}' group to allow submissions: usermod -aG ${YORI_GROUP} <user>"

exit 0
EOF

cat > "${STAGE}/DEBIAN/prerm" <<'EOF'
#!/bin/sh
# 移除/升级前停止 daemon。RULE-10：yorid 退出不终止运行中的训练进程；
# 训练进程随后由系统 init 接续，重装后经恢复路径重新采纳。
set -e
if [ -d /run/systemd/system ]; then
  systemctl stop yori.service || true
fi
exit 0
EOF

cat > "${STAGE}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
# 移除后停用服务。状态数据（/var/lib/yori：Job 历史与日志）与用户组保留，
# 不随卸载删除（purge 亦不触碰训练数据目录）。
set -e
if [ "${1:-}" = "remove" ] && [ -d /run/systemd/system ]; then
  systemctl disable yori.service >/dev/null 2>&1 || true
  systemctl daemon-reload || true
fi
exit 0
EOF

chmod 0755 "${STAGE}/DEBIAN/postinst" "${STAGE}/DEBIAN/prerm" "${STAGE}/DEBIAN/postrm"

dpkg-deb --build "${STAGE}" "${OUT_DIR}/${DEB_NAME}" >/dev/null
echo "build-deb: ${OUT_DIR}/${DEB_NAME}"

# 自检：control 可解析、关键文件在位（contents 先落变量，避免 grep -q 的
# 早退 SIGPIPE 噪声）。
dpkg-deb --field "${OUT_DIR}/${DEB_NAME}" Package Version Architecture
# 防回归：deb 不得携带任何 NVIDIA 依赖——NVML 由运行期 dlopen 解析，驱动
# 栈由管理员维护；apt 默认安装 Recommends，拉入新版驱动组件会与已安装的
# 版本化驱动系列（如 nvidia-driver-570）冲突，导致 apt 卸载既有驱动栈
# （v0.1.0 已知问题，v0.1.1 修复）。
if dpkg-deb --field "${OUT_DIR}/${DEB_NAME}" Depends Recommends Suggests 2>/dev/null \
    | grep -qi nvidia; then
  echo "build-deb: control must not reference nvidia packages" >&2
  exit 1
fi
CONTENTS=$(dpkg-deb --contents "${OUT_DIR}/${DEB_NAME}")
for entry in '/usr/bin/yorid$' '/usr/bin/yori$' 'systemd/system/yori.service$'; do
  echo "${CONTENTS}" | grep -Eq "${entry}" || { echo "build-deb: missing ${entry}" >&2; exit 1; }
done
