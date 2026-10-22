# FrameSink / FrameBus

`FrameSink` 是 WCAS 的 push 式帧消费者接口。`ControlledApp::capture()` 每产生一帧，`FrameBus` 会把它 fan-out 给所有已 attach 的 sink；sink 回调运行在各自的 worker 线程，不会阻塞 Wayland proxy / capture 线程。

## 1. 使用

```cpp
auto app = harness.launch(config);

auto sink_id = app.attach_frame_sink(sink, SinkPolicy{
    .overflow = SinkOverflow::LatestOnly,
});
if (!sink_id)
    return 1;

// 方式一：每次 capture() 产生帧时自动 publish
auto frame = app.capture();

// 方式二：后台自动采集并 publish
if (auto started = app.start_frame_pump(std::chrono::milliseconds{16}); !started)
    return 1;
app.stop_frame_pump();

auto stats = app.frame_sink_stats(sink_id.value());
app.detach_frame_sink(sink_id.value());
```

`FrameSink` 是 ABIX interface。内置 C++ sink 直接用 registry 绑定：

```cpp
class Recorder {
public:
    SinkStatus on_frame(const FrameView *view) {
        // view 只在本次回调期间有效
        cv::Mat frame(view->height, view->width, CV_8UC3,
            const_cast<uint8_t *>(view->data), view->stride);
        // ...
        return SinkStatus::Accepted;
    }

    SinkStatus on_state(const FrameState *) { return SinkStatus::Accepted; }
    SinkStatus on_error(int32_t, std::string) { return SinkStatus::Accepted; }
};

const InterfaceMethod methods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_frame, &Recorder::on_frame),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_state, &Recorder::on_state),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(FrameSinkTraits, on_error, &Recorder::on_error),
};

InterfaceRegistry<4> registry;
registry.add({FrameSinkTraits::id, recorder_type, methods, 3, FrameSinkTraits::version, 0});

Recorder recorder;
FrameSink sink(registry, &recorder, recorder_type);
app.attach_frame_sink(std::move(sink), policy);
```

## 2. 线程模型

```text
ControlledApp::capture()  (调用方线程)
        │ FrameBus::publish()
        ▼
   per-sink mailbox + worker
        │
        └── FrameSink::on_frame()
```

- capture 线程只做引用计数和 mailbox 投递，不执行 sink 代码；
- 每个 sink 一个 worker，sink 之间互不阻塞；
- `FrameBus` 当前总是使用 dedicated worker；`SinkExecution::SharedDispatcher` 预留给未来共享快速 dispatcher；
- `on_frame` / `on_state` / `on_error` 由该 sink 的 worker 串行调用；
- `start_frame_pump(interval)` 启动后台采集线程，按 interval 调用 capture 并 publish；
- 不要在回调里调用 `capture()`、`detach_frame_sink()` 或 `stop()`，避免重入和关闭竞争。

## 3. 背压策略

| `SinkOverflow` | 行为 | 适用 |
|---|---|---|
| `LatestOnly` | 只保留最新一帧，旧帧丢弃 | 预览、实时推理 |
| `DropOldest` | 队列满时丢最旧 | 录制、推流 |
| `DropNewest` | 队列满时拒绝新帧 | 有积压时先不接 |
| `BlockWithTimeout` | 限时等待队列空间 | 明确允许 capture 短暂等待 |

`capacity` 对应 `DropOldest` / `DropNewest` / `BlockWithTimeout`；`timeout_ms` 对应 `BlockWithTimeout`。`LatestOnly` 忽略 `capacity`。

默认不阻塞 capture；丢帧通过 `SinkStats` 暴露：

```cpp
struct SinkStats {
    uint64_t received;
    uint64_t delivered;
    uint64_t dropped;
    uint64_t busy;
    uint32_t queue_depth;
};
```

## 4. 帧生命周期

`FrameView` 是 borrowed view：

- `data/width/height/stride/format/sequence/timestamp_ns` 只在 `on_frame` 回调期间有效；
- 需要跨回调保存时，在回调返回前调用 `view->retain(view->owner)`，用完调用 `view->release(view->owner)`；
- 不调用 `retain` 的 sink 必须在回调返回前完成拷贝或处理；
- `FrameView` 的 `owner/retain/release` 由 `FrameBus` 提供，模块 sink 不能自行 `free()` 或持有 `cv::Mat`。

## 5. 当前限制

- `on_state` 在帧尺寸变化时发射；`on_error` 在连续 capture 失败时发射一次（错误恢复后重新计数）。
- `FrameBus` 现在有两种驱动方式：调用方循环 `capture()`，或 `start_frame_pump()` 后台自动采集。
- `SinkExecution::SharedDispatcher` 已实现；共享组内串行，慢 sink 会影响同组其他 sink。
- `FrameBus` 预分配 8 个 Frame（`FrameBus` 构造参数可调）；池耗尽时回退到临时 `shared_ptr<Frame>`，保证 capture 不被阻塞，池化会随着 sink 释放自然回收。
- `FrameView` 和 `FrameState` 是进程内 POD；如果 sink 作为模块加载，`FrameView` 的 retain/release 和 module lease 需要一起使用。

## 6. 相关

- 实现：`src/skl/wcas/harness/frame_bus.{h,cpp}`
- 契约：`include/skl/wcas/frame.h`
- 测试：`tests/frame_bus.cpp`
- 计划与冻结状态：[INTERFACE.md](INTERFACE.md)
