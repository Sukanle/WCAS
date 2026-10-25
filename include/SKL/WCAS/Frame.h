#ifndef SKL_WCAS_FRAME_H
#define SKL_WCAS_FRAME_H

#include "SKL/WCAS/API.h"
#include "SKL/WCAS/Types.h"

#include "SKL/ABIX/Interface/Declaration.h"

#include <stdint.h>

#include <string>

namespace skl::wcas {

/// Pixel layout of a published frame. WCAS capture currently produces BGR
/// 8-bit, row-major images.
enum class FrameFormat : uint32_t {
    Bgr8 = 1,
    Rgba8 = 2,
    Gray8 = 3,
};

enum class SinkStatus : uint32_t {
    Accepted = 0,
    Dropped = 1,
    Busy = 2,
    Stop = 3,
};

enum class SinkOverflow : uint32_t {
    /// Keep only the newest frame; the sink's worker overwrites older frames.
    LatestOnly = 0,
    DropOldest = 1,
    DropNewest = 2,
    BlockWithTimeout = 3,
};

enum class SinkExecution : uint32_t {
    /// One worker thread per sink: strongest isolation.
    DedicatedWorker = 0,
    /// Reserved for a future shared fast dispatcher. The current bus uses a
    /// dedicated worker for every sink.
    SharedDispatcher = 1,
};

struct SKL_WCAS_API SinkPolicy {
    SinkOverflow overflow = SinkOverflow::LatestOnly;
    SinkExecution execution = SinkExecution::DedicatedWorker;
    /// Queue capacity for DropOldest/DropNewest/BlockWithTimeout. Ignored by
    /// LatestOnly.
    uint32_t capacity = 1;
    /// Maximum wait for BlockWithTimeout, in milliseconds.
    uint32_t timeout_ms = 0;
};

struct SKL_WCAS_API SinkStats {
    uint64_t received = 0;
    uint64_t delivered = 0;
    uint64_t dropped = 0;
    uint64_t busy = 0;
    uint32_t queue_depth = 0;
};

/// A borrowed view of one frame. It is valid only for the duration of
/// `FrameSink::on_frame`. If a sink needs to keep it longer, it must call
/// `retain(owner)` before returning and `release(owner)` when done.
struct SKL_WCAS_API FrameView {
    const uint8_t *data = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    uint32_t format = static_cast<uint32_t>(FrameFormat::Bgr8);
    uint64_t sequence = 0;
    int64_t timestamp_ns = 0;
    void *owner = nullptr;
    void (*retain)(void *) = nullptr;
    void (*release)(void *) = nullptr;
};

/// Platform-neutral frame descriptor spelling; FrameView is the current POD
/// representation (including retain/release handles).
using FrameDescriptor = FrameView;

struct SKL_WCAS_API FrameState {
    uint64_t sequence = 0;
    uint64_t dropped = 0;
    int32_t width = 0;
    int32_t height = 0;
};

// FrameSink is a push consumer. The capture thread only publishes into the
// bus; sink callbacks run on the sink's worker, never on the Wayland proxy
// thread. `on_frame` receives a borrowed view (see FrameView).
#define SKL_WCAS_FRAME_SINK_METHODS(X)                             \
    X(on_frame, SinkStatus, reg_frame::status, {reg_frame::view})  \
    X(on_state, SinkStatus, reg_frame::status, {reg_frame::state}) \
    X(on_error, SinkStatus, reg_frame::status, {i32, str})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    FrameSink, SKL_WCAS_FRAME_SINK_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_FRAME_SINK_METHODS

}   // namespace skl::wcas

#endif   // SKL_WCAS_FRAME_H
