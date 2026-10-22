# WCAS 文档

## 本仓库文档

| 文件                                   | 内容                                                                                             |
| -------------------------------------- | ------------------------------------------------------------------------------------------------ |
| [INTERFACE.md](INTERFACE.md)           | Interface 架构、契约地图、迁移状态与 provider 接入规则（2026-10 冻结，约 90%）                   |
| [CAPTURE.md](CAPTURE.md)               | DMA-BUF 采集：后端选择（Vulkan / EGL+GBM）、能力协商、合成器回退与计时                           |
| [COMPATIBILITY.md](COMPATIBILITY.md)   | compositor / toolkit 兼容矩阵                                                                    |
| [FRAMES.md](FRAMES.md)                 | FrameSink / FrameBus：push 式帧消费、线程模型、背压策略与帧生命周期                              |
| [CONTROLLER.md](CONTROLLER.md)         | 非侵入式 Controller 接口：为何选用 ABIX Interface、设备能力拆分、`DeviceInfo`、新增手柄/键盘后端 |
| [MAA-COMPARISON.md](MAA-COMPARISON.md) | WCAS 与 MAA 现有 Linux 控制器（`MaaLinuxControllerCreate`）的差异、优势与局限                    |
| [Proton.md](Proton.md)                 | Proton/Wine 游戏自动化集成指南                                                                   |
| [SECURITY.md](SECURITY.md)             | 安全边界与报告方式                                                                               |
