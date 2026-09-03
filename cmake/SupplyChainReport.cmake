# script 模式：读取 dependencies.lock.json，在构建树内生成许可证清单与
# SPDX SBOM 骨架（M0 骨架；M7 发布前以发布配置再生成并定稿格式）。
#
# 参数：LOCK_FILE、OUTPUT_DIR（见 cmake/SupplyChain.cmake）。

if(NOT DEFINED LOCK_FILE OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "SupplyChainReport.cmake 需要 -DLOCK_FILE=... -DOUTPUT_DIR=...")
endif()

file(READ "${LOCK_FILE}" lock_json)
string(JSON dep_count ERROR_VARIABLE json_error LENGTH "${lock_json}" "dependencies")
if(json_error)
  message(FATAL_ERROR "解析 ${LOCK_FILE} 失败：${json_error}")
endif()

set(manifest "# 依赖许可证清单（生成物，请勿手改）\n\n")
string(APPEND manifest "来源：dependencies.lock.json；生成命令：cmake --build <build> --target yori_supply_chain_report。\n\n")
string(APPEND manifest "| 依赖 | 版本 | commit | 许可证 | 许可文件 |\n| --- | --- | --- | --- | --- |\n")

set(spdx_packages "")
math(EXPR last "${dep_count} - 1")
foreach(i RANGE ${last})
  string(JSON name GET "${lock_json}" "dependencies" ${i} "name")
  string(JSON version GET "${lock_json}" "dependencies" ${i} "version")
  string(JSON commit GET "${lock_json}" "dependencies" ${i} "commit")
  string(JSON license GET "${lock_json}" "dependencies" ${i} "license")
  string(JSON license_file GET "${lock_json}" "dependencies" ${i} "license_file")

  string(APPEND manifest "| ${name} | ${version} | ${commit} | ${license} | ${license_file} |\n")

  if(i GREATER 0)
    string(APPEND spdx_packages ",")
  endif()
  string(APPEND spdx_packages "\n    {\n"
    "      \"name\": \"${name}\",\n"
    "      \"versionInfo\": \"${version}\",\n"
    "      \"downloadLocation\": \"NOASSERTION\",\n"
    "      \"filesAnalyzed\": false,\n"
    "      \"licenseConcluded\": \"${license}\",\n"
    "      \"licenseDeclared\": \"${license}\",\n"
    "      \"copyrightText\": \"NOASSERTION\",\n"
    "      \"externalRefs\": [{\n"
    "        \"referenceCategory\": \"PERSISTENT-ID\",\n"
    "        \"referenceType\": \"git-commit-id\",\n"
    "        \"referenceLocator\": \"${commit}\"\n"
    "      }]\n"
    "    }")
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/license-manifest.md" "${manifest}")

file(WRITE "${OUTPUT_DIR}/sbom.spdx.json"
  "{\n"
  "  \"spdxVersion\": \"SPDX-2.3\",\n"
  "  \"dataLicense\": \"CC0-1.0\",\n"
  "  \"SPDXID\": \"SPDXRef-DOCUMENT\",\n"
  "  \"name\": \"yori-sbom\",\n"
  "  \"documentNamespace\": \"https://github.com/Linductor-alkaid/yori/sbom/m0-skeleton\",\n"
  "  \"creationInfo\": { \"creators\": [\"Tool: yori_supply_chain_report\"], \"created\": \"\" },\n"
  "  \"packages\": [${spdx_packages}\n  ]\n"
  "}\n")

message(STATUS "供应链报告已生成：${OUTPUT_DIR}/license-manifest.md、${OUTPUT_DIR}/sbom.spdx.json")
