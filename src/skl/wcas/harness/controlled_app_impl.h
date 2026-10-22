#pragma once

#include "skl/wcas/wcas.h"
#include "harness/capture_source.h"
#include "harness/focus_backend.h"
#include "harness/frame_bus.h"
#include "harness/health_monitor.h"
#include "harness/target_selector.h"
#include "harness/process_host.h"
#include "harness/session_context.h"
#include "harness/window_source.h"
#include "xwayland.h"

#include "proxy/conn.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

namespace skl::wcas {

/// Shared out-of-class definition of `ControlledApp::Impl`. It lives in a
/// header because `ComputerUse::launch` (a friend) constructs the app while
/// `ControlledApp`'s own methods live in a separate translation unit.
struct ControlledApp::Impl : SessionContext {
    ~Impl();

    /// Owning process/group host; SessionContext::child/process_group mirror its
    /// ids for existing session/window code.
    std::unique_ptr<ProcessHost> process;
    /// Capture interface runtime. Created by the ControlledApp constructor; it
    /// reads `this` for session/backend/transport changes.
    std::unique_ptr<CaptureRuntime> capture;
    /// Window enumeration / geometry resolver runtime.
    std::unique_ptr<WindowRuntime> windows;
    /// Activation/focus policy runtime.
    std::unique_ptr<FocusRuntime> focus;
    /// Process/session health runtime.
    std::unique_ptr<HealthRuntime> health;
    /// Persistent window/surface selection.
    std::unique_ptr<TargetRuntime> target;
    /// Push fan-out for attached frame sinks.
    std::unique_ptr<FrameBus> frames;

    /// Serializes capture-path state (session selection, backend, target).
    mutable std::recursive_mutex capture_mutex;
    /// Background capture pump state. Kept separate from capture_mutex so
    /// stop_frame_pump() can join without holding the capture lock.
    std::mutex pump_mutex;
    std::condition_variable pump_cv;
    std::thread pump_thread;
    std::chrono::milliseconds pump_interval{16};
    bool pump_running = false;
    bool pump_stop = false;
};

}   // namespace skl::wcas
