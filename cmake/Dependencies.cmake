# pinned 依赖校验（M0-03；工程规范 9.1/10.7、依赖管理策略第 2 节）。
#
# configure 阶段比对 third_party/<dep> 的 HEAD 与仓库根 dependencies.lock.json
# 登记的 commit，不匹配即 FATAL_ERROR；同时校验许可文件存在。
#
# YORI_FETCH_DEPENDENCIES 语义：
#   ON  —— 依赖目录缺失时允许执行 `git submodule update --init <path>` 后再校验；
#   OFF —— 不执行任何拉取，仅做一致性校验（离线/CI 默认路径）；缺失即失败。

# 依赖校验以 include 时的源码根为准（而非 CMAKE_SOURCE_DIR，允许未来作为子项目构建）。
set(YORI_DEPS_SOURCE_DIR "${PROJECT_SOURCE_DIR}")

find_program(YORI_GIT_EXECUTABLE git REQUIRED)

# 读取依赖目录 HEAD commit；成功时 ok_var 为 TRUE、out_var 为 40 位 commit。
function(yori_dep_head dir out_var ok_var)
  set(ok FALSE)
  set(head "")
  if(EXISTS "${dir}")
    execute_process(
      COMMAND "${YORI_GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
      RESULT_VARIABLE rc
      OUTPUT_VARIABLE raw_head
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(rc EQUAL 0 AND raw_head MATCHES "^[0-9a-fA-F]+$")
      string(LENGTH "${raw_head}" head_length)
      if(head_length EQUAL 40)
        set(ok TRUE)
        set(head "${raw_head}")
      endif()
    endif()
  endif()
  set(${out_var} "${head}" PARENT_SCOPE)
  set(${ok_var} "${ok}" PARENT_SCOPE)
endfunction()

function(yori_validate_pinned_dependencies)
  set(lock_file "${YORI_DEPS_SOURCE_DIR}/dependencies.lock.json")
  if(NOT EXISTS "${lock_file}")
    message(FATAL_ERROR "缺少 ${lock_file}：pinned 依赖必须以锁文件登记（工程规范 10.7）")
  endif()

  file(READ "${lock_file}" lock_json)
  string(JSON dep_count ERROR_VARIABLE json_error LENGTH "${lock_json}" "dependencies")
  if(json_error)
    message(FATAL_ERROR "解析 ${lock_file} 失败：${json_error}")
  endif()
  if(dep_count LESS 1)
    message(FATAL_ERROR "${lock_file} 未登记任何依赖")
  endif()

  math(EXPR last "${dep_count} - 1")
  foreach(i RANGE ${last})
    string(JSON name GET "${lock_json}" "dependencies" ${i} "name")
    string(JSON path GET "${lock_json}" "dependencies" ${i} "path")
    string(JSON expected_commit GET "${lock_json}" "dependencies" ${i} "commit")
    string(JSON version GET "${lock_json}" "dependencies" ${i} "version")
    string(JSON license GET "${lock_json}" "dependencies" ${i} "license")
    string(JSON license_file GET "${lock_json}" "dependencies" ${i} "license_file")

    set(dep_dir "${YORI_DEPS_SOURCE_DIR}/${path}")
    yori_dep_head("${dep_dir}" actual_head head_ok)

    if(NOT head_ok)
      if(YORI_FETCH_DEPENDENCIES)
        message(STATUS "依赖 ${name}（${path}）未检出，尝试 git submodule update --init")
        execute_process(
          COMMAND "${YORI_GIT_EXECUTABLE}" submodule update --init -- "${path}"
          WORKING_DIRECTORY "${YORI_DEPS_SOURCE_DIR}"
          RESULT_VARIABLE init_result)
        if(NOT init_result EQUAL 0)
          message(FATAL_ERROR
            "初始化 submodule ${path} 失败（exit ${init_result}）；"
            "请手动执行 git submodule update --init --recursive 后重新 configure")
        endif()
        yori_dep_head("${dep_dir}" actual_head head_ok)
      endif()
    endif()

    if(NOT head_ok)
      message(FATAL_ERROR
        "依赖 ${name}（${path}）未检出，且当前为只校验路径"
        "（YORI_FETCH_DEPENDENCIES=OFF）或初始化失败；"
        "请执行 git submodule update --init --recursive 后重新 configure")
    endif()

    string(TOLOWER "${expected_commit}" expected_lower)
    string(TOLOWER "${actual_head}" actual_lower)
    if(NOT expected_lower STREQUAL actual_lower)
      message(FATAL_ERROR
        "pinned 依赖校验失败：${path} 的 HEAD 与 dependencies.lock.json 不一致\n"
        "  期望（lock）：     ${expected_commit}（${name} ${version}）\n"
        "  实际（submodule）：${actual_head}\n"
        "修复方式：\n"
        "  - 非有意变更：git submodule update --init --recursive\n"
        "  - 计划升级依赖：按 docs/supply-chain/dependency-policy.md 第 4 节"
        " 走独立 MR 同步更新 submodule 与 lock 文件")
    endif()

    if(NOT EXISTS "${YORI_DEPS_SOURCE_DIR}/${license_file}")
      message(FATAL_ERROR
        "依赖 ${name} 的许可文件 ${license_file} 不存在；"
        "许可证信息缺失属于供应链校验失败（依赖管理策略第 2 节）")
    endif()

    string(SUBSTRING "${actual_head}" 0 12 head_short)
    message(STATUS "依赖校验：${name} ${version} @ ${head_short}（${license}）与锁文件一致")
  endforeach()
endfunction()

# 将已校验的 pinned Executor 作为 Yori 私有构建依赖接入。M1 当前只需要普通
# finite-task facade，因此关闭上游测试、示例和 GPU 后端；后续工作项需要新能力时，
# 必须先按 pinned 集成指南复核并在这里显式开启。
function(yori_add_pinned_dependencies)
  set(EXECUTOR_BUILD_TESTS OFF CACHE BOOL "Yori 不构建 Executor 自身测试" FORCE)
  set(EXECUTOR_BUILD_EXAMPLES OFF CACHE BOOL "Yori 不构建 Executor 示例" FORCE)
  set(EXECUTOR_BUILD_SHARED OFF CACHE BOOL "Yori 静态链接 pinned Executor" FORCE)
  set(EXECUTOR_ENABLE_GPU OFF CACHE BOOL "M1 不使用 Executor GPU 后端" FORCE)
  set(EXECUTOR_ENABLE_CUDA OFF CACHE BOOL "M1 不使用 Executor CUDA 后端" FORCE)
  set(EXECUTOR_ENABLE_OPENCL OFF CACHE BOOL "M1 不使用 Executor OpenCL 后端" FORCE)

  add_subdirectory(
    "${YORI_DEPS_SOURCE_DIR}/third_party/executor"
    "${PROJECT_BINARY_DIR}/third_party/executor"
    EXCLUDE_FROM_ALL)

  # 上游 facade 作为第三方系统头消费；Yori 自研目标继续使用 -Werror，但不把
  # pinned 依赖头中的诊断归属为 Yori 错误。Executor 自身仍按其构建规则编译。
  set_target_properties(executor PROPERTIES SYSTEM TRUE)
endfunction()
