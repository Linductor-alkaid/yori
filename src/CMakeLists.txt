# yori_core：Core 静态库。M0 仅承载版本信息；
# M1 起填充 JobSpec、状态机、全局队列与调度契约（设计第 4 节）。
configure_file(
  "${PROJECT_SOURCE_DIR}/include/yori/version.h.in"
  "${PROJECT_BINARY_DIR}/generated/yori/version.h"
  @ONLY)

add_library(yori_core
  gpu/gpu_provider.cpp
  ipc/ipc_protocol.cpp
  ipc/ipc_service.cpp
  ipc/ipc_transport.cpp
  job/job.cpp
  launch/launch_adapter.cpp
  observe/log_sink.cpp
  process/process_supervisor.cpp
  queue/job_queue.cpp
  recovery/job_recovery.cpp
  scheduler/scheduler.cpp
  store/mutation_core.cpp
  store/state_store.cpp
  version.cpp)
add_library(yori::yori_core ALIAS yori_core)

target_include_directories(yori_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

yori_apply_warnings(yori_core)

# 进程私有的 Executor owner 边界。该目标不安装、不从 yori_core 传递，确保
# Core 公开契约不暴露第三方类型。当前仅 yorid 持有它；无状态 CLI 在出现异步
# 工作前不创建 Executor，届时必须由 CLI 主生命周期建立独立 owner。
add_library(yori_runtime STATIC
  runtime/daemon.cpp
  runtime/executor_runtime.cpp
  runtime/grace_escalation.cpp
  runtime/gpu_manager.cpp
  runtime/ipc_server.cpp
  runtime/job_manager.cpp
  runtime/log_follow_service.cpp
  runtime/log_pump.cpp
  runtime/log_streamer.cpp
  runtime/process_exit_monitor.cpp
  runtime/scheduler_task_runner.cpp
  runtime/serial_state_store.cpp
  runtime/store_task_runner.cpp)
add_library(yori::runtime ALIAS yori_runtime)
target_include_directories(yori_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(yori_runtime PRIVATE executor::executor yori::yori_core yori::ipc)
yori_apply_warnings(yori_runtime)

# UDS IPC 客户端适配（M5-02，Adapter 层，RULE-02）：无 Executor、同步有界；
# 服务端（Executor blocking worker 承载）在 yori_runtime。内部头 ipc/uds_io.hpp
# 与 yori_runtime 共享（帧 I/O）。
add_library(yori_ipc
  ipc/uds_client.cpp
  ipc/uds_io.cpp)
add_library(yori::ipc ALIAS yori_ipc)
target_include_directories(yori_ipc
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yori_ipc PUBLIC yori::yori_core)
yori_apply_warnings(yori_ipc)

# NVML 适配（M3-01，Adapter 层，RULE-02）：运行期 dlopen 绑定真实驱动，构建
# 不依赖 CUDA/NVML SDK；公开头只暴露 Core SPI 类型。内部声明头 gpu/nvml_api.hpp
# 与 tests/stub_nvml 共享以保证配对 ABI。
add_library(yori_nvml gpu/nvml_gpu_provider.cpp)
add_library(yori::nvml ALIAS yori_nvml)
target_include_directories(yori_nvml
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yori_nvml PUBLIC yori::yori_core ${CMAKE_DL_LIBS})
yori_apply_warnings(yori_nvml)

# SQLite 适配（M4-02，Adapter 层，RULE-02）：运行期 dlopen 绑定系统
# libsqlite3.so.0，构建不依赖 libsqlite3-dev；公开头只暴露 Core SPI 类型。
# 内部声明头 store/sqlite_api.hpp 与共享验证核心 store/mutation_core.hpp 保持
# 与内存实现同语义。
add_library(yori_sqlite store/sqlite_state_store.cpp)
add_library(yori::sqlite ALIAS yori_sqlite)
target_include_directories(yori_sqlite
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yori_sqlite PUBLIC yori::yori_core ${CMAKE_DL_LIBS})
yori_apply_warnings(yori_sqlite)

if(YORI_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
  add_library(yori_test_support STATIC
    testing/fake_gpu_provider.cpp
    testing/in_memory_state_store.cpp)
  add_library(yori::test_support ALIAS yori_test_support)
  target_include_directories(yori_test_support
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
  target_link_libraries(yori_test_support PUBLIC yori::yori_core)
  yori_apply_warnings(yori_test_support)
endif()

set_target_properties(yori_core PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

install(TARGETS yori_core yori_nvml yori_sqlite yori_ipc EXPORT yori-export
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# 公共头安装：源码树占位头（排除模板 *.in）+ 构建树生成的 version.h
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/yori/"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yori
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.hpp"
  PATTERN "*.in" EXCLUDE)
install(DIRECTORY "${PROJECT_BINARY_DIR}/generated/yori/"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yori)

# 导出包：主配置 + 版本文件（consumer 以 find_package(yori 0.1) 解析）
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/yori-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/yori-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yori)

install(EXPORT yori-export
  NAMESPACE yori::
  FILE yori-config.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yori)
