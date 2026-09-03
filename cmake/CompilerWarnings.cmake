# 编译警告策略（工程规范第 11 节：关键警告按仓库策略设为 error）。
# 以函数形式对目标施加 PRIVATE 警告选项：不进入安装导出，也不泄漏给 consumer。
# Yori 目标平台仅为 Linux（GCC/Clang）；不提供 MSVC 分支。

set(YORI_WARNING_FLAGS
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wformat=2
  -Wcast-qual
  -Wdouble-promotion
  -Wnull-dereference
  -Wnon-virtual-dtor
  -Woverloaded-virtual)

function(yori_apply_warnings target)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Yori 仅支持 GCC/Clang（目标平台 Linux，工程规范第 11 节）")
  endif()
  target_compile_options(${target} PRIVATE ${YORI_WARNING_FLAGS})
  if(YORI_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
