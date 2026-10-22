# WCAS Interface 架构与迁移

> 状态：**2026-10-22 暂时冻结，约 90% 完成。**
> 剩余工作需要在真实 Windows/macOS 环境、module lease 或第三方 transport 需求出现时再继续。
> 详细历史分析见 `memory/interface_plan.md`（内部记忆，非公开文档）。

## 为什么使用 ABIX Interface

WCAS 的后端不是一棵继承树：capture、输入、窗口、传输和帧消费都可能被替换，甚至由独立
模块提供。ABIX Interface 提供了：

- **稳定契约**：接口 id 由名称、版本和有序方法签名计算；
- **非侵入 provider**：普通 C++ struct，不继承接口、不需要虚函数；
- **绑定期解析**：绑定一次，热路径只调用缓存的函数指针；
- **显式失败**：`interface_not_found` / `version_mismatch` / `method_not_found` /
  `signature_mismatch`，而不是空指针或静默回退；
- **组合优先**：按能力拆成小接口，而不是一个不断变胖的 `Backend`。

## 契约地图

| 能力 | 契约/实现 | 说明 |
|---|---|---|
| 输入设备 | `include/skl/wcas/controller.h` | `DeviceInfo` / `PointerController` / `KeyboardController` / `GamepadController` |
| 输入 provider | `src/skl/wcas/harness/controller.cpp` | native 私有 seat；XWayland 选择 `X11InputDevice` |
| X11 输入 | `src/skl/wcas/harness/x11_input.{h,cpp}` | Xlib + XTest；实现现有 Controller 能力 |
| 截图 | `include/skl/wcas/capture.h` | `CaptureInfo` / `CaptureSource` |
| 截图 provider | `src/skl/wcas/harness/capture_source.{h,cpp}` | AutoCaptureSource / shm / DMA-BUF / KMS / grim / X11 |
| DMA-BUF 解码 | `src/skl/wcas/proxy/dmabuf_decoder.h` | `DmaBufDecoder` + Auto/Vulkan/EGL/Mmap |
| 推帧 | `include/skl/wcas/frame.h` | `FrameSink` / `FrameView` / `SinkPolicy` |
| 推帧调度 | `src/skl/wcas/harness/frame_bus.{h,cpp}` | per-sink mailbox、worker/shared dispatcher、背压 |
| 窗口枚举/几何 | `include/skl/wcas/window.h` | `WindowEnumerator` / `WindowResolver` |
| 窗口 runtime | `src/skl/wcas/harness/window_source.{h,cpp}` | native proxy / X11 枚举，geometry 回退 |
| 传输描述 | `include/skl/wcas/transport.h` | `TransportKind` / `TransportCapability` / `TargetId` / `LaunchSpec` / `SessionHandle` |
| `TransportBackend` | `include/skl/wcas/transport.h` | 预留描述接口，无生产 provider |
| 进程 | `src/skl/wcas/harness/process_host.{h,cpp}` | fork/exec、进程组、PDEATHSIG、terminate |
| 会话上下文 | `src/skl/wcas/harness/session_context.h` | capture/window/input 共用的 transport/session 状态 |
| 焦点 | `src/skl/wcas/harness/focus_backend.{h,cpp}` | `FocusBackend`：available/set_allowed/allowed/apply |
| 健康 | `src/skl/wcas/harness/health_monitor.{h,cpp}` | alive/connected/commit/presented |
| 目标选择 | `src/skl/wcas/harness/target_selector.{h,cpp}` | index 与 `TargetId` 选择 |
| 启动策略 | `src/skl/wcas/harness/launch_strategy.{h,cpp}` | `LaunchStrategy` 描述/排序；native → XWayland |

专题文档：

- [CONTROLLER.md](CONTROLLER.md)：Controller/ABIX 选型与成员适配器
- [FRAMES.md](FRAMES.md)：FrameSink/FrameBus 线程与背压
- [CAPTURE.md](CAPTURE.md)：DMA-BUF 解码与回退

## 设计规则

1. **能力拆分，不做大接口**  
   capture、window、input、focus、health、target 各自独立；`TransportBackend` 只描述
   transport，不包含数据面方法。

2. **provider 是普通 struct**  
   方法表用 `SKL_ABIX_INTERFACE_MEMBER_ENTRY`；不要求继承或虚函数。

3. **slot 顺序是 ABI**  
   只追加、不改序；新增能力用新接口或新版本。接口 id 变化会让旧 provider 失配。

4. **Unique vs Shared**  
   一个 app/对象一个的代理用 `UniqueInterfaceProxy`；大量 proxy 共享同一实现时才用
   `SharedInterfaceProxy` + `InterfaceSlotTable`。

5. **registry 只用于绑定**  
   `InterfaceRegistry` 一次填充后只读；并发 `find()` 安全，`add()` 必须由调用方串行化。
   动态 provider 使用 `ModuleRegistry` + module lease。

6. **显式失败**  
   能力缺失返回 `errc::feature_not_implemented`；不静默切换实现。

7. **组合优先**  
   `ControlledApp` 是 facade；内部用 `SessionContext` + 各 capability runtime 组合，
   transport 差异由 provider 吸收。

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
