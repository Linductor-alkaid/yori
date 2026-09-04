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

# ---- integration：CLI/daemon 冒烟 ----------------------------------------
add_test(NAME example.integration.yori-version COMMAND $<TARGET_FILE:yori> --version)
set_tests_properties(example.integration.yori-version PROPERTIES
  LABELS "integration" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "^yori [0-9]+\\.[0-9]+\\.[0-9]+")

add_test(NAME example.integration.yorid-version COMMAND $<TARGET_FILE:yorid> --version)
set_tests_properties(example.integration.yorid-version PROPERTIES
  LABELS "integration" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "^yorid [0-9]+\\.[0-9]+\\.[0-9]+")

# ---- platform;gpu：真实 NVIDIA GPU 探测示例 --------------------------------
add_executable(example_gpu_probe_test example_gpu_probe_test.cpp)
yori_apply_warnings(example_gpu_probe_test)
add_test(NAME example.platform.gpu-nvml COMMAND example_gpu_probe_test)
set_tests_properties(example.platform.gpu-nvml PROPERTIES
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
yori_add_pending_example(example.recovery.restart "recovery"
  "M4 daemon 重启恢复用例接入后启用；补跑条件：M4 合入后运行 ctest -L recovery")
yori_add_pending_example(example.fuzz.parser "fuzz"
  "M5 IPC parser fuzz corpus 接入后启用；补跑条件：M5 合入后运行 ctest -L fuzz")
yori_add_pending_example(example.performance.scheduler "performance"
  "调度性能基准用例（M6+）接入后启用；补跑条件：基准方法与目标定稿后运行 ctest -L performance")
