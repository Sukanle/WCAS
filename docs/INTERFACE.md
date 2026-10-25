# WCAS Interface 架构与迁移

> 状态：**2026-10-22 暂时冻结，约 90% 完成。** 剩余工作需要在真实 Windows/macOS 环境、module lease 或第三方 transport 需求出现时再继续。迁移状态与验证记录见 [STATUS.md](STATUS.md)。

## 为什么使用 ABIX Interface

WCAS 的后端不是一棵继承树：capture、输入、窗口、传输和帧消费都可能被替换，甚至由独立模块提供。ABIX Interface 提供了：

- **稳定契约**：接口 id 由名称、版本和有序方法签名计算；
- **非侵入 provider**：普通 C++ struct，不继承接口、不需要虚函数；
- **绑定期解析**：绑定一次，热路径只调用缓存的函数指针；
- **显式失败**：`interface_not_found` / `version_mismatch` / `method_not_found` / `signature_mismatch`，而不是空指针或静默回退；
- **组合优先**：按能力拆成小接口，而不是一个不断变胖的 `Backend`。

## 契约地图

> 公共头根目录为 `include/SKL/WCAS/`；私有头与实现分别位于 `include/SKL/WCAS/Private/` 和 `src/SKL/WCAS/Private/`。表中 `{h,cpp}` 表示同名头文件与源文件。

| 能力               | 契约/实现                                | 说明                                                                                  |
| ------------------ | ---------------------------------------- | ------------------------------------------------------------------------------------- |
| 输入设备           | `include/SKL/WCAS/Controller.h`          | `DeviceInfo` / `PointerController` / `KeyboardController` / `GamepadController`       |
| 输入 provider      | `Private/Harness/Controller.cpp`         | native 私有 seat；XWayland 选择 `X11InputDevice`                                      |
| X11 输入           | `Private/Harness/X11Input.{h,cpp}`       | Xlib + XTest；实现现有 Controller 能力                                                |
| 截图               | `include/SKL/WCAS/Capture.h`             | `CaptureInfo` / `CaptureSource`                                                       |
| 截图 provider      | `Private/Harness/CaptureSource.{h,cpp}`  | AutoCaptureSource / shm / DMA-BUF / KMS / grim / X11                                  |
| DMA-BUF 解码       | `Private/Proxy/DMABUF/Decoder.h`         | `DmaBufDecoder` + Auto/Vulkan/EGL/Mmap                                                |
| 推帧               | `include/SKL/WCAS/Frame.h`               | `FrameSink` / `FrameView` / `SinkPolicy`                                              |
| 推帧调度           | `Private/Harness/FrameBus.{h,cpp}`       | per-sink mailbox、worker/shared dispatcher、背压                                      |
| 窗口枚举/几何      | `include/SKL/WCAS/Window.h`              | `WindowEnumerator` / `WindowResolver`                                                 |
| 窗口 runtime       | `Private/Harness/WindowSource.{h,cpp}`   | native proxy / X11 枚举，geometry 回退                                                |
| 传输描述           | `include/SKL/WCAS/Transport.h`           | `TransportKind` / `TransportCapability` / `TargetId` / `LaunchSpec` / `SessionHandle` |
| `TransportBackend` | `include/SKL/WCAS/Transport.h`           | 预留描述接口，无生产 provider                                                         |
| 进程               | `Private/Harness/ProcessHost.{h,cpp}`    | fork/exec、进程组、PDEATHSIG、terminate                                               |
| 会话上下文         | `Private/Harness/SessionContext.h`       | capture/window/input 共用的 transport/session 状态                                    |
| 焦点               | `Private/Harness/FocusBackend.{h,cpp}`   | `FocusBackend`：available/set_allowed/allowed/apply                                   |
| 健康               | `Private/Harness/HealthMonitor.{h,cpp}`  | alive/connected/commit/presented                                                      |
| 目标选择           | `Private/Harness/TargetSelector.{h,cpp}` | index 与 `TargetId` 选择                                                              |
| 启动策略           | `Private/Harness/LaunchStrategy.{h,cpp}` | `LaunchStrategy` 描述/排序；native → XWayland                                         |
| 专题文档：         |

- [CONTROLLER.md](CONTROLLER.md)：Controller/ABIX 选型与成员适配器
- [FRAMES.md](FRAMES.md)：FrameSink/FrameBus 线程与背压
- [CAPTURE.md](CAPTURE.md)：DMA-BUF 解码与回退

## 设计规则

1. **能力拆分，不做大接口** capture、window、input、focus、health、target 各自独立；`TransportBackend` 只描述 transport，不包含数据面方法。

2. **provider 是普通 struct** 方法表用 `SKL_ABIX_INTERFACE_MEMBER_ENTRY`；不要求继承或虚函数。

3. **slot 顺序是 ABI** 只追加、不改序；新增能力用新接口或新版本。接口 id 变化会让旧 provider 失配。

4. **Unique vs Shared** 一个 app/对象一个的代理用 `UniqueInterfaceProxy`；大量 proxy 共享同一实现时才用 `SharedInterfaceProxy` + `InterfaceSlotTable`。

5. **registry 只用于绑定** `InterfaceRegistry` 一次填充后只读；并发 `find()` 安全，`add()` 必须由调用方串行化。动态 provider 使用 `ModuleRegistry` + module lease。

6. **显式失败** 能力缺失返回 `errc::feature_not_implemented`；不静默切换实现。

7. **组合优先** `ControlledApp` 是 facade；内部用 `SessionContext` + 各 capability runtime 组合， transport 差异由 provider 吸收。

## 能力边界

传输后端不是一个接口，而是若干互相独立的能力。边界如下：

| 能力                                  | 只负责                                                      | 明确不负责                      |
| ------------------------------------- | ----------------------------------------------------------- | ------------------------------- |
| `ProcessHost`                         | fork/exec、env/args/cwd、进程组、PDEATHSIG、终止、pid/alive | session、capture、window、input |
| `TransportSession`                    | 建立/接管 display，连接状态，关闭，创建能力 provider        | 截图像素、窗口语义、输入动作    |
| `CaptureSource`                       | 产生帧、target、frame_size、背景截图                        | 进程生命周期、窗口列表、焦点    |
| `WindowEnumerator` / `WindowResolver` | 枚举窗口/表面；pid 或 size/title 到 rect                    | 修改 selection、激活窗口、截图  |
| `Controller`                          | action 级 pointer/keyboard/gamepad 注入                     | capture、window、process        |
| `FocusBackend`                        | 激活/聚焦窗口，允许/禁止 activation                         | 输入事件、窗口枚举              |
| `TargetSelector`                      | 当前选中 target 的持久状态                                  | 枚举窗口、焦点、截图实现        |
| `HealthMonitor`                       | 进程/session 存活、commit/presented 等只读指标              | 执行 capture、修改 selection    |
| `SessionInventory`                    | sessions/surfaces 诊断视图                                  | 控制路径依赖                    |

### native 与 XWayland 能力矩阵

| 能力               | native Wayland                   | XWayland                    |
| ------------------ | -------------------------------- | --------------------------- |
| ProcessHost        | 有                               | 有                          |
| TransportSession   | 私有 Wayland socket + proxy      | Xwayland bridge + X display |
| CaptureSource      | shm / DMA-BUF / KMS / grim       | XComposite / XGetImage      |
| WindowEnumerator   | proxy session 窗口/表面          | X11 root/top-level          |
| WindowResolver     | grim / Sway / Hyprland / xdotool | X11 几何                    |
| Controller         | 私有 seat                        | XTest 输入                  |
| FocusBackend       | `xdg_activation_v1`              | 当前 `available() == false` |
| HealthMonitor      | commit / presented frames        | 进程 + 内容检测             |
| Background capture | DMA-BUF 调度器                   | 无                          |
| TargetSelector     | surface id                       | window id（`TargetId`）     |

不可用能力返回 `errc::feature_not_implemented`，不静默换实现。

### 反模式

- **大接口**：`TransportBackend` 同时包含 capture/window/input/health，导致每个新 transport 都被迫实现一堆空方法；
- **能力泄漏**：在 `WindowEnumerator` 里做 selection/activation，或在 `CaptureSource` 里读进程/焦点状态；
- **transport 分支扩散**：每个方法都写 `if (xwayland)`，而不是在 provider 选择阶段决定；
- **静默回退**：XWayland 没有输入却偷偷复用 native session；
- **生命周期倒置**：capture/window 持有整个 app 实现并读取全部字段，现在改为持有 `SessionContext`；
- **把诊断当控制**：`sessions()` / `surfaces()` 被控制路径依赖。

## 扩展性评估

| 场景                       | 改造量 | 主要工作                                              | 阻塞                   |
| -------------------------- | ------ | ----------------------------------------------------- | ---------------------- |
| gamescope / wlr-screencopy | 低-中  | 新增 `CaptureSource`，可能新增 `WindowEnumerator`     | 无结构性问题           |
| PipeWire portal            | 中     | 新增 `CaptureSource` + session/授权                   | portal 生命周期        |
| X11 输入/焦点              | 中     | `XTest` 输入已就绪；`_NET_ACTIVE_WINDOW` FocusBackend | X11 目标捕获           |
| 远程桌面（RDP/VNC）        | 中-高  | CaptureSource + Input + Session                       | 网络/编码/lease        |
| Windows                    | 高     | ProcessHost、CaptureSource、Input、Focus provider     | POSIX/Wayland 类型泄漏 |
| macOS                      | 高     | ProcessHost、ScreenCaptureKit、CGEvent、Accessibility | 同上                   |
| 第三方 transport 模块      | 高     | ABIX `TransportBackend` 工厂 + module lease           | 需要稳定 POD 契约      |

当前架构对“新增同类传输后端”已经足够好；对“不同平台”是正确方向但还不是开箱可移植。主要缺口是 **target / process / session / frame 契约的中性化**，而不是组合模型本身。

平台中性化需要处理的点：

1. `Transport` 枚举改为可扩展的 `TransportKind` + capability 查询；
2. `ControlledApp::Impl*` 依赖改为 `SessionContext`（已落地）；
3. `surface_id` 假设改为 opaque `TargetId`（已落地）；
4. `pid_t` / 进程组 / 信号收敛到 `ProcessHost` provider 内部（已落地）；
5. `cv::Mat` / `std::string` 收敛为 POD `FrameView` + owner handle（`FrameView` 已落地，跨模块版本仍需 owner handle）；
6. 平台相关枚举收敛为 capability bits。

## 迁移状态

### 已完成

- Controller 设备能力拆分
- CaptureSource / AutoCaptureSource
- DmaBufDecoder + Auto/Vulkan/EGL/Mmap
- FrameSink / FrameBus / FramePool / SharedDispatcher
- WindowEnumerator / WindowResolver
- ProcessHost / SessionContext
- FocusBackend / HealthMonitor / TargetSelector
- X11 XTest 输入（Controller provider）
- `TargetId` / `LaunchSpec` / `SessionHandle` / `FrameDescriptor`
- `TransportBackend` 预留描述接口
- `LaunchStrategy` 描述/排序并接入 `ComputerUse::launch()`

### 未来计划

1. X11 `TargetId` capture 支持（当前 X11 target 返回 `feature_not_implemented`）。
2. `TransportBackend::create(LaunchSpec)` 工厂、POD session handle、ownership/lease。
3. 模块化 FrameSink/provider 的 module lease。
4. Windows/macOS ProcessHost、CaptureSource、Input、Focus provider。
5. FramePool 容量调优、SharedDispatcher 分组策略。
6. ProtocolObserver（仅低频生命周期事件）。

冻结原则：已有契约保持稳定；未来需求优先新增 provider/能力，不修改既有 slot 顺序。

## 添加一个新 provider

1. 选择已有契约（不要为单一实现新增接口）；
2. 写普通 struct 和成员适配器；
3. 用 `SKL_ABIX_INTERFACE_MEMBER_ENTRY` 建方法表；
4. 在对应 runtime 的 registry 中注册 `(TypeId, InterfaceTraits::id)`；
5. 绑定并检查 `valid()`；
6. 添加 fake provider 单测 + 真实 provider 的集成验证（如环境允许）。

## 测试

当前 CTest 覆盖：

- `wcas-controller-interface`
- `wcas-capture-interface`
- `wcas-dmabuf-decoder-interface`
- `wcas-frame-bus` / `wcas-frame-pump`
- `wcas-window-interface`
- `wcas-transport-interface`
- `wcas-x11-input`
- `wcas-launch-strategy`
- `wcas-process-host`
- `wcas-focus-backend` / `wcas-health-monitor` / `wcas-target-selector`

实际 X11/Vulkan/EGL 路径依赖运行环境；无显示/无 GPU 时相关集成测试会跳过。
