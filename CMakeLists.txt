cmake_minimum_required(VERSION 3.25)

project(yori
  VERSION 0.1.0
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

# ---- 编译警告政策 --------------------------------------------------------
include(cmake/CompilerWarnings.cmake)

add_subdirectory(src)
add_subdirectory(apps)

if(YORI_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(tests)
endif()
