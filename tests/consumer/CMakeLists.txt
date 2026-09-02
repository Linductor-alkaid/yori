cmake_minimum_required(VERSION 3.25)
project(yori_minimal_consumer LANGUAGES CXX)

# 安装后最小 consumer（M0-05 验收）：只依赖安装产物中的 yori-config.cmake，
# 不引用源码树，用于验证公共头与导出目标（yori::yori_core）的正确性。
find_package(yori 0.1 REQUIRED CONFIG)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE yori::yori_core)
target_compile_features(consumer PRIVATE cxx_std_20)
