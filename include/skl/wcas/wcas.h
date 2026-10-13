#pragma once
#include "skl/wcas/api.h"
#include <boost/outcome.hpp>
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace skl::wcas {
template<typename T>
using result = boost::outcome_v2::std_result<T>;
enum class PointerButton {
    left,
    right,
    middle
};
enum class Transport {
    native_wayland,
    xwayland
};
enum class GraphicsApi {
    /// Proton/umu default: DXVK translates the game's DirectX calls to Vulkan.
    dxvk,
    /// The game's own native Vulkan renderer: DXVK is disabled so the game
    /// reaches Vulkan directly. Only meaningful for games that ship one.
    vulkan,
    /// WineD3D (OpenGL), selected via PROTON_USE_WINED3D.
    opengl
};
enum class CaptureBackend {
    client_buffer,
    kms,
    shm,
    dmabuf
};
struct WCAS_API CaptureTarget {
    std::optional<std::string> app_id;
    std::optional<std::string> title;
};
struct WCAS_API CapturedFrame {
    cv::Mat image;
    [[nodiscard]] int width() const noexcept { return image.cols; }
    [[nodiscard]] int height() const noexcept { return image.rows; }
};
/// Current size of the app's committed surface, without doing a capture. Use it
/// to keep input coordinates in step with a window that has been resized.
struct WCAS_API FrameSize {
    int width = 0;
    int height = 0;
};
/// Pointer position reported by the compositor for the app's focused surface
/// (surface-local, logical pixels). This reflects the real input device, not
/// the coordinates wcas injected.
struct WCAS_API CursorPosition {
    double x = 0.0;
    double y = 0.0;
};
/// How a window was discovered. A rendering surface often is not the toplevel
/// itself (games put window content in a subsurface), so this distinguishes a
/// real toplevel from content that merely inherited its metadata.
enum class WCAS_API WindowOrigin : uint8_t {
    Toplevel = 0,
    /// Content surface whose app_id/title were inherited from an ancestor toplevel.
    Subsurface,
    /// Renders frames but has neither a toplevel nor a known parent.
    Bare,
};
/// A window managed by wcas: the main app window or a later-started child
/// window (overlay, dialog) that opened its own Wayland connection.
struct WCAS_API WindowInfo {
    std::string app_id;
    std::string title;
    int width = 0;
    int height = 0;
    int pid = 0;
    bool selected = false;
    WindowOrigin origin = WindowOrigin::Bare;
    /// Wayland surface id within its session. Distinct windows can share
    /// app_id/title, so this disambiguates entries in `list`.
    std::uint32_t surface_id = 0;
};
/// Diagnostic view of every proxy session seen for this launch, including
/// disconnected or unmatched ones (useful when a window seems to disappear).
struct WCAS_API SessionInfo {
    int pid = 0;
    bool connected = false;
    bool matched = false;
    int windows = 0;
    /// Total wl_surface / xdg_toplevel objects tracked for this session. A
    /// session with surfaces>0 but windows==0 exists but has not rendered yet;
    /// surfaces==0 means no window was ever created on this connection.
    int surfaces = 0;
    int toplevels = 0;
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
struct WCAS_API SurfaceInfo {
    int pid = 0;
    std::uint32_t surface_id = 0;
    std::uint32_t parent_surface_id = 0;
    std::uint32_t toplevel_id = 0;
    std::string role;
    std::string app_id;
    std::string title;
    int width = 0;
    int height = 0;
    bool has_frame = false;
    std::uint64_t last_commit_seq = 0;
};
class WCAS_API LaunchConfig {
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
    /// Extra process matcher (ECMAScript regex) for windows that are not direct
    /// descendants of the launched program. Defaults to the program's parent
    /// directory. Matched against the process command line + executable path.
    LaunchConfig &match_regex(std::string value);
    [[nodiscard]] const std::string &program() const noexcept;

private:
    friend class ComputerUse;
    std::string program_;
    std::vector<std::string> args_;
    std::vector<std::pair<std::string, std::optional<std::string>>> env_;
    std::optional<std::string> current_dir_;
    std::optional<std::string> match_regex_;
    bool inherit_diagnostics_ = false;
    bool gpu_accelerated_ = false;
    GraphicsApi graphics_api_ = GraphicsApi::vulkan;
};
class ControlledApp;
class WCAS_API ComputerUseBuilder {
public:
    ComputerUseBuilder &native_timeout(std::chrono::milliseconds value) noexcept;
    ComputerUseBuilder &fallback_timeout(std::chrono::milliseconds value) noexcept;
    ComputerUseBuilder &content_timeout(std::chrono::milliseconds value) noexcept;
    ComputerUseBuilder &bridge_geometry(int width, int height) noexcept;
    ComputerUseBuilder &for_proton() noexcept;
    [[nodiscard]] result<class ComputerUse> build() const;

private:
    std::chrono::milliseconds native_timeout_{8'000};
    std::chrono::milliseconds fallback_timeout_{30'000};
    std::chrono::milliseconds content_timeout_{30'000};
    int bridge_width_ = 1'280;
    int bridge_height_ = 800;
};
class WCAS_API ComputerUse {
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
class WCAS_API ControlledApp {
public:
    ControlledApp(ControlledApp &&) noexcept;
    ControlledApp &operator=(ControlledApp &&) noexcept;
    ~ControlledApp();
    ControlledApp(const ControlledApp &) = delete;
    ControlledApp &operator=(const ControlledApp &) = delete;
    [[nodiscard]] int pid() const noexcept;
    [[nodiscard]] bool is_alive() const noexcept;
    [[nodiscard]] Transport transport() const noexcept;
    [[nodiscard]] std::uint64_t total_commit_count() const noexcept;
    /// Frames the app has presented (from `wl_surface.frame` / `wl_callback.done`).
    /// Zero for XWayland or apps that never request frame callbacks.
    [[nodiscard]] std::uint64_t presented_frames() const noexcept;
    void set_capture_backend(CaptureBackend backend) noexcept;
    void set_capture_target(CaptureTarget target);
    [[nodiscard]] result<CapturedFrame> capture() const;
    /// Size of the most recently committed frame of the target surface. Cheap:
    /// it reads the retained frame metadata and performs no GPU/mmap capture.
    [[nodiscard]] std::optional<FrameSize> frame_size() const;
    /// Latest pointer position reported by the compositor for the app. Empty
    /// until the compositor has sent a `wl_pointer.enter`/`motion` event, i.e.
    /// until the real device has moved over the app at least once.
    [[nodiscard]] std::optional<CursorPosition> cursor_position() const;
    /// Every window wcas currently manages for this launch (main window plus
    /// later-started child windows), in a stable order. Index into this list
    /// with `select_window`.
    [[nodiscard]] std::vector<WindowInfo> windows() const;
    /// Every proxy session seen for this launch (connected or not, matched or
    /// not) — diagnostics for why a window is missing.
    [[nodiscard]] std::vector<SessionInfo> sessions() const;
    /// Every tracked surface (including non-toplevel, non-rendered ones) across
    /// all sessions — diagnostics for locating a window that `windows()` omits.
    [[nodiscard]] std::vector<SurfaceInfo> surfaces() const;
    /// Route capture and input to the window at `index` (from `windows()`).
    [[nodiscard]] result<void> select_window(int index);
    /// When enabled, the selected window's `xdg_activation_v1.activate` requests
    /// are forwarded so the compositor activates it. Required for Wine/Unity
    /// windows to accept synthetic keyboard input, but the activation takes the
    /// user's keyboard focus. Off by default.
    void set_allow_activation(bool value) noexcept;
    [[nodiscard]] result<void> move_mouse(double x, double y) const;
    [[nodiscard]] result<void> click(
        double x, double y, PointerButton button = PointerButton::left, unsigned count = 1) const;
    [[nodiscard]] result<void> click_with_modifiers(
        double x, double y, PointerButton button, unsigned count, std::string modifiers) const;
    [[nodiscard]] result<void> scroll(double x, double y, int dx, int dy) const;
    [[nodiscard]] result<void> press_key(std::string combination) const;
    /// Like `press_key`, but keeps the key (and modifiers) held for `hold`
    /// before releasing, for long-press actions (e.g. `key Enter 3s`).
    [[nodiscard]] result<void> press_key_hold(std::string combination, std::chrono::milliseconds hold) const;
    [[nodiscard]] result<void> type_text(std::string text) const;
    [[nodiscard]] result<void> drag(double from_x, double from_y, double to_x, double to_y) const;
    /// Press at (from_x, from_y), slide by (dx, dy) frame pixels over `hold`,
    /// then release. Models a long press-and-drag (e.g. `click 4s R 300`).
    [[nodiscard]] result<void> press_drag(
        double from_x, double from_y, double dx, double dy, std::chrono::milliseconds hold) const;
    /// Move by (dx, dy) frame pixels **without** pressing a mouse button.
    /// Drives pointer-locked 3D cameras (relative motion) and moves the cursor
    /// (absolute motion) but never registers as a click.
    [[nodiscard]] result<void> relative_drag(double from_x, double from_y, double dx, double dy) const;
    void stop() noexcept;

private:
    friend class ComputerUse;
    struct Impl;
    explicit ControlledApp(std::unique_ptr<Impl> impl) noexcept;
    /// Point `impl_->session` at a live, capturable session (the selected one if
    /// still valid, otherwise the best in the launch tree). Persists the
    /// fallback so status/list agree. False when nothing usable exists.
    [[nodiscard]] bool ensure_active_session() const;
    /// Apply `impl_->allow_activation` to the session tree: only the selected
    /// session may forward activation.
    void apply_activation_selection() const;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] WCAS_API bool frame_has_visible_content(const CapturedFrame &frame) noexcept;
}   // namespace skl::wcas
