#include "skl/wcas/wcas.h"
#include "skl/wcas/error.h"

#include "debug.h"
#include "harness/controlled_app_impl.h"
#include "harness/launch_strategy.h"
#include "harness/process_host.h"
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
#include <mutex>
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
    /// Per-connection proxy threads. They are joined on shutdown; leaving them
    /// detached let them run during process exit and crash on static state.
    std::mutex connection_mutex;
    std::vector<std::thread> connection_threads;
    std::vector<std::shared_ptr<proxy::ConnState>> connection_states;
    bool closed = false;
    ~Impl() {
        stopping = true;
        if (socket_fd >= 0) {
            ::shutdown(socket_fd, SHUT_RDWR);
            ::close(socket_fd);
        }
        if (accept_thread.joinable()) accept_thread.join();
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(connection_mutex);
            for (const auto &state : connection_states)
                state->stop = true;
            threads.swap(connection_threads);
        }
        for (auto &thread : threads) {
            if (thread.joinable()) thread.join();
        }
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
    return wine(std::move(p)).env("SteamGameId", "0").graphics_api(GraphicsApi::dxvk);
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
    bridge_window = cv::Size(w, h);
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
        || bridge_window.width < 160
        || bridge_window.height < 120)
        return code(errc::unspecified);
    auto out = ComputerUse::create();
    if (out) {
        out.value().impl_->native_timeout = native_timeout_;
        out.value().impl_->fallback_timeout = fallback_timeout_;
        out.value().impl_->content_timeout = content_timeout_;
        out.value().impl_->bridge_width = bridge_window.width;
        out.value().impl_->bridge_height = bridge_window.height;
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
                debug_trace(DebugCategory::Harness, fmt::format("rejected proxy connection uid={} (expected {})",
                                                        credentials.uid, static_cast<int>(::getuid())));
                ::close(client);
                continue;
            }
            const auto upstream = connect_wayland(state->runtime, state->upstream);
            if (upstream < 0) {
                debug_trace(DebugCategory::Harness,
                    fmt::format("upstream compositor connect failed for pid={}", credentials.pid));
                ::close(client);
                continue;
            }
            auto session = std::make_shared<proxy::ConnState>();
            session->owner_pid = credentials.pid;
            session->client_fd = client;
            detail::register_session(credentials.pid, session);
            debug_trace(
                DebugCategory::Harness, fmt::format("proxy accepted connection pid={} client_fd={} upstream_fd={}",
                                            credentials.pid, client, upstream));
            {
                std::lock_guard lock(state->connection_mutex);
                state->connection_states.push_back(session);
                state->connection_threads.emplace_back([client, upstream, session] {
                    proxy::Connection(client, upstream, session).run();
                    debug_trace(DebugCategory::Harness, fmt::format("proxy connection ended pid={} client_fd={}",
                                                            static_cast<long>(session->owner_pid), client));
                    detail::erase_session(session);
                });
            }
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
        ProcessSpec spec;
        spec.program = config.program_;
        spec.args = config.args_;
        spec.env = config.env_;
        spec.current_dir = config.current_dir_;
        spec.inherit_diagnostics = config.inherit_diagnostics_;
        spec.gpu_accelerated = config.gpu_accelerated_;
        spec.graphics_api = config.graphics_api_;
        spec.use_x11 = use_x11;
        spec.display = display;
        spec.xauthority = xauthority;
        spec.wayland_socket = impl_->socket_name;

        auto process = std::make_unique<ProcessHost>();
        if (!process->spawn(spec)) return code(errc::process_spawn_failed);
        const pid_t child = process->pid();
        const auto start_time = std::chrono::steady_clock::now();
        if (use_x11) {
            // X11-only apps never connect to the Wayland proxy socket, so the
            // success condition is visible content on the bridge display,
            // bounded by content_timeout.
            const auto content_start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - content_start < impl_->content_timeout) {
                if (process->poll_exited()) return code(errc::process_spawn_failed);
                auto frame = capture_x11_window(display, xauthority);
                if (frame.has_value() && x11_frame_has_visible_content(frame.value())) {
                    auto app = std::make_unique<ControlledApp::Impl>();
                    app->process = std::move(process);
                    app->child = child;
                    app->process_group = child;
                    app->transport = Transport::xwayland;
                    app->bridge = std::move(bridge);
                    app->match_regex = match_regex;
                    return ControlledApp(std::move(app));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            process->terminate();
            return code(errc::xwayland_bridge_failed);
        }

        // Native Wayland: wait for the proxy session to report a capturable surface.
        std::shared_ptr<proxy::ConnState> session;
        // Whether any proxy session ever connected. A launcher exits after
        // spawning the game; the game's own connections may come and go while it
        // boots, so a momentary "no session right now" must not be read as a
        // spawn failure.
        bool session_seen = false;
        const auto timeout = impl_->native_timeout;
        auto last_trace = start_time;
        while (std::chrono::steady_clock::now() - start_time < timeout) {
            // `match_regex` matters here, not just for `list`/`target`: the
            // process that connects to the proxy is `wineserver`, which detaches,
            // so once an intermediate launcher (umu-run, proton.py) exits the
            // owner is re-parented to init and the pid chain no longer reaches
            // `child`. Matching on pid alone then never finds the session even
            // though it is connected and capturable, the 30s timeout expires and
            // the whole game gets killed and relaunched over XWayland.
            session = detail::session_for(child, match_regex);
            if (session && session->connected.load()) session_seen = true;
            if (debug_enabled(DebugCategory::Harness)
                && std::chrono::steady_clock::now() - last_trace >= std::chrono::seconds(1)) {
                last_trace = std::chrono::steady_clock::now();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
                        .count();
                debug_trace(DebugCategory::Harness,
                    fmt::format("native wait {}ms: session={} connected={} capturable={} commits={}", elapsed,
                        session ? "yes" : "no", session && session->connected.load() ? 1 : 0,
                        session && session->capturable_surface.load() ? 1 : 0,
                        session ? session->commit_count.load() : 0));
            }
            if (session && session->connected && session->capturable_surface) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (session->connected) break;
                session.reset();
            }
            if (process->poll_exited()) {
                const int status = process->exit_status();
                debug_trace(DebugCategory::Harness,
                    fmt::format("native child pid={} exited (status={}), session={} connected={} session_seen={}",
                        child, WIFEXITED(status) ? WEXITSTATUS(status) : -1, session ? "yes" : "no",
                        session && session->connected.load() ? 1 : 0, session_seen ? 1 : 0));
                // A launcher (umu-run / proton.py) exits after spawning the real
                // game, which keeps running under a descendant. Only fail when no
                // session ever connected at all; otherwise keep waiting for the
                // game's capturable surface until timeout, instead of aborting
                // native early and falling back to an empty XWayland window.
                if (!session_seen) return code(errc::process_spawn_failed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!session || !session->connected || !session->capturable_surface) {
            debug_trace(DebugCategory::Harness,
                fmt::format("native timed out after {}ms (session={} connected={} capturable={} commits={})",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
                        .count(),
                    session ? "yes" : "no", session && session->connected.load() ? 1 : 0,
                    session && session->capturable_surface.load() ? 1 : 0, session ? session->commit_count.load() : 0));
            process->terminate();
            return code(errc::native_wayland_timeout);
        }
        auto app = std::make_unique<ControlledApp::Impl>();
        app->process = std::move(process);
        app->child = child;
        app->process_group = child;
        app->session = std::move(session);
        app->transport = Transport::native_wayland;
        app->match_regex = match_regex;
        debug_trace(DebugCategory::Harness, "native session ready (capturable)");
        return ControlledApp(std::move(app));
    };

    // The launch order is a strategy contract; each strategy still delegates
    // fork/exec/wait to ProcessHost via `attempt`.
    std::error_code last_error = code(errc::native_wayland_timeout);
    for (const auto &strategy : launch_strategies()) {
        if (!strategy.available()) continue;
        switch (strategy.kind()) {
            case LaunchKind::NativeWayland: {
                auto native = attempt(false, "", "", nullptr);
                if (native) return native;
                last_error = native.error();
                debug_trace(DebugCategory::Harness, "native Wayland failed, trying next launch strategy");
                break;
            }
            case LaunchKind::XWayland: {
                if (impl_->fallback_timeout.count() <= 0) continue;
                auto bridge = std::make_unique<XwaylandBridge>();
                if (!bridge->start(impl_->socket_name, 5)) {
                    debug_trace(DebugCategory::Harness, "XWayland bridge start failed");
                    last_error = code(errc::xwayland_bridge_failed);
                    continue;
                }
                // Copy strings before moving the bridge: argument evaluation
                // order is unspecified.
                const std::string x11_display = bridge->display();
                const std::string x11_xauthority = bridge->xauthority();
                debug_trace(DebugCategory::Harness, fmt::format("XWayland bridge display={}", x11_display));
                auto fallback = attempt(true, x11_display, x11_xauthority, std::move(bridge));
                if (fallback) return fallback;
                last_error = fallback.error();
                debug_trace(DebugCategory::Harness, "XWayland fallback failed");
                break;
            }
            case LaunchKind::Custom:
            case LaunchKind::Unknown: break;
        }
    }
    return last_error;
}

}   // namespace skl::wcas
