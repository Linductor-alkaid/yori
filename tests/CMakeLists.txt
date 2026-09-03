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
