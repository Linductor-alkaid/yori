# yorid：全局唯一 authoritative scheduler（设计第 3 节）。M0 为可编译骨架，
# daemon 生命周期与 Executor owner 语义自 M1 起落地。
add_executable(yorid main.cpp)
target_link_libraries(yorid PRIVATE yori::yori_core)
yori_apply_warnings(yorid)

install(TARGETS yorid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
