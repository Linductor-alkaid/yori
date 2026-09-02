# 测试入口（M0-02）：最小 unit 用例验证 ctest 通路；
# 完整标签体系与显式 skip 示例随 M0-06 落地。

# ---- unit：版本契约 ------------------------------------------------------
add_executable(unit_version_test unit_version_test.cpp)
target_link_libraries(unit_version_test PRIVATE yori::yori_core)
yori_apply_warnings(unit_version_test)
add_test(NAME example.unit.version COMMAND unit_version_test)
set_tests_properties(example.unit.version PROPERTIES LABELS "unit" TIMEOUT 60)
