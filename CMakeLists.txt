cmake_minimum_required(VERSION 3.25)

project(yori
  VERSION 0.2.0
  DESCRIPTION "单节点多用户 GPU 训练任务排队、调度与进程守护系统"
  LANGUAGES CXX)

# C++20（工程规范第 11 节）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

include(GNUInstallDirs)

# ---- 选项 ---------------------------------------------------------------

option(YORI_BUILD_TESTS "构建测试" ON)
option(YORI_WARNINGS_AS_ERRORS "将编译器关键警告视为错误" ON)
option(YORI_FETCH_DEPENDENCIES
  "允许 configure 阶段初始化缺失的依赖 submodule；OFF 表示不执行任何拉取，仅做一致性校验（离线/CI 默认路径）"
  ON)

# ---- pinned 依赖校验（M0-03，工程规范 9.1/10.7）--------------------------
include(cmake/Dependencies.cmake)
yori_validate_pinned_dependencies()
yori_add_pinned_dependencies()

# ---- 供应链报告骨架（依赖策略第 6 节）------------------------------------
include(cmake/SupplyChain.cmake)
yori_add_supply_chain_report_target()

# ---- 编译警告政策 --------------------------------------------------------
include(cmake/CompilerWarnings.cmake)

add_subdirectory(src)
add_subdirectory(apps)

# ---- 打包（M7）：systemd unit 随安装部署 ----------------------------------
# 设计 10.1 + DEC-004/RULE-10：Type=simple、RuntimeDirectory=yori；守护语义
# （daemon 停止不终止训练）要求不以 control-group 方式回收后代。ExecStart 的
# 安装前缀在 configure 阶段注入（源码安装 /usr/local，deb 安装 /usr）。
# 部署前置（yori 组、--socket-group、状态目录）见 packaging/systemd/README.md；
# deb 打包见 packaging/deb/build-deb.sh。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  configure_file(packaging/systemd/yori.service.in
                 "${CMAKE_BINARY_DIR}/generated/packaging/yori.service" @ONLY)
  # systemd 只扫描 <prefix>/lib/systemd/system（与 multiarch 的 LIBDIR 无关，
  # Debian 上 CMAKE_INSTALL_LIBDIR 会变为 lib/<arch>，故使用固定相对路径）。
  install(FILES "${CMAKE_BINARY_DIR}/generated/packaging/yori.service"
          DESTINATION lib/systemd/system)
endif()

if(YORI_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(tests)
endif()
