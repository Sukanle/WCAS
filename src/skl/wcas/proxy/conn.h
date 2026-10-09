#pragma once

#include "interfaces.h"


#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <array>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

namespace skl::wcas::proxy {

enum class Role : uint8_t {
    Generic = 0,
    Registry,
    Surface,
    Shm,
    ShmPool,
    Buffer,
    Callback,
    XdgWmBase,
    XdgSurface,
    XdgToplevel,
    XdgPopup,
    XdgPositioner,
    Seat,
    Pointer,
    Keyboard,
    Touch,
    Output,
    Compositor,
    Subcompositor,
    Region,
    DataDeviceManager,
    DataDevice,
    DataSource,
    DataOffer,
    DmaBuf,
    DmaBufParams,
    DmaBufFeedback,
    Viewporter,
    Viewport,
    Presentation,
    PresentationFeedback,
    FractionalScale,
    FractionalScaleManager,
    CursorShapeManager,
    CursorShapeDevice,
    DecorationManager,
    ToplevelDecoration,
    XdgOutputManager,
    XdgOutput,
    TextInputManager,
    TextInput,
    SinglePixelBufferManager,
    Activation,
    ActivationToken,
    IdleInhibitManager,
    IdleInhibitor,
    PointerConstraints,
    LockedPointer,
    ConfinedPointer,
    RelativePointerManager,
    RelativePointer,
    PointerGestures,
    PointerGestureSwipe,
    PointerGesturePinch,
    PointerGestureHold,
    TabletManager,
    TabletSeat,
    TabletTool,
    Tablet,
    TabletPad,
    InputTimestampsManager,
    InputTimestamps,
};

Role role_from_interface(const std::string &name);

struct ShmPool {
    int fd = -1;
    std::int32_t size = 0;
};

struct ShmBuffer {
    std::uint32_t pool_id = 0;
    std::int32_t offset = 0, width = 0, height = 0, stride = 0;
    std::uint32_t format = 0;
};

struct RetainedShm {
    int fd = -1;
    std::int32_t pool_size = 0, offset = 0, width = 0, height = 0, stride = 0;
    std::uint32_t format = 0;
};

struct DmaBufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    std::uint64_t modifier = 0;
};

struct DmaBufParams {
    std::array<std::optional<DmaBufPlane>, 4> planes{};
    /// Arguments of the asynchronous `create` request. The buffer id is only
    /// delivered later in the `created` event, so the metadata has to be kept
    /// here until then.
    std::int32_t width = 0, height = 0;
    std::uint32_t format = 0, flags = 0;
    bool create_requested = false;
};

struct DmaBufBuffer {
    std::uint32_t params_id = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t flags = 0;
    std::array<std::optional<DmaBufPlane>, 4> planes{};
};

/// A committed DMA-BUF frame, retained independently of the protocol buffer
/// so capture still works after the client destroys the wl_buffer or the
/// buffer params object. The plane FDs are duplicates owned by this struct.
struct RetainedDmaBuf {
    std::array<std::optional<DmaBufPlane>, 4> planes{};
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t format = 0;
};

struct ToplevelState {
    std::uint32_t surface_id = 0;
    std::optional<std::string> app_id;
    std::optional<std::string> title;
};

/// One capturable window of a session (a toplevel, or a bare committed
/// surface). Used by the CLI `list` command and window selection.
struct WindowInfo {
    std::uint32_t surface_id = 0;
    std::optional<std::string> app_id;
    std::optional<std::string> title;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct SurfaceState {
    std::optional<std::uint32_t> pending_buffer;
    std::optional<std::uint32_t> committed_buffer;
    std::optional<std::uint32_t> pending_dmabuf;
    std::optional<std::uint32_t> committed_dmabuf;
    std::optional<RetainedShm> committed_frame;
    std::optional<RetainedDmaBuf> committed_dmabuf_frame;
    std::uint64_t commit_count = 0;
};

struct ConnState {
    std::int64_t owner_pid = -1;
    int client_fd = -1;
    std::atomic<std::uint64_t> commit_count{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> capturable_surface{false};
    /// Set when the public API was asked for a capture backend this build
    /// cannot serve. Capture must then fail instead of silently falling back
    /// to a different backend.
    std::atomic<bool> backend_unsupported{false};
    /// The selected capture backend, mirroring the public `CaptureBackend`
    /// enum order: 0=client_buffer, 1=kms, 2=shm, 3=dmabuf.
    std::atomic<int> capture_backend{0};
    /// When true the event loop runs the periodic background capture
    /// scheduler; callers then read `pre_captured_frame` instead of
    /// blocking on a synchronous capture.
    std::atomic<bool> background_capture{false};

    std::unordered_map<std::uint32_t, std::string> object_roles;
    std::unordered_map<std::uint32_t, Role> object_role_enum;
    std::unordered_map<std::uint32_t, ShmPool> shm_pools;
    std::unordered_map<std::uint32_t, ShmBuffer> shm_buffers;
    std::unordered_map<std::uint32_t, DmaBufParams> dmabuf_params;
    std::unordered_map<std::uint32_t, DmaBufBuffer> dmabuf_buffers;
    std::unordered_map<std::uint32_t, SurfaceState> surfaces;
    /// Input coordinate mapping. A surface's buffer (what capture returns) is
    /// often larger than its surface-local (logical) size under buffer scale or
    /// wp_viewport fractional scaling; wl_pointer coordinates are logical, so
    /// frame pixels must be divided by this scale before injection.
    std::unordered_map<std::uint32_t, std::uint32_t> viewport_to_surface;
    std::unordered_map<std::uint32_t, std::uint32_t> fractional_scale_to_surface;
    std::unordered_map<std::uint32_t, std::pair<std::int32_t, std::int32_t>> surface_logical_size;
    std::unordered_map<std::uint32_t, std::uint32_t> surface_buffer_scale;
    std::unordered_map<std::uint32_t, std::uint32_t> surface_preferred_scale_120;
    std::unordered_map<std::uint32_t, std::uint32_t> xdg_surface_to_surface;
    std::unordered_map<std::uint32_t, std::uint32_t> surface_to_toplevel;
    std::unordered_map<std::uint32_t, ToplevelState> toplevels;
    std::optional<std::string> capture_app_id;
    std::optional<std::string> capture_title;
    /// Explicitly selected window surface (app ID). When set, both capture and
    /// input target it instead of the focus/commit heuristics. Used to bring a
    /// later-started child window (e.g. an in-game overlay) under control.
    std::uint32_t preferred_surface = 0;
    /// Latest frame produced by the background capture scheduler, if any.
    std::optional<cv::Mat> pre_captured_frame;

    // Input tracking
    std::uint32_t pointer_obj = 0;       // App-side wl_pointer object ID
    std::uint32_t keyboard_obj = 0;      // App-side wl_keyboard object ID
    std::uint32_t focused_surface = 0;   // Last pointer-entered surface (app ID space)
    std::uint32_t pointer_focus = 0;     // Surface with injected pointer focus
    std::uint32_t keyboard_focus = 0;    // Surface with injected keyboard focus
    std::atomic<std::uint32_t> next_serial{1};
    /// Number of frames the app presented, counted from `wl_callback.done` for
    /// the `wl_surface.frame` callbacks it requested. This is the strongest
    /// "a new frame is on screen" signal a Wayland client can observe, so
    /// capture can wait for it to advance after injecting input.
    std::atomic<std::uint64_t> presented_frames{0};
    std::unordered_map<std::uint32_t, std::uint32_t> callback_to_surface;
    /// Last pointer position the compositor reported for the app (surface-local
    /// wl_fixed raw bits), tracked from `wl_pointer.enter`/`motion`. This is the
    /// real device position, independent of coordinates wcas injects.
    std::atomic<std::uint32_t> pointer_sx{0};
    std::atomic<std::uint32_t> pointer_sy{0};
    std::atomic<bool> pointer_position_known{false};

    // text-input-v3 tracking (app-side object ID in app ID space)
    std::uint32_t text_input_obj = 0;
    bool text_input_pending_enabled = false;
    bool text_input_enabled = false;
    std::uint32_t text_input_commit_serial = 0;

    // Keymap tracking
    int keymap_fd = -1;
    std::uint32_t keymap_size = 0;

    mutable std::mutex data_mutex;

    ~ConnState();
};

class Connection {
public:
    Connection(int app_fd, int upstream_fd, std::shared_ptr<ConnState> state);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    void run();

private:
    int app_fd_;
    int upstream_fd_;
    std::shared_ptr<ConnState> state_;
};

bool capture_shm(const std::shared_ptr<ConnState> &state, cv::Mat &image);
bool capture_dmabuf(const std::shared_ptr<ConnState> &state, cv::Mat &image);
/// Ratio between a surface's buffer pixels (what capture returns) and its
/// surface-local (logical) pixels used by wl_pointer. 1.0 when unknown.
double input_scale(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id);
void set_capture_target(
    const std::shared_ptr<ConnState> &state, std::optional<std::string> app_id, std::optional<std::string> title);
/// Enumerate the session's capturable windows (toplevels plus bare committed
/// surfaces), newest metadata included. Used by `list` / window selection.
std::vector<WindowInfo> list_windows(const std::shared_ptr<ConnState> &state);

}   // namespace skl::wcas::proxy
