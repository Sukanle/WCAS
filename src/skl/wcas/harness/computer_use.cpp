#include "skl/wcas/wcas.h"
#include "skl/wcas/error.h"

#include "harness/controlled_app_impl.h"
#include "harness/session.h"
#include "x11_capture.h"
#include "xwayland.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/base.h>

namespace skl::wcas {
namespace {
std::error_code code(errc value) { return make_error_code(value); }
std::string runtime_directory() {
    const char *v = std::getenv("XDG_RUNTIME_DIR");
    return v ? v : "";
}
std::string upstream_display() {
    const char *v = std::getenv("WAYLAND_DISPLAY");
    return v ? v : "";
}
int connect_wayland(const std::string &runtime, const std::string &display) {
    const auto path = display.front() == '/' ? display : runtime + "/" + display;
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
/// Turn a path into a literal ECMAScript regex so directory separators and
/// dots do not act as metacharacters.
std::string escape_regex(const std::string &value) {
    std::string out;
    out.reserve(value.size() * 2);
    for (const char c : value) {
        if (std::strchr(".^$|()[]{}*+?\\", c) != nullptr) out += '\\';
        out += c;
    }
    return out;
}
}   // namespace

struct ComputerUse::Impl {
    int socket_fd = -1;
    std::string socket_path, socket_name, runtime, upstream;
    std::chrono::milliseconds native_timeout{8'000}, fallback_timeout{30'000}, content_timeout{30'000};
    int bridge_width = 1'280, bridge_height = 800;
    std::atomic<bool> stopping{false};
    std::thread accept_thread;
    bool closed = false;
    ~Impl() {
        stopping = true;
        if (socket_fd >= 0) {
            ::shutdown(socket_fd, SHUT_RDWR);
            ::close(socket_fd);
        }
        if (accept_thread.joinable()) accept_thread.join();
        if (!socket_path.empty()) ::unlink(socket_path.c_str());
    }
};

LaunchConfig::LaunchConfig(std::string p)
    : program_(std::move(p)) {}
LaunchConfig LaunchConfig::wine(std::string p) {
    LaunchConfig c(std::move(p));
    return c.env("PROTON_ENABLE_WAYLAND", "1").env("WINE_GST_REGISTRY_DIR", "/tmp/wine-gst").gpu_accelerated();
}
LaunchConfig LaunchConfig::proton(std::string p) {
    return wine(std::move(p)).env("SteamGameId", "0").graphics_api(GraphicsApi::vulkan);
}
LaunchConfig LaunchConfig::umu(std::string p) {
    LaunchConfig c("umu-run");
    return c.arg(std::move(p))
        .env("GAMEID", "umu-default")
        .env("PROTON_VERB", "waitforexitandrun")
        .env("PROTON_ENABLE_WAYLAND", "1")
        .gpu_accelerated();
}
LaunchConfig &LaunchConfig::arg(std::string v) {
    args_.push_back(std::move(v));
    return *this;
}
LaunchConfig &LaunchConfig::env(std::string k, std::string v) {
    env_.push_back({std::move(k), std::move(v)});
    return *this;
}
LaunchConfig &LaunchConfig::env_remove(std::string k) {
    env_.push_back({std::move(k), std::nullopt});
    return *this;
}
LaunchConfig &LaunchConfig::current_dir(std::string v) {
    current_dir_ = std::move(v);
    return *this;
}
LaunchConfig &LaunchConfig::inherit_diagnostics(bool v) noexcept {
    inherit_diagnostics_ = v;
    return *this;
}
LaunchConfig &LaunchConfig::gpu_accelerated(bool v) noexcept {
    gpu_accelerated_ = v;
    return *this;
}
LaunchConfig &LaunchConfig::graphics_api(GraphicsApi v) noexcept {
    graphics_api_ = v;
    return *this;
}
LaunchConfig &LaunchConfig::match_regex(std::string value) {
    match_regex_ = std::move(value);
    return *this;
}
const std::string &LaunchConfig::program() const noexcept { return program_; }
ComputerUseBuilder &ComputerUseBuilder::native_timeout(std::chrono::milliseconds v) noexcept {
    native_timeout_ = v;
    return *this;
}
ComputerUseBuilder &ComputerUseBuilder::fallback_timeout(std::chrono::milliseconds v) noexcept {
    fallback_timeout_ = v;
    return *this;
}
ComputerUseBuilder &ComputerUseBuilder::content_timeout(std::chrono::milliseconds v) noexcept {
    content_timeout_ = v;
    return *this;
}
ComputerUseBuilder &ComputerUseBuilder::bridge_geometry(int w, int h) noexcept {
    bridge_width_ = w;
    bridge_height_ = h;
    return *this;
}
ComputerUseBuilder &ComputerUseBuilder::for_proton() noexcept {
    native_timeout_ = fallback_timeout_ = content_timeout_ = std::chrono::seconds(120);
    return *this;
}
result<ComputerUse> ComputerUseBuilder::build() const {
    if (native_timeout_.count() <= 0
        || fallback_timeout_.count() <= 0
        || content_timeout_.count() <= 0
        || bridge_width_ < 160
        || bridge_height_ < 120)
        return code(errc::unspecified);
    auto out = ComputerUse::create();
    if (out) {
        out.value().impl_->native_timeout = native_timeout_;
        out.value().impl_->fallback_timeout = fallback_timeout_;
        out.value().impl_->content_timeout = content_timeout_;
        out.value().impl_->bridge_width = bridge_width_;
        out.value().impl_->bridge_height = bridge_height_;
    }
    return out;
}
ComputerUse::ComputerUse(std::unique_ptr<Impl> p) noexcept
    : impl_(std::move(p)) {}
ComputerUse::ComputerUse(ComputerUse &&) noexcept = default;
ComputerUse &ComputerUse::operator=(ComputerUse &&) noexcept = default;
ComputerUse::~ComputerUse() = default;
result<ComputerUse> ComputerUse::create() {
    const auto runtime = runtime_directory(), display = upstream_display();
    if (runtime.empty()) return code(errc::missing_runtime_dir);
    if (display.empty() || connect_wayland(runtime, display) < 0) return code(errc::no_wayland_session);
    auto impl = std::make_unique<Impl>();
    impl->runtime = runtime;
    impl->upstream = display;
    impl->socket_name = "agent-seat-" + std::to_string(::getpid());
    impl->socket_path = runtime + "/" + impl->socket_name;
    impl->socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (impl->socket_fd < 0 || impl->socket_path.size() >= sizeof(sockaddr_un::sun_path))
        return code(errc::socket_bind_failed);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, impl->socket_path.c_str(), sizeof(address.sun_path) - 1);
    ::unlink(impl->socket_path.c_str());
    if (::bind(impl->socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || ::chmod(impl->socket_path.c_str(), 0600) != 0
        || ::listen(impl->socket_fd, 16) != 0)
        return code(errc::socket_bind_failed);
    auto *state = impl.get();
    impl->accept_thread = std::thread([state] {
        while (!state->stopping) {
            pollfd p{state->socket_fd, POLLIN, 0};
            if (::poll(&p, 1, 250) <= 0) continue;
            int client = ::accept4(state->socket_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0
                || credentials.uid != ::getuid()) {
                ::close(client);
                continue;
            }
            const auto upstream = connect_wayland(state->runtime, state->upstream);
            if (upstream < 0) {
                ::close(client);
                continue;
            }
            auto session = std::make_shared<proxy::ConnState>();
            session->owner_pid = credentials.pid;
            session->client_fd = client;
            detail::register_session(credentials.pid, session);
            std::thread([client, upstream, session] {
                proxy::Connection(client, upstream, session).run();
                detail::erase_session(static_cast<pid_t>(session->owner_pid));
            }).detach();
        }
    });
    return ComputerUse(std::move(impl));
}
ComputerUseBuilder ComputerUse::builder() noexcept { return {}; }
std::string ComputerUse::socket_name() const { return impl_ ? impl_->socket_name : ""; }
void ComputerUse::close() noexcept {
    if (impl_ && !impl_->closed) {
        impl_->closed = true;
        if (impl_->socket_fd >= 0) {
            ::shutdown(impl_->socket_fd, SHUT_RDWR);
            ::close(impl_->socket_fd);
            impl_->socket_fd = -1;
        }
        ::unlink(impl_->socket_path.c_str());
    }
}
result<ControlledApp> ComputerUse::launch(const LaunchConfig &config) const {
    if (!impl_ || impl_->closed || config.program_.empty()) return code(errc::process_spawn_failed);

    // Extra matcher for child windows launched later (e.g. a game overlay that
    // lives beside the main exe). Defaults to the program's parent directory;
    // descendant matching still runs on top of this.
    std::string match_regex = config.match_regex_.value_or("");
    if (match_regex.empty()) {
        const auto parent = std::filesystem::path(config.program_).parent_path().string();
        if (!parent.empty()) match_regex = escape_regex(parent);
    }

    // Spawn the app once and wait for its proxy session. `use_x11` selects the
    // XWayland bridge environment instead of the native Wayland socket.
    auto attempt = [&](bool use_x11, const std::string &display, const std::string &xauthority,
                       std::unique_ptr<XwaylandBridge> bridge) -> result<ControlledApp> {
        const auto start_time = std::chrono::steady_clock::now();
        const pid_t child = ::fork();
        if (child < 0) return code(errc::process_spawn_failed);
        if (child == 0) {
            ::setpgid(0, 0);
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (!config.inherit_diagnostics_) {
                const int fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
                if (fd >= 0) {
                    ::dup2(fd, 1);
                    ::dup2(fd, 2);
                    if (fd > 2) ::close(fd);
                }
            }
            if (config.current_dir_) ::chdir(config.current_dir_->c_str());
            if (use_x11) {
                ::setenv("DISPLAY", display.c_str(), 1);
                ::setenv("XAUTHORITY", xauthority.c_str(), 1);
                ::setenv("XDG_SESSION_TYPE", "x11", 1);
                ::unsetenv("WAYLAND_DISPLAY");
            } else {
                ::setenv("WAYLAND_DISPLAY", impl_->socket_name.c_str(), 1);
                ::setenv("GDK_BACKEND", "wayland", 1);
                ::setenv("QT_QPA_PLATFORM", "wayland", 1);
            }
            if (!config.gpu_accelerated_) ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
            if (config.graphics_api_ == GraphicsApi::opengl)
                ::setenv("PROTON_USE_WINED3D", "1", 1);
            else
                ::unsetenv("PROTON_USE_WINED3D");
            for (const auto &e : config.env_) {
                if (e.second)
                    ::setenv(e.first.c_str(), e.second->c_str(), 1);
                else
                    ::unsetenv(e.first.c_str());
            }
            std::vector<char *> argv{const_cast<char *>(config.program_.c_str())};
            for (const auto &arg : config.args_)
                argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            _exit(127);
        }
        ::setpgid(child, child);
        if (use_x11) {
            // X11-only apps never connect to the Wayland proxy socket, so the
            // success condition is visible content on the bridge display,
            // bounded by content_timeout.
            const auto content_start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - content_start < impl_->content_timeout) {
                int status = 0;
                if (::waitpid(child, &status, WNOHANG) == child) return code(errc::process_spawn_failed);
                auto frame = capture_x11_window(display, xauthority);
                if (frame.has_value() && x11_frame_has_visible_content(frame.value())) {
                    auto app = std::make_unique<ControlledApp::Impl>();
                    app->child = child;
                    app->process_group = child;
                    app->transport = Transport::xwayland;
                    app->bridge = std::move(bridge);
                    app->match_regex = match_regex;
                    return ControlledApp(std::move(app));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            detail::terminate_group(child);
            return code(errc::xwayland_bridge_failed);
        }

        // Native Wayland: wait for the proxy session to report a capturable surface.
        std::shared_ptr<proxy::ConnState> session;
        const auto timeout = impl_->native_timeout;
        while (std::chrono::steady_clock::now() - start_time < timeout) {
            session = detail::session_for(child);
            if (session && session->connected && session->capturable_surface) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (session->connected) break;
                session.reset();
            }
            int status = 0;
            if (::waitpid(child, &status, WNOHANG) == child) return code(errc::process_spawn_failed);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!session || !session->connected || !session->capturable_surface) {
            detail::terminate_group(child);
            return code(errc::native_wayland_timeout);
        }
        auto app = std::make_unique<ControlledApp::Impl>();
        app->child = child;
        app->process_group = child;
        app->session = std::move(session);
        app->transport = Transport::native_wayland;
        app->match_regex = match_regex;
        return ControlledApp(std::move(app));
    };

    // 1) Native Wayland first; fall back to an authenticated XWayland bridge.
    if (auto native = attempt(false, "", "", nullptr); native) return native;
    if (impl_->fallback_timeout.count() <= 0) return code(errc::native_wayland_timeout);

    auto bridge = std::make_unique<XwaylandBridge>();
    if (!bridge->start(impl_->socket_name, 5)) return code(errc::xwayland_bridge_failed);
    // Copy the strings before moving the bridge: argument evaluation order is
    // unspecified, so `bridge->display()` could otherwise run after the move.
    const std::string x11_display = bridge->display();
    const std::string x11_xauthority = bridge->xauthority();
    auto fallback = attempt(true, x11_display, x11_xauthority, std::move(bridge));
    if (!fallback) return code(errc::xwayland_bridge_failed);
    return fallback;
}

}   // namespace skl::wcas
