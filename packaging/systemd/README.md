# packaging/systemd

systemd unit 与部署说明（设计 §10.1、DEC-004、DEC-010）。`yori.service` 随
CMake `install()` 部署到 `${CMAKE_INSTALL_LIBEXECDIR}/systemd/system/`（默认
前缀 `/usr/local` 时为 `/usr/local/lib/systemd/system/yori.service`）。

## 安装与启用（root）

```bash
cmake --preset release -DYORI_FETCH_DEPENDENCIES=OFF
cmake --build --preset release
sudo cmake --install build/release --prefix /usr/local
sudo systemctl enable --now yori.service
```

安装内容包括：

- `/usr/local/bin/yori`、`/usr/local/bin/yorid`：CLI 与 daemon；
- systemd unit（见上）；
- 开发库与头文件（`libyori*`、`include/yori`），服务器部署可不安装。

## 部署前置（root，一次性）

1. **IPC 连接组（DEC-010）**：`/run/yori/yori.sock` 属主 `root:yori`、模式
   `0660`。创建系统组并把允许提交/查询的用户加入：

   ```bash
   sudo groupadd -r yori
   sudo usermod -aG yori <user>   # 每个允许使用 Yori 的用户
   ```

2. **管理员组（可选）**：以 `--admin-gid <GID>` 启动（或在 drop-in
   `/etc/systemd/system/yori.service.d/override.conf` 中以空 `ExecStart=`
   行后接完整命令覆盖），成员可查看与取消任何 Job。

3. **运行时目录**：systemd `RuntimeDirectory=yori` 自动创建 `/run/yori`；
   socket 属主收敛由 yorid 以 `--socket-group <GID>` 完成（drop-in 示例）：

   ```ini
   [Service]
   ExecStart=
   ExecStart=/usr/local/bin/yorid --socket-group <yori 组的 GID>
   ```

4. **状态与日志目录**：`StateDirectory=yori` 创建 `/var/lib/yori`（0750，
   root）。SQLite 状态库默认 `/var/lib/yori/state.db`（符号链接拒绝、权限
   0600，DEC-009）；Job 日志根目录默认 `/var/lib/yori/jobs`（yorid 启动时校验
   父目录链属主与写位，威胁模型基线 8），可用 `--log-root` 覆盖。

5. **NVML / SQLite 动态库**：运行时 `dlopen`（默认
   `libnvidia-ml.so.1` / `libsqlite3.so.0`），可用 `--gpu-library` /
   `--sqlite-library` 指定路径。

## 守护语义要点

- daemon 正常停止（`systemctl stop yori`）**不终止**运行中的训练进程
  （RULE-10）；训练进程的 `SIGPIPE` 在 exec 前被忽略（DEC-008），重启窗口内
  的日志输出丢失、文件原位续写。
- 重启后 `JobRecovery` 以 PID/PGID/启动时间核验进程身份并重新采纳 RUNNING
  Job（RULE-06，绝不无条件重启）；无法核验的活动 Job 转 `LOST` 并释放 GPU。
- unit 不使用 `KillMode=control-group`（默认会击杀 daemon 的全部后代，违反
  RULE-10）；yorid 退出前显式 abandon 受守护进程。

## CI 覆盖与真机补跑

- CI（非 root、无 systemd/NVML/多用户）覆盖：安装（`cmake --install`）后
  unit 文件存在；守护闭环以进程内 Daemon + Fake GPU 覆盖。
- 需真机补跑（root + systemd + NVIDIA GPU + 至少两个 Linux 用户）：设计
  §19 判据中的真机项，记录于
  [M7 验证记录](../../docs/plans/m7-packaging-acceptance.md)。
