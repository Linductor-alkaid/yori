# 测试标签体系（M0-06，工程规范第 11 节）：
#   unit / integration / ipc / security / recovery / fuzz / performance / platform；
#   需要真实 NVML GPU 的用例额外打 gpu，需要多 Linux 用户的用例额外打 multi-user。
# 依赖真实环境的示例用例显式 skip（退出码 77 = ctest SKIP_RETURN_CODE），
# 并在输出中给出原因与补跑条件。

# ---- unit：版本契约 ------------------------------------------------------
add_executable(unit_version_test unit_version_test.cpp)
target_link_libraries(unit_version_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_version_test)
add_test(NAME example.unit.version COMMAND unit_version_test)
set_tests_properties(example.unit.version PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：Executor owner 生命周期（M1-01）-------------------------------
add_executable(unit_executor_runtime_test unit_executor_runtime_test.cpp)
target_link_libraries(unit_executor_runtime_test PRIVATE yori::runtime executor::executor)
yori_apply_warnings(unit_executor_runtime_test)
add_test(NAME m1.unit.executor-runtime COMMAND unit_executor_runtime_test)
set_tests_properties(m1.unit.executor-runtime PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- compile boundary：Yori 公共头不得依赖 Executor 头（M1-01）-----------
# 该目标只获得 yori_core 的公开 include path，故任何 <executor/...> 泄漏都会在
# 编译阶段失败；它也覆盖安装集合中的全部源码公共头。
add_executable(public_header_boundary_test public_header_boundary_test.cpp)
target_link_libraries(public_header_boundary_test PRIVATE yori::yori_core)
yori_apply_warnings(public_header_boundary_test)
add_test(NAME m1.unit.public-header-boundary COMMAND public_header_boundary_test)
set_tests_properties(m1.unit.public-header-boundary PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：JobSpec 与状态机（M1-02）--------------------------------------
add_executable(unit_job_spec_test unit_job_spec_test.cpp)
target_link_libraries(unit_job_spec_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_job_spec_test)
add_test(NAME m1.unit.job-spec COMMAND unit_job_spec_test)
set_tests_properties(m1.unit.job-spec PROPERTIES LABELS "unit" TIMEOUT 60)

add_executable(unit_job_state_machine_test unit_job_state_machine_test.cpp)
target_link_libraries(unit_job_state_machine_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_job_state_machine_test)
add_test(NAME m1.unit.job-state-machine COMMAND unit_job_state_machine_test)
set_tests_properties(m1.unit.job-state-machine PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：GpuProvider / StateStore SPI 与进程内实现（M1-03）--------------
add_executable(unit_fake_gpu_provider_test unit_fake_gpu_provider_test.cpp)
target_link_libraries(unit_fake_gpu_provider_test PRIVATE yori::test_support)
yori_apply_warnings(unit_fake_gpu_provider_test)
add_test(NAME m1.unit.fake-gpu-provider COMMAND unit_fake_gpu_provider_test)
set_tests_properties(m1.unit.fake-gpu-provider PROPERTIES LABELS "unit" TIMEOUT 60)

add_executable(unit_in_memory_state_store_test unit_in_memory_state_store_test.cpp)
target_link_libraries(unit_in_memory_state_store_test PRIVATE yori::test_support)
yori_apply_warnings(unit_in_memory_state_store_test)
add_test(NAME m1.unit.in-memory-state-store COMMAND unit_in_memory_state_store_test)
set_tests_properties(m1.unit.in-memory-state-store PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：服务器级全局 FIFO 队列与准入（M1-04）-------------------------
add_executable(unit_global_job_queue_test unit_global_job_queue_test.cpp)
target_link_libraries(unit_global_job_queue_test PRIVATE yori::test_support)
yori_apply_warnings(unit_global_job_queue_test)
add_test(NAME m1.unit.global-job-queue COMMAND unit_global_job_queue_test)
set_tests_properties(m1.unit.global-job-queue PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：事件驱动 FIFO 调度、lease 与 Executor 任务边界（M1-05）-------
add_executable(unit_fifo_scheduler_test unit_fifo_scheduler_test.cpp)
target_link_libraries(unit_fifo_scheduler_test PRIVATE yori::test_support)
yori_apply_warnings(unit_fifo_scheduler_test)
add_test(NAME m1.unit.fifo-scheduler COMMAND unit_fifo_scheduler_test)
set_tests_properties(m1.unit.fifo-scheduler PROPERTIES LABELS "unit" TIMEOUT 60)

add_executable(unit_scheduler_task_runner_test unit_scheduler_task_runner_test.cpp)
target_link_libraries(unit_scheduler_task_runner_test
  PRIVATE yori::runtime yori::test_support executor::executor)
yori_apply_warnings(unit_scheduler_task_runner_test)
add_test(NAME m1.unit.scheduler-task-runner COMMAND unit_scheduler_task_runner_test)
set_tests_properties(m1.unit.scheduler-task-runner PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：LaunchProfile/LaunchAdapter 契约（M2-01）-----------------------
add_executable(unit_launch_adapter_test unit_launch_adapter_test.cpp)
target_link_libraries(unit_launch_adapter_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_launch_adapter_test)
add_test(NAME m2.unit.launch-adapter COMMAND unit_launch_adapter_test)
set_tests_properties(m2.unit.launch-adapter PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：进程引擎与守护状态机（M2-02/M2-03）-----------------------------
add_executable(unit_process_supervisor_test unit_process_supervisor_test.cpp)
target_link_libraries(unit_process_supervisor_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_process_supervisor_test)
add_test(NAME m2.unit.process-supervisor COMMAND unit_process_supervisor_test)
set_tests_properties(m2.unit.process-supervisor PROPERTIES LABELS "unit" TIMEOUT 120)

# ---- unit：有界日志落盘与轮转（M2-05）-------------------------------------
add_executable(unit_log_sink_test unit_log_sink_test.cpp)
target_link_libraries(unit_log_sink_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_log_sink_test)
add_test(NAME m2.unit.log-sink COMMAND unit_log_sink_test)
set_tests_properties(m2.unit.log-sink PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：退出监视与日志泵的 Executor 承载（M2-04）-----------------------
add_executable(unit_exit_monitor_test unit_exit_monitor_test.cpp)
target_link_libraries(unit_exit_monitor_test
  PRIVATE yori::yori_core yori::runtime executor::executor)
yori_apply_warnings(unit_exit_monitor_test)
add_test(NAME m2.unit.exit-monitor COMMAND unit_exit_monitor_test)
set_tests_properties(m2.unit.exit-monitor PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_log_pump_test unit_log_pump_test.cpp)
target_link_libraries(unit_log_pump_test
  PRIVATE yori::yori_core yori::runtime executor::executor)
yori_apply_warnings(unit_log_pump_test)
add_test(NAME m2.unit.log-pump COMMAND unit_log_pump_test)
set_tests_properties(m2.unit.log-pump PROPERTIES LABELS "unit" TIMEOUT 120)

# ---- integration：LaunchAdapter -> spawn -> 监视/落盘 -> 取消升级 ---------
add_executable(integration_process_supervision_test integration_process_supervision_test.cpp)
target_link_libraries(integration_process_supervision_test
  PRIVATE yori::yori_core yori::runtime executor::executor)
yori_apply_warnings(integration_process_supervision_test)
add_test(NAME m2.integration.process-supervision COMMAND integration_process_supervision_test)
set_tests_properties(m2.integration.process-supervision PROPERTIES
  LABELS "integration" TIMEOUT 180)

# ---- stub NVML 共享库（M3-03）：适配器单测的可控 NVML 假实现 ----------------
# 与 NvmlGpuProvider 共享 src/gpu/nvml_api.hpp 内部声明（配对 ABI）。导出
# stub_nvml_* 控制接口，测试经 dlopen 同一路径 + dlsym 注入状态。
add_library(yori_stub_nvml SHARED stub_nvml/stub_nvml.cpp)
target_include_directories(yori_stub_nvml PRIVATE ${PROJECT_SOURCE_DIR}/src)
yori_apply_warnings(yori_stub_nvml)

# ---- unit：NvmlGpuProvider 适配器（M3-01/M3-03）-----------------------------
add_executable(unit_nvml_gpu_provider_test unit_nvml_gpu_provider_test.cpp)
target_link_libraries(unit_nvml_gpu_provider_test PRIVATE yori::nvml)
target_include_directories(unit_nvml_gpu_provider_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(unit_nvml_gpu_provider_test PRIVATE
  YORI_STUB_NVML_PATH="$<TARGET_FILE:yori_stub_nvml>")
yori_apply_warnings(unit_nvml_gpu_provider_test)
add_test(NAME m3.unit.nvml-gpu-provider COMMAND unit_nvml_gpu_provider_test)
set_tests_properties(m3.unit.nvml-gpu-provider PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：GpuManager 周期采样、快照与事件（M3-02/M3-04）--------------------
add_executable(unit_gpu_manager_test unit_gpu_manager_test.cpp)
target_link_libraries(unit_gpu_manager_test
  PRIVATE yori::yori_core yori::runtime executor::executor)
yori_apply_warnings(unit_gpu_manager_test)
add_test(NAME m3.unit.gpu-manager COMMAND unit_gpu_manager_test)
set_tests_properties(m3.unit.gpu-manager PROPERTIES LABELS "unit" TIMEOUT 120)

# ---- integration：GpuManager 事件驱动调度闭环（M3-04）----------------------
add_executable(integration_gpu_manager_scheduler_test integration_gpu_manager_scheduler_test.cpp)
target_link_libraries(integration_gpu_manager_scheduler_test
  PRIVATE yori::runtime yori::test_support executor::executor)
yori_apply_warnings(integration_gpu_manager_scheduler_test)
add_test(NAME m3.integration.gpu-scheduling COMMAND integration_gpu_manager_scheduler_test)
set_tests_properties(m3.integration.gpu-scheduling PROPERTIES
  LABELS "integration" TIMEOUT 180)

# ---- unit：SQLite StateStore 适配器（M4-02）---------------------------------
# 直接 dlopen 系统 libsqlite3.so.0 驱动真实库；测试内最小 SQL 绑定共享
# src/store/sqlite_api.hpp（配对 ABI），用于篡改/故障注入。
add_executable(unit_sqlite_state_store_test unit_sqlite_state_store_test.cpp)
target_link_libraries(unit_sqlite_state_store_test PRIVATE yori::sqlite)
target_include_directories(unit_sqlite_state_store_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
yori_apply_warnings(unit_sqlite_state_store_test)
add_test(NAME m4.unit.sqlite-state-store COMMAND unit_sqlite_state_store_test)
set_tests_properties(m4.unit.sqlite-state-store PROPERTIES LABELS "unit" TIMEOUT 120)

# ---- unit：JobRecovery 决策与恢复流程（M4-03）-------------------------------
add_executable(unit_job_recovery_test unit_job_recovery_test.cpp)
target_link_libraries(unit_job_recovery_test PRIVATE yori::yori_core yori::test_support)
yori_apply_warnings(unit_job_recovery_test)
add_test(NAME m4.unit.job-recovery COMMAND unit_job_recovery_test)
set_tests_properties(m4.unit.job-recovery PROPERTIES LABELS "unit" TIMEOUT 60)

# ---- unit：StoreTaskRunner 的 Executor 载载（M4-04，EXEC-08）----------------
add_executable(unit_store_task_runner_test unit_store_task_runner_test.cpp)
target_link_libraries(unit_store_task_runner_test
  PRIVATE yori::runtime yori::test_support executor::executor)
yori_apply_warnings(unit_store_task_runner_test)
add_test(NAME m4.unit.store-task-runner COMMAND unit_store_task_runner_test)
set_tests_properties(m4.unit.store-task-runner PROPERTIES LABELS "unit" TIMEOUT 120)

# ---- integration;recovery：daemon 重启恢复闭环（M4-03/M4-02）----------------
# 替换 M0 的 example.recovery.restart 占位：SQLite 持久化 -> 重开 -> 身份核验
# 采纳/LOST -> 队列重建 -> kRecoveryCompleted 触发调度；含 PID reuse 防护。
add_executable(integration_recovery_test integration_recovery_test.cpp)
target_link_libraries(integration_recovery_test
  PRIVATE yori::yori_core yori::sqlite executor::executor)
yori_apply_warnings(integration_recovery_test)
add_test(NAME m4.integration.recovery-restart COMMAND integration_recovery_test)
set_tests_properties(m4.integration.recovery-restart PROPERTIES
  LABELS "integration;recovery" TIMEOUT 180)

# ---- security;multi-user：root 降权身份断言（DEC-004 M2 验证）-------------
add_executable(security_process_demotion_test security_process_demotion_test.cpp)
target_link_libraries(security_process_demotion_test PRIVATE yori::yori_core)
yori_apply_warnings(security_process_demotion_test)
add_test(NAME m2.security.process-demotion COMMAND security_process_demotion_test)
set_tests_properties(m2.security.process-demotion PROPERTIES
  LABELS "security;multi-user" SKIP_RETURN_CODE 77 TIMEOUT 120)

# ---- integration：CLI/daemon 冒烟 ----------------------------------------
add_test(NAME example.integration.yori-version COMMAND $<TARGET_FILE:yori> --version)
set_tests_properties(example.integration.yori-version PROPERTIES
  LABELS "integration" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "^yori [0-9]+\\.[0-9]+\\.[0-9]+")

add_test(NAME example.integration.yorid-version COMMAND $<TARGET_FILE:yorid> --version)
set_tests_properties(example.integration.yorid-version PROPERTIES
  LABELS "integration" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "^yorid [0-9]+\\.[0-9]+\\.[0-9]+")

# ---- platform;gpu：真实 NVIDIA GPU 探测（M3 起经 NvmlGpuProvider）----------
add_executable(example_gpu_probe_test example_gpu_probe_test.cpp)
target_link_libraries(example_gpu_probe_test PRIVATE yori::nvml)
yori_apply_warnings(example_gpu_probe_test)
add_test(NAME m3.platform.gpu-nvml COMMAND example_gpu_probe_test)
set_tests_properties(m3.platform.gpu-nvml PROPERTIES
  LABELS "platform;gpu" SKIP_RETURN_CODE 77 TIMEOUT 60)

# ---- security;multi-user：多用户环境前置示例 --------------------------------
add_executable(example_multiuser_test example_multiuser_test.cpp)
yori_apply_warnings(example_multiuser_test)
add_test(NAME example.security.multi-user COMMAND example_multiuser_test)
set_tests_properties(example.security.multi-user PROPERTIES
  LABELS "security;multi-user" SKIP_RETURN_CODE 77 TIMEOUT 60)

# ---- 后续里程碑真实用例接入前的显式 skip 占位 -------------------------------
add_executable(example_pending_test example_pending_test.cpp)
yori_apply_warnings(example_pending_test)

function(yori_add_pending_example test_name labels reason)
  add_test(NAME ${test_name} COMMAND example_pending_test "${reason}")
  set_tests_properties(${test_name} PROPERTIES
    LABELS "${labels}" SKIP_RETURN_CODE 77 TIMEOUT 60)
endfunction()

yori_add_pending_example(example.ipc.protocol "ipc"
  "M5 IPC 协议解析用例接入后启用；补跑条件：M5 合入后运行 ctest -L ipc")
yori_add_pending_example(example.fuzz.parser "fuzz"
  "M5 IPC parser fuzz corpus 接入后启用；补跑条件：M5 合入后运行 ctest -L fuzz")
yori_add_pending_example(example.performance.scheduler "performance"
  "调度性能基准用例（M6+）接入后启用；补跑条件：基准方法与目标定稿后运行 ctest -L performance")
