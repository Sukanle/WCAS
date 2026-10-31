#pragma once
#include "SKL/WCAS/API.h"
#include "SKL/WCAS/Controller.h"
#include "SKL/WCAS/Types.h"
#include "SKL/WCAS/Capture.h"
#include "SKL/WCAS/Frame.h"
#include "SKL/WCAS/Transport.h"

#include <stdint.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace skl::wcas {
enum class Transport : uint8_t {
    native_wayland,
    xwayland,
};
enum class GraphicsApi : uint8_t {
    /// Proton/umu default: DXVK translates the game's DirectX calls to Vulkan.
    dxvk,
    /// The game's own native Vulkan renderer: DXVK is disabled so the game
    /// reaches Vulkan directly. Only meaningful for games that ship one.
    vulkan,
    /// WineD3D (OpenGL), selected via PROTON_USE_WINED3D.
    opengl,
};

/// Preferred launch transport. `Auto` keeps the native Wayland proxy first and
/// falls back to XWayland; `XWayland` is useful for Wine/QtWebEngine launchers
/// that render a white, unresponsive window through the Wine Wayland driver.
enum class LaunchTransport : uint8_t {
    Auto = 0,
    NativeWayland = 1,
    XWayland = 2,
};
/// Pointer position reported by the compositor for the app's focused surface
/// (surface-local, logical pixels). This reflects the real input device, not
/// the coordinates wcas injected.
using CursorPosition = cv::Point2d;
/// How a window was discovered. A rendering surface often is not the toplevel
/// itself (games put window content in a subsurface), so this distinguishes a
/// real toplevel from content that merely inherited its metadata.
enum class SKL_WCAS_API WindowOrigin : uint8_t {
    Toplevel = 0,
    /// Content surface whose app_id/title were inherited from an ancestor toplevel.
    Subsurface,
    /// Renders frames but has neither a toplevel nor a known parent.
    Bare,
};
/// A window managed by wcas: the main app window or a later-started child
/// window (overlay, dialog) that opened its own Wayland connection.
struct SKL_WCAS_API WindowInfo {
    std::string app_id;
    std::string title;
    int32_t width = 0;
    int32_t height = 0;
    int32_t pid = 0;
    bool selected = false;
    WindowOrigin origin = WindowOrigin::Bare;
    /// Wayland surface id within its session. Distinct windows can share
    /// app_id/title, so this disambiguates entries in `list`.
    uint32_t surface_id = 0;
    /// Platform-neutral target identity; providers populate the appropriate kind.
    TargetId target{};
};
/// Diagnostic view of every proxy session seen for this launch, including
/// disconnected or unmatched ones (useful when a window seems to disappear).
struct SKL_WCAS_API SessionInfo {
    int32_t pid = 0;
    bool connected = false;
    bool matched = false;
    int32_t windows = 0;
    /// Total wl_surface / xdg_toplevel objects tracked for this session. A
    /// session with surfaces>0 but windows==0 exists but has not rendered yet;
    /// surfaces==0 means no window was ever created on this connection.
    int32_t surfaces = 0;
    int32_t toplevels = 0;
    /// Whether the connection bound wl_keyboard / wl_pointer and received a
    /// keymap. Keyboard injection cannot work without all three.
    bool has_keyboard = false;
    bool has_pointer = false;
    bool has_keymap = false;
    /// Command line + executable of the owning process (diagnostic: tells the
    /// game connection apart from helper/CEF ones).
    std::string command;
};
/// Diagnostic view of one tracked Wayland surface, including surfaces that
/// never became toplevels and never rendered (which `windows()` omits). Used to
/// find a window whose metadata/role has not been established yet.
struct SKL_WCAS_API SurfaceInfo {
    int32_t pid = 0;
    uint32_t surface_id = 0;
    uint32_t parent_surface_id = 0;
    uint32_t toplevel_id = 0;
    std::string role;
    std::string app_id;
    std::string title;
    int32_t width = 0;
    int32_t height = 0;
    bool has_frame = false;
    uint32_t last_commit_seq = 0;
};
class SKL_WCAS_API ControlledApp {
public:
    ControlledApp(ControlledApp &&) noexcept;
    ControlledApp &operator=(ControlledApp &&) noexcept;
    ~ControlledApp();
    ControlledApp(const ControlledApp &) = delete;
    ControlledApp &operator=(const ControlledApp &) = delete;
    [[nodiscard]] int32_t pid() const noexcept;
    [[nodiscard]] bool is_alive() const noexcept;
    [[nodiscard]] Transport transport() const noexcept;
    [[nodiscard]] uint64_t total_commit_count() const;
    [[nodiscard]] uint64_t presented_frames() const;

    void set_capture_backend(CaptureKind backend);
    void set_capture_target(CaptureTarget target);
    [[nodiscard]] result<cv::Mat> capture() const;

    [[nodiscard]] optional<cv::Size> frame_size() const;
    [[nodiscard]] optional<CursorPosition> cursor_position() const;
    [[nodiscard]] std::vector<WindowInfo> windows() const;
    [[nodiscard]] std::vector<SessionInfo> sessions() const;
    [[nodiscard]] std::vector<SurfaceInfo> surfaces() const;
    [[nodiscard]] result<void> select_window(int32_t index);
    /// Platform-neutral target selection. Native providers accept
    /// TargetKind::WaylandSurface; unsupported kinds return
    /// feature_not_implemented.
    [[nodiscard]] result<void> select_target(TargetId target);
    [[nodiscard]] TargetId selected_target() const;
    /// Attach a push-style frame consumer. Frames are published whenever
    /// capture() produces one; sink callbacks run on the sink's worker thread.
    [[nodiscard]] result<uint64_t> attach_frame_sink(FrameSink sink, SinkPolicy policy = {});
    void detach_frame_sink(uint64_t id);
    [[nodiscard]] optional<SinkStats> frame_sink_stats(uint64_t id) const;
    /// Start a background capture pump. Each successful capture is published
    /// to attached FrameSinks. interval == 0 captures as fast as possible.
    [[nodiscard]] result<void> start_frame_pump(std::chrono::milliseconds interval = std::chrono::milliseconds{16});
    void stop_frame_pump() noexcept;
    [[nodiscard]] bool frame_pump_running() const noexcept;
    /// Create a device-independent controller bound to this app's active
    /// session. The returned handle must not outlive this ControlledApp.
    [[nodiscard]] result<Controller> controller();
    void set_allow_activation(bool value);
    void stop() noexcept;
    [[nodiscard]] bool ensure_active_session() const;
    void apply_activation_selection() const;

    /// Opaque state shared with the internal controller backends. Defined in
    /// `harness/controlled_app_impl.h`; not part of the public ABI.
    struct Impl;

private:
    friend class ComputerUse;
    friend class Controller;
    explicit ControlledApp(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};
class SKL_WCAS_API LaunchConfig {
public:
    explicit LaunchConfig(std::string program);
    static LaunchConfig wine(std::string program);
    static LaunchConfig proton(std::string program);
    static LaunchConfig umu(std::string program);
    LaunchConfig &arg(std::string value);
    LaunchConfig &env(std::string key, std::string value);
    LaunchConfig &env_remove(std::string key);
    LaunchConfig &current_dir(std::string value);
    LaunchConfig &inherit_diagnostics(bool value = true) noexcept;
    LaunchConfig &gpu_accelerated(bool value = true) noexcept;
    LaunchConfig &graphics_api(GraphicsApi value) noexcept;
    LaunchConfig &transport(LaunchTransport value) noexcept;
    /// Extra process matcher (ECMAScript regex) for windows that are not direct
    /// descendants of the launched program. Defaults to the program's parent
    /// directory. Matched against the process command line + executable path.
    LaunchConfig &match_regex(std::string value);
    [[nodiscard]] const std::string &program() const noexcept;

private:
    friend class ComputerUse;
    std::string program_;
    std::vector<std::string> args_;
    std::vector<std::pair<std::string, optional<std::string>>> env_;
    optional<std::string> current_dir_;
    optional<std::string> match_regex_;
    bool inherit_diagnostics_ = false;
    bool gpu_accelerated_ = false;
    GraphicsApi graphics_api_ = GraphicsApi::vulkan;
    LaunchTransport transport_ = LaunchTransport::Auto;
};
class SKL_WCAS_API ComputerUseBuilder {
public:
    ComputerUseBuilder &native_timeout(std::chrono::milliseconds value) noexcept;
    ComputerUseBuilder &fallback_timeout(std::chrono::milliseconds value) noexcept;
    ComputerUseBuilder &content_timeout(std::chrono::milliseconds value) noexcept;
    ComputerUseBuilder &bridge_geometry(int32_t width, int32_t height) noexcept;
    ComputerUseBuilder &for_proton() noexcept;
    [[nodiscard]] result<class ComputerUse> build() const;

private:
    std::chrono::milliseconds native_timeout_{8'000};
    std::chrono::milliseconds fallback_timeout_{30'000};
    std::chrono::milliseconds content_timeout_{30'000};
    cv::Size bridge_window = {1'280, 800};
};
class SKL_WCAS_API ComputerUse {
public:
    ComputerUse(ComputerUse &&) noexcept;
    ComputerUse &operator=(ComputerUse &&) noexcept;
    ~ComputerUse();
    ComputerUse(const ComputerUse &) = delete;
    ComputerUse &operator=(const ComputerUse &) = delete;
    static result<ComputerUse> create();
    static ComputerUseBuilder builder() noexcept;
    [[nodiscard]] std::string socket_name() const;
    [[nodiscard]] result<ControlledApp> launch(const LaunchConfig &config) const;
    void close() noexcept;

private:
    friend class ComputerUseBuilder;
    struct Impl;
    explicit ComputerUse(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] SKL_WCAS_API bool frame_has_visible_content(const cv::Mat &frame) noexcept;
}   // namespace skl::wcas
