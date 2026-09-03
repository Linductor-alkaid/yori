# 供应链报告骨架（依赖管理策略第 6 节）：从 dependencies.lock.json 再生成
# 许可证清单与 SPDX SBOM 骨架。产物只写入构建树 supply_chain/，不进入源码树；
# M0 为骨架目标，M7 发布前以发布配置再生成并存档证据。
function(yori_add_supply_chain_report_target)
  add_custom_target(yori_supply_chain_report
    COMMAND "${CMAKE_COMMAND}"
      "-DLOCK_FILE=${YORI_DEPS_SOURCE_DIR}/dependencies.lock.json"
      "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/supply_chain"
      "-P" "${YORI_DEPS_SOURCE_DIR}/cmake/SupplyChainReport.cmake"
    BYPRODUCTS
      "${CMAKE_BINARY_DIR}/supply_chain/license-manifest.md"
      "${CMAKE_BINARY_DIR}/supply_chain/sbom.spdx.json"
    COMMENT "生成供应链报告骨架（license-manifest.md、sbom.spdx.json）"
    VERBATIM)
endfunction()
