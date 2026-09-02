# yori CLI：无状态客户端（设计第 3 节）。M0 为可编译骨架，命令集随 M5/M6 接入。
add_executable(yori main.cpp)
target_link_libraries(yori PRIVATE yori::yori_core)
yori_apply_warnings(yori)

install(TARGETS yori RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
