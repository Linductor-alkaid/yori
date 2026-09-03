# yori_core：Core 静态库。M0 仅承载版本信息；
# M1 起填充 JobSpec、状态机、全局队列与调度契约（设计第 4 节）。
configure_file(
  "${PROJECT_SOURCE_DIR}/include/yori/version.h.in"
  "${PROJECT_BINARY_DIR}/generated/yori/version.h"
  @ONLY)

add_library(yori_core version.cpp)
add_library(yori::yori_core ALIAS yori_core)

target_include_directories(yori_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

yori_apply_warnings(yori_core)

set_target_properties(yori_core PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

install(TARGETS yori_core EXPORT yori-export
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
