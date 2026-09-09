# yori CLI：无状态客户端（设计第 3 节）。M5 起接入 submit/ps/queue/gpu/
# cancel/logs 快照命令；不创建 Executor。
add_executable(yori main.cpp)
target_link_libraries(yori PRIVATE yori::yori_core yori::ipc)
yori_apply_warnings(yori)

install(TARGETS yori RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
