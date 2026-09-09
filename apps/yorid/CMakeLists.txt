# yorid：全局唯一 authoritative scheduler（设计第 3 节）。M5 起为可服务
# daemon：Executor owner（EXEC-01）+ 恢复 + GPU 观察 + IPC（M5 子集总装，
# 调度触发与进程守护随守护总装收口接入）。
add_executable(yorid main.cpp)
target_link_libraries(yorid PRIVATE yori::yori_core yori::runtime yori::sqlite yori::nvml
  executor::executor)
yori_apply_warnings(yorid)

install(TARGETS yorid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
