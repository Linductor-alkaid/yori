#pragma once

// 占位头：GpuProvider 资源侧接口（发现、遥测、外部占用检测、GPU 逻辑状态
// FREE/ALLOCATED/EXTERNAL_BUSY/UNAVAILABLE 的观测事实）。M1 提供进程内伪实现，
// M3 接入 NVML 适配（设计第 7 节）。
