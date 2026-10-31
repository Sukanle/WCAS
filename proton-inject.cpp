#include "SKL/WCAS/WCAS.h"
#include "SKL/WCAS/DMABUF.h"
#include "SKL/WCAS/Private/IPC/Protocol.h"
#include "SKL/WCAS/Private/IPC/Server.h"
#include "SKL/WCAS/Private/SDM/Channel.h"

#include <opencv2/imgcodecs.hpp>
#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include <cmdly/terminal.h>
#include <fmt/std.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctype.h>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace skl::wcas;

volatile std::sig_atomic_t g_ipc_stop = 0;

void handle_ipc_stop(int) { g_ipc_stop = 1; }

/// Debug log categories, mirroring the Rust `Debug` bitflags.
struct DebugFlags {
    bool capture = false;
    bool dmabuf = false;
    bool harness = false;
    bool proxy = false;
    bool input = false;
    bool xwayland = false;
    bool wire = false;
    bool all = false;

    [[nodiscard]] std::string describe() const {
        // clang-format off
        const std::pair<bool, const char *> categories[] = {
            {capture, "capture"},
            {dmabuf, "dmabuf"},
            {harness, "harness"},
            {proxy, "proxy"},
            {input, "input"},
            {xwayland, "xwayland"},
            {wire, "wire"},
        };
        // clang-format on
        std::string joined;
        for (const auto &[enabled, name] : categories) {
            if (!(all || enabled)) continue;
            if (!joined.empty()) joined += ", ";
            joined += name;
        }
        return joined.empty() ? "none" : joined;
    }
};

/// Everything the command line can configure.
struct Options {
    std::string program;
    std::string mode = "native";
    bool cli = false;
    bool serve = false;
    bool detach = false;
    std::string socket_path;
    bool allow_activation = false;
    double key_delay = 0.0;
    std::string output_monitor;
    bool inherit_output = false;
    bool gpu_accelerated = false;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> environment;
    GraphicsApi graphics_api = GraphicsApi::dxvk;
    CaptureKind capture_backend = CaptureKind::client_buffer;
    int native_timeout = 8;
    int fallback_timeout = 30;
    int content_timeout = 30;
    int stale_timeout = 60;
    int bridge_width = 1'980;
    int bridge_height = 1'080;
    std::string output_dir = "/tmp/wcas-logs";
    std::string match_regex;
    DebugFlags debug;
    bool probe_dmabuf = false;
    optional<int> test_dmabuf_fd;
};

optional<CaptureKind> parse_backend(const std::string &value) {
    if (value == "client" || value == "client-buffer" || value == "cache") return CaptureKind::client_buffer;
    if (value == "kms" || value == "drm") return CaptureKind::kms;
    if (value == "shm" || value == "wlshm") return CaptureKind::shm;
    if (value == "dmabuf") return CaptureKind::dmabuf;
    return std::nullopt;
}

optional<GraphicsApi> parse_graphics_api(const std::string &value) {
    if (value == "dxvk") return GraphicsApi::dxvk;
    if (value == "vulkan" || value == "vk") return GraphicsApi::vulkan;
    if (value == "opengl" || value == "gl" || value == "wined3d") return GraphicsApi::opengl;
    return std::nullopt;
}

void print_usage() {
    fmt::println(
        "usage: proton-inject --program EXEC [--arg ARG] "
        "[--proton|--wine|--umu] [--cli] [--serve [--socket PATH] [--detach]] "
        "[--capture-backend B] [--debug-all] "
        "[--native-timeout SEC] [--output-dir DIR] [--test-dmabuf FD]");
}

/// Parse a trailing hold duration such as `3s`, `500ms`, `1.5s`.
optional<std::chrono::milliseconds> parse_hold(const std::string &token) {
    std::string number;
    std::string unit;
    for (const char c : token) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.')
            number += c;
        else
            unit += c;
    }
    if (number.empty() || unit.empty()) return std::nullopt;
    double value = 0.0;
    try {
        value = std::stod(number);
    } catch (...) {
        return std::nullopt;
    }

    if (unit == "ms") return std::chrono::milliseconds(static_cast<long long>(value));

    return std::chrono::milliseconds(static_cast<long long>(value * 1000.0));
}

/// Split an optional trailing hold duration off a key combination, so
/// `key Enter 3s` becomes combination `Enter` + hold 3s.
void split_hold(std::string &combination, std::chrono::milliseconds &hold) {
    const auto last = combination.find_last_of(" \t");
    if (last == std::string::npos) return;
    const std::string tail = combination.substr(last + 1);
    if (tail == combination) return;
    const auto parsed = parse_hold(tail);
    if (!parsed) return;
    hold = *parsed;
    combination = combination.substr(0, last);
}

/// Move the window whose app_id is `cls` to monitor `monitor` using Hyprland's
/// IPC. Wayland gives a client no way to place its own window, so this is
/// compositor-specific (Hyprland only); it fails quietly elsewhere.
int hyprctl_move_to_monitor(const std::string &cls, const std::string &monitor) {
    if (cls.empty() || monitor.empty()) return -1;
    const std::string focus = "hyprctl dispatch focuswindow class:^(" + cls + ")$ >/dev/null 2>&1";
    const std::string move = "hyprctl dispatch movewindow mon:" + monitor + " >/dev/null 2>&1";
    return std::system((focus + "; " + move).c_str());
}

void run_dmabuf_probe(int fd) {
    fmt::print("\n═══ DMA-BUF 同步能力测试 (FD={}) ═══\n", fd);
    DmaBufSyncProbe probe(fd);
    fmt::print("{}", probe.run_all().report());
}

// ── option parsing ──────────────────────────────────────────────────────────

optional<int> resolve_graphics_api(const boost::program_options::variables_map &vm, Options &options) {
    if (vm.count("graphics-api") == 0) return std::nullopt;
    const auto value = vm["graphics-api"].as<std::string>();
    const auto parsed = parse_graphics_api(value);
    if (!parsed) {
        fmt::println(stderr, "error: 未知图形 API: {}，可选值: dxvk, vulkan (vk, 原生), opengl (gl/wined3d)", value);
        return 2;
    }
    options.graphics_api = *parsed;
    return std::nullopt;
}

optional<int> resolve_capture_backend(const boost::program_options::variables_map &vm, Options &options) {
    if (vm.count("capture-backend") == 0) return std::nullopt;
    const auto value = vm["capture-backend"].as<std::string>();
    const auto parsed = parse_backend(value);
    if (!parsed) {
        fmt::println(stderr, "error: 未知截图后端: {}，可选值: client (默认), kms, shm, dmabuf", value);
        return 2;
    }
    options.capture_backend = *parsed;
    return std::nullopt;
}

optional<int> resolve_environment(const std::vector<std::string> &entries, Options &options) {
    for (const auto &entry : entries) {
        const auto separator = entry.find('=');
        if (separator == std::string::npos || separator == 0) {
            fmt::println(stderr, "error: --env expects KEY=VALUE");
            return 2;
        }
        options.environment.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
    }
    return std::nullopt;
}

optional<int> validate_timeouts(const Options &options) {
    struct Timeout {
        const char *flag;
        int value;
    };
    // clang-format off
    const Timeout timeouts[] = {
        {"--stale-timeout", options.stale_timeout},
        {"--native-timeout", options.native_timeout},
        {"--fallback-timeout", options.fallback_timeout},
        {"--content-timeout", options.content_timeout},
    };
    // clang-format on
    for (const auto &timeout : timeouts) {
        if (timeout.value > 0) continue;
        fmt::println("error: {} requires a positive value", timeout.flag);
        return 2;
    }
    return std::nullopt;
}

/// Debug categories are surfaced through WCAS_DEBUG (the C++ umbrella gate);
/// the remaining flags set their own WCAS_DEBUG_<CATEGORY> so a single
/// category can be traced without pulling in the rest.
void export_debug_environment(const Options &options) {
    if (options.debug.all || options.debug.capture) setenv("WCAS_DEBUG", "1", 1);
    if (options.debug.dmabuf) setenv("WCAS_DEBUG_DMABUF", "1", 1);
    if (options.debug.harness) setenv("WCAS_DEBUG_HARNESS", "1", 1);
    if (options.debug.proxy) setenv("WCAS_DEBUG_PROXY", "1", 1);
    if (options.debug.input) setenv("WCAS_DEBUG_INPUT", "1", 1);
    if (options.debug.xwayland) setenv("WCAS_DEBUG_XWAYLAND", "1", 1);
    if (options.debug.wire) setenv("WCAS_DEBUG_WIRE", "1", 1);
    if (options.probe_dmabuf) setenv("WCAS_DMABUF_PROBE", "1", 1);
}

/// Fills `options` from argv. Returns an exit code when the caller must stop
/// (0 for --help, 2 for invalid input), otherwise nullopt.
optional<int> parse_arguments(int argc, char **argv, Options &options) {
    namespace po = boost::program_options;

    std::vector<std::string> env_entries;
    bool debug_rawdump = false;
    bool no_gpu = false;
    bool proton = false;
    bool wine = false;
    bool umu = false;

    po::options_description description("proton-inject options");

    // clang-format off
    description.add_options()
        ("help,h", "help message")
        ("program,p", po::value<std::string>(&options.program), "executable to launch")
        ("arg,s", po::value<std::vector<std::string>>(&options.args), "argument passed to the program (repeatable)")
        ("env,e", po::value<std::vector<std::string>>(&env_entries), "environment variable KEY=VALUE (repeatable)")
        ("cli,c", po::bool_switch(&options.cli), "use CLI mode")
        ("serve", po::bool_switch(&options.serve), "run as an IPC session host (does not read stdin)")
        ("detach", po::bool_switch(&options.detach), "detach from the controlling terminal (setsid)")
        ("socket", po::value<std::string>(&options.socket_path),
            "IPC socket path (default: $XDG_RUNTIME_DIR/wcas/<pid>.sock)")
        ("allow-activation", po::bool_switch(&options.allow_activation),
            "forward the selected window's activation so it accepts keyboard input (steals focus)")
        ("key-delay", po::value<double>(&options.key_delay)->default_value(0.0),
            "seconds to wait before injecting a key, to focus the window manually first")
        ("output", po::value<std::string>(&options.output_monitor),
            "move the game's windows to this Hyprland monitor (e.g. DP-2, HDMI-A-1)")
        ("output-dir,o", po::value<std::string>(&options.output_dir)->default_value(options.output_dir),
            "output directory")
        ("bridge-width,x", po::value<int>(&options.bridge_width)->default_value(options.bridge_width), "bridge width")
        ("bridge-height,y", po::value<int>(&options.bridge_height)->default_value(options.bridge_height),
            "bridge height")
        ("graphics-api,g", po::value<std::string>(), "dxvk (default), vulkan (vk, native), opengl (gl/wined3d)")
        ("capture-backend,b", po::value<std::string>(), "client (default), kms, shm, dmabuf")
        ("match-regex,r", po::value<std::string>(&options.match_regex),
            "extra regex for child-window processes (default: program dir)")
        ("debug-all", po::bool_switch(&options.debug.all), "enable all debug categories")
        ("debug-capture", po::bool_switch(&options.debug.capture), "enable capture debug logs")
        ("debug-dmabuf", po::bool_switch(&options.debug.dmabuf), "enable dmabuf debug logs")
        ("debug-harness", po::bool_switch(&options.debug.harness), "enable harness (launch/fallback) debug logs")
        ("debug-proxy", po::bool_switch(&options.debug.proxy), "enable proxy lifecycle debug logs")
        ("debug-input", po::bool_switch(&options.debug.input), "enable input injection debug logs")
        ("debug-xwayland", po::bool_switch(&options.debug.xwayland), "enable XWayland bridge debug logs")
        ("debug-wire", po::bool_switch(&options.debug.wire), "enable per-message Wayland tracing")
        ("debug-rawdump", po::bool_switch(&debug_rawdump), "dump raw frames to disk")
        ("test-dmabuf", po::value<int>(), "probe a specific DMA-BUF fd and exit")
        ("probe-dmabuf", po::bool_switch(&options.probe_dmabuf), "probe DMA-BUF sync capability")
        ("stale-timeout", po::value<int>(&options.stale_timeout)->default_value(options.stale_timeout),
            "stale frame timeout in seconds")
        ("native-timeout", po::value<int>(&options.native_timeout)->default_value(options.native_timeout),
            "native startup timeout in seconds")
        ("fallback-timeout", po::value<int>(&options.fallback_timeout)->default_value(options.fallback_timeout),
            "fallback timeout in seconds")
        ("content-timeout", po::value<int>(&options.content_timeout)->default_value(options.content_timeout),
            "content timeout in seconds")
        ("inherit-output", po::bool_switch(&options.inherit_output), "inherit child diagnostics output")
        ("gpu-accelerated", po::bool_switch(&options.gpu_accelerated), "enable GPU acceleration")
        ("no-gpu", po::bool_switch(&no_gpu), "disable GPU acceleration")
        ("proton", po::bool_switch(&proton), "launch through Proton")
        ("wine", po::bool_switch(&wine), "launch through Wine")
        ("umu", po::bool_switch(&umu), "launch through umu");
    // clang-format on

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(description).run(), vm);
        po::notify(vm);
    } catch (const po::error &error) {
        fmt::println(stderr, "error: [{}] {}", error, error.what());
        return 2;
    }

    if (vm.count("help") != 0) {
        fmt::print("Usage: {} [options]\n\n", argv[0]);
        fmt::print("{}", fmt::streamed(description));
        return 0;
    }

    if (const auto exit_code = resolve_graphics_api(vm, options)) return exit_code;
    if (const auto exit_code = resolve_capture_backend(vm, options)) return exit_code;
    if (const auto exit_code = resolve_environment(env_entries, options)) return exit_code;

    if (debug_rawdump) setenv("WCAS_DUMP_RAW_DEBUG", "1", 1);
    if (vm.count("test-dmabuf") != 0) options.test_dmabuf_fd = vm["test-dmabuf"].as<int>();
    if (no_gpu) options.gpu_accelerated = false;
    if (proton)
        options.mode = "proton";
    else if (wine)
        options.mode = "wine";
    else if (umu)
        options.mode = "umu";

    if (const auto exit_code = validate_timeouts(options)) return exit_code;

    export_debug_environment(options);
    return std::nullopt;
}

// ── launching ───────────────────────────────────────────────────────────────

/// A launched app plus the handles bound to it. Declaration order matters: the
/// Controller points into the app and must be destroyed before it.
struct Session {
    ComputerUse harness;
    ControlledApp app;
};

LaunchConfig make_launch_config(const Options &options) {
    LaunchConfig config = options.mode == "proton" ? LaunchConfig::proton(options.program)
                        : options.mode == "wine"   ? LaunchConfig::wine(options.program)
                        : options.mode == "umu"    ? LaunchConfig::umu(options.program)
                                                   : LaunchConfig(options.program);
    for (const auto &arg : options.args)
        config.arg(arg);
    config.graphics_api(options.graphics_api);
    config.gpu_accelerated(options.gpu_accelerated);
    config.inherit_diagnostics(options.inherit_output);
    if (!options.match_regex.empty()) config.match_regex(options.match_regex);
    for (const auto &entry : options.environment)
        config.env(entry.first, entry.second);
    return config;
}

optional<Session> launch_session(const Options &options) {
    std::error_code error;
    std::filesystem::create_directories(options.output_dir, error);

    auto harness_result = ComputerUse::builder()
                              .native_timeout(std::chrono::seconds(options.native_timeout))
                              .fallback_timeout(std::chrono::seconds(options.fallback_timeout))
                              .content_timeout(std::chrono::seconds(options.content_timeout))
                              .bridge_geometry(options.bridge_width, options.bridge_height)
                              .build();
    if (!harness_result) {
        fmt::println("{}", harness_result.error());
        return std::nullopt;
    }
    // std::move is required: `result` is move-only.
    auto harness = std::move(harness_result);

    auto launched = harness.value().launch(make_launch_config(options));
    if (!launched) {
        fmt::println("{}", launched.error());
        return std::nullopt;
    }
    auto app = std::move(launched.value());
    app.set_capture_backend(options.capture_backend);
    app.set_allow_activation(options.allow_activation);

    // Build the Session first; the Controller is created by Shell after the
    // final move, because it stores a pointer to ControlledApp. Creating it
    // before moving the app (or the Session) leaves a dangling pointer.
    return Session{std::move(harness.value()), std::move(app)};
}

// ── interactive shell ───────────────────────────────────────────────────────

struct FrameBarrier {
    uint64_t commits = 0;
    uint64_t frames = 0;
};

double clamp_axis(double value, int extent) {
    if (extent <= 0) return value;
    return std::clamp(value, 0.0, static_cast<double>(extent - 1));
}

bool is_quit_command(const std::string &command) { return command == "quit" || command == "exit" || command == "q"; }

/// Reads one command line at a time and runs it against a launched app.
class Shell {
public:
    Shell(Session session, Options options);

    void run();
    void serve();

private:
    using Handler = void (Shell::*)(std::istringstream &);

    struct CommandSpec {
        std::string_view name;
        Handler handler;
    };

    static const CommandSpec commands_[];

    // dispatch / setup
    bool dispatch(const std::string &command, std::istringstream &input);
    void create_terminal();
    void print_startup() const;
    boost::json::object handle_ipc(const boost::json::object &request);
    bool ensure_sdm_channel(std::size_t bytes, std::string &error);

    // cursor and frame bookkeeping
    void refresh_frame_size();
    void clamp_cursor();
    void sync_cursor_from_device();
    [[nodiscard]] FrameBarrier frame_barrier() const;
    bool wait_for_new_frame(const FrameBarrier &before, int timeout_ms) const;

    // input helpers
    void report_error(const std::error_code &error) const;
    void sleep_for_key_delay() const;
    void move_cursor();
    void move_to(double x, double y);
    void click_at(const std::string &arguments);
    void nudge(bool vertical, bool positive);
    void inject_key(const std::string &combination, std::chrono::milliseconds hold);
    [[nodiscard]] bool direction_delta(std::string direction, double distance, double &dx, double &dy) const;
    [[nodiscard]] bool try_click_hold(const std::string &token, std::istringstream &args);
    [[nodiscard]] std::string target_app_id() const;
    void select_index(int index);
    void select_by_name(const std::string &name);
    void capture_screenshot(bool wait_fresh);

    // commands
    void cmd_status(std::istringstream &input);
    void cmd_cursor(std::istringstream &input);
    void cmd_list(std::istringstream &input);
    void cmd_surfaces(std::istringstream &input);
    void cmd_activation(std::istringstream &input);
    void cmd_monitor(std::istringstream &input);
    void cmd_sessions(std::istringstream &input);
    void cmd_target(std::istringstream &input);
    void cmd_screenshot(std::istringstream &input);
    void cmd_drag(std::istringstream &input);
    void cmd_move(std::istringstream &input);
    void cmd_click(std::istringstream &input);
    void cmd_up(std::istringstream &input);
    void cmd_down(std::istringstream &input);
    void cmd_left(std::istringstream &input);
    void cmd_right(std::istringstream &input);
    void cmd_center(std::istringstream &input);
    void cmd_key(std::istringstream &input);
    void cmd_escape(std::istringstream &input);
    void cmd_type(std::istringstream &input);
    void cmd_testdmabuf(std::istringstream &input);

    Session session_;
    Options options_;
    Controller controller_;
    std::unique_ptr<sdm::Channel> sdm_channel_;
    std::string ipc_path_;
    cv::Size frame_;
    unsigned screenshot_ = 0;
    double mouse_x_ = -1;
    double mouse_y_ = -1;
    // Last compositor-reported position we saw, and whether the CLI cursor was
    // last set by the physical device (true) or by our own injection (false).
    double device_x_ = -1.0;
    double device_y_ = -1.0;
    bool cursor_is_device_ = false;
    std::unique_ptr<cmdly::Terminal> terminal_;
};

// clang-format off
const Shell::CommandSpec Shell::commands_[] = {
    {"status", &Shell::cmd_status},
    {"cursor", &Shell::cmd_cursor},
    {"pxy", &Shell::cmd_cursor},
    {"list", &Shell::cmd_list},
    {"windows", &Shell::cmd_list},
    {"ls", &Shell::cmd_list},
    {"surfaces", &Shell::cmd_surfaces},
    {"activation", &Shell::cmd_activation},
    {"monitor", &Shell::cmd_monitor},
    {"output", &Shell::cmd_monitor},
    {"sessions", &Shell::cmd_sessions},
    {"target", &Shell::cmd_target},
    {"select", &Shell::cmd_target},
    {"screenshot", &Shell::cmd_screenshot},
    {"ss", &Shell::cmd_screenshot},
    {"drag", &Shell::cmd_drag},
    {"move", &Shell::cmd_move},
    {"click", &Shell::cmd_click},
    {"up", &Shell::cmd_up},
    {"w", &Shell::cmd_up},
    {"down", &Shell::cmd_down},
    {"s", &Shell::cmd_down},
    {"left", &Shell::cmd_left},
    {"a", &Shell::cmd_left},
    {"right", &Shell::cmd_right},
    {"d", &Shell::cmd_right},
    {"center", &Shell::cmd_center},
    {"c", &Shell::cmd_center},
    {"key", &Shell::cmd_key},
    {"press", &Shell::cmd_key},
    {"k", &Shell::cmd_key},
    {"escape", &Shell::cmd_escape},
    {"esc", &Shell::cmd_escape},
    {"type", &Shell::cmd_type},
    {"testdmabuf", &Shell::cmd_testdmabuf},
};
// clang-format on

Shell::Shell(Session session, Options options)
    : session_(std::move(session))
    , options_(std::move(options)) {
    auto result = session_.app.controller();
    if (result) {
        controller_ = std::move(result.value());
    } else {
        fmt::println(stderr, "controller: {}", result.error().message());
    }
}

void Shell::run() {
    print_startup();
    create_terminal();

    std::string line;
    while (session_.app.is_alive()) {
        if (terminal_)
            line = terminal_->readLine("wcas> ");
        else if (!std::getline(std::cin, line)) {
            // A desktop shortcut has no terminal and stdin is usually /dev/null.
            // Do not interpret EOF as "quit" unless the caller explicitly asked
            // for CLI mode; otherwise keep supervising the game until it exits.
            if (!options_.cli) {
                while (session_.app.is_alive())
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            break;
        }

        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty()) continue;
        if (is_quit_command(command)) break;
        if (!dispatch(command, input)) fmt::println("unknown command");
    }

    session_.app.stop();
}

void Shell::serve() {
    std::string path = options_.socket_path.empty() ? ipc::default_socket_path() : options_.socket_path;
    if (path.empty()) {
        fmt::println(stderr, "ipc: XDG_RUNTIME_DIR is not set and no --socket was provided");
        session_.app.stop();
        return;
    }
    ipc_path_ = path;
    ipc::IpcServer server(std::move(path), [this](const boost::json::object &request) { return handle_ipc(request); });
    std::string error;
    if (!server.start(error)) {
        fmt::println(stderr, "ipc: {}", error);
        session_.app.stop();
        return;
    }
    ipc_path_ = server.path();
    fmt::println("IPC listening on {}", ipc_path_);
    g_ipc_stop = 0;
    std::signal(SIGINT, handle_ipc_stop);
    std::signal(SIGTERM, handle_ipc_stop);
    while (session_.app.is_alive() && g_ipc_stop == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();
    if (session_.app.is_alive()) session_.app.stop();
}

boost::json::object Shell::handle_ipc(const boost::json::object &request) {
    using boost::json::array;
    using boost::json::object;

    const auto *method_it = request.find("method");
    if (method_it == request.end() || !method_it->value().is_string()) return ipc::error(-32'600, "missing method");

    const std::string method(method_it->value().as_string());
    object params;
    if (const auto *params_it = request.find("params"); params_it != request.end() && params_it->value().is_object())
        params = params_it->value().as_object();

    const auto number = [&params](std::string_view key, double fallback) {
        const auto *it = params.find(key);
        return it == params.end() ? fallback : it->value().to_number<double>();
    };
    const auto text = [&params](std::string_view key) -> std::string {
        const auto *it = params.find(key);
        return it == params.end() ? std::string{} : std::string(it->value().as_string());
    };
    const auto ok = [](boost::json::value result = boost::json::object{}) { return ipc::success(std::move(result)); };
    const auto fail = [](int code, std::string message) { return ipc::error(code, std::move(message)); };

    if (method == "hello") {
        object result;
        result["protocol"] = ipc::kProtocolVersion;
        result["pid"] = session_.app.pid();
        result["transport"] = session_.app.transport() == Transport::xwayland ? "xwayland" : "native_wayland";
        result["socket"] = ipc_path_;
        array capabilities;
        for (const char *capability : {"capture", "input", "pointer", "keyboard", "windows", "stop", "sdm"}) {
            capabilities.emplace_back(capability);
        }
        result["capabilities"] = std::move(capabilities);
        refresh_frame_size();
        if (frame_.width > 0 && frame_.height > 0) {
            result["frame"] = object{
                { "width",  frame_.width},
                {"height", frame_.height}
            };
        }
        const auto windows = session_.app.windows();
        if (!windows.empty()) result["app_id"] = windows.front().app_id;
        return ok(std::move(result));
    }

    if (method == "status") {
        refresh_frame_size();
        const auto windows = session_.app.windows();
        object result;
        result["pid"] = session_.app.pid();
        result["alive"] = session_.app.is_alive();
        result["transport"] = session_.app.transport() == Transport::xwayland ? "xwayland" : "native_wayland";
        result["commit"] = session_.app.total_commit_count();
        result["frames"] = session_.app.presented_frames();
        result["windows"] = static_cast<std::uint64_t>(windows.size());
        result["socket"] = ipc_path_;
        if (frame_.width > 0 && frame_.height > 0) {
            result["frame"] = object{
                { "width",  frame_.width},
                {"height", frame_.height}
            };
        }
        return ok(std::move(result));
    }

    if (method == "capture") {
        auto captured = session_.app.capture();
        if (!captured) return fail(-32'001, captured.error().message());
        std::vector<unsigned char> encoded;
        if (!cv::imencode(".png", captured.value(), encoded)) return fail(-32'002, "PNG encoding failed");
        frame_ = cv::Size(captured.value().cols, captured.value().rows);
        object result;
        result["format"] = "png";
        result["width"] = frame_.width;
        result["height"] = frame_.height;
        result["data"] =
            ipc::base64_encode(std::string_view(reinterpret_cast<const char *>(encoded.data()), encoded.size()));
        return ok(std::move(result));
    }

    if (method == "capture_sdm") {
        auto captured = session_.app.capture();
        if (!captured) return fail(-32'001, captured.error().message());
        const cv::Mat &image = captured.value();
        if (image.depth() != CV_8U || (image.channels() != 3 && image.channels() != 4)) {
            return fail(-32'004, "unsupported capture format for SDM");
        }
        const std::size_t bytes = image.step[0] * image.rows;
        if (bytes > std::numeric_limits<std::uint32_t>::max()) {
            return fail(-32'005, "frame too large for SDM channel");
        }
        std::string error;
        if (!ensure_sdm_channel(bytes, error)) return fail(-32'005, std::move(error));

        sdm::FrameInfo info;
        info.width = static_cast<std::uint32_t>(image.cols);
        info.height = static_cast<std::uint32_t>(image.rows);
        info.stride = static_cast<std::uint32_t>(image.step[0]);
        info.format = image.channels() == 3 ? sdm::PixelFormat::Bgr8 : sdm::PixelFormat::Bgra8;
        info.timestamp_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (!sdm_channel_->write(image.data, bytes, info, error)) return fail(-32'006, std::move(error));

        frame_ = cv::Size(image.cols, image.rows);
        object result;
        result["channel"] = sdm_channel_->name();
        result["seq"] = sdm_channel_->latest_seq();
        result["width"] = image.cols;
        result["height"] = image.rows;
        result["stride"] = info.stride;
        result["format"] = image.channels() == 3 ? "bgr8" : "bgra8";
        result["slot_count"] = sdm_channel_->slot_count();
        result["slot_bytes"] = sdm_channel_->slot_bytes();
        return ok(std::move(result));
    }

    if (method == "pointer.move" || method == "pointer.click") {
        refresh_frame_size();
        const double x = clamp_axis(number("x", frame_.width / 2.0), frame_.width);
        const double y = clamp_axis(number("y", frame_.height / 2.0), frame_.height);
        auto result = method == "pointer.click" ? controller_.click({x, y}) : controller_.move({x, y});
        if (!result) return fail(-32'003, result.error().message());
        mouse_x_ = x;
        mouse_y_ = y;
        cursor_is_device_ = false;
        return ok();
    }

    if (method == "pointer.scroll") {
        refresh_frame_size();
        const double x = clamp_axis(number("x", frame_.width / 2.0), frame_.width);
        const double y = clamp_axis(number("y", frame_.height / 2.0), frame_.height);
        const int dx = static_cast<int>(number("dx", 0.0));
        const int dy = static_cast<int>(number("dy", 0.0));
        auto result = controller_.scroll({x, y}, {dx, dy});
        if (!result) return fail(-32'003, result.error().message());
        return ok();
    }

    if (method == "key") {
        const std::string combination = text("combo");
        if (combination.empty()) return fail(-32'602, "missing combo");
        const auto hold = std::chrono::milliseconds(static_cast<int>(number("hold_ms", 50.0)));
        auto result = controller_.key(combination, hold);
        if (!result) return fail(-32'003, result.error().message());
        return ok();
    }

    if (method == "type") {
        const std::string value = text("text");
        if (value.empty()) return fail(-32'602, "missing text");
        auto result = controller_.type_text(value);
        if (!result) return fail(-32'003, result.error().message());
        return ok();
    }

    if (method == "window.list") {
        const auto windows = session_.app.windows();
        array result;
        for (std::size_t index = 0; index < windows.size(); ++index) {
            const auto &window = windows[index];
            object item;
            item["index"] = static_cast<std::uint64_t>(index);
            item["app_id"] = window.app_id;
            item["title"] = window.title;
            item["width"] = window.width;
            item["height"] = window.height;
            item["pid"] = window.pid;
            item["selected"] = window.selected;
            result.emplace_back(std::move(item));
        }
        return ok(boost::json::value(std::move(result)));
    }

    if (method == "window.select") {
        const int index = static_cast<int>(number("index", -1.0));
        if (index < 0) return fail(-32'602, "missing index");
        auto result = session_.app.select_window(index);
        if (!result) return fail(-32'004, result.error().message());
        return ok();
    }

    if (method == "stop") {
        session_.app.stop();
        return ok();
    }

    if (method == "ping") return ok();

    return fail(-32'601, "method not found: " + method);
}

bool Shell::ensure_sdm_channel(std::size_t bytes, std::string &error) {
    if (sdm_channel_ && sdm_channel_->slot_bytes() >= bytes) return true;
    sdm_channel_.reset();
    if (bytes == 0 || bytes > std::numeric_limits<std::uint32_t>::max()) {
        error = "invalid SDM slot size";
        return false;
    }
    sdm::Config config;
    config.slot_count = 4;
    config.slot_bytes = static_cast<std::uint32_t>(bytes);
    auto channel = sdm::Channel::create(sdm::Channel::unique_name("wcas-sdm"), config, error);
    if (!channel) return false;
    sdm_channel_ = std::move(channel);
    return true;
}

bool Shell::dispatch(const std::string &command, std::istringstream &input) {
    for (const auto &spec : commands_) {
        if (spec.name != command) continue;
        (this->*spec.handler)(input);
        return true;
    }
    return false;
}

void Shell::create_terminal() {
    if (::isatty(STDIN_FILENO) == 0) return;

    auto io = std::make_shared<cmdly::StandardIO>();
    auto history = std::make_shared<cmdly::MemoryHistory>();
    auto completion = std::make_shared<cmdly::Completion>();
    for (const auto &spec : commands_)
        completion->insert(std::string(spec.name));

    terminal_ = std::make_unique<cmdly::Terminal>(io, history, completion);
    terminal_->setPromptStyle(cmdly::TextStyle(cmdly::Color::Cyan));
}

void Shell::print_startup() const {
    fmt::println("started pid= {} socket= {} transport= {}", session_.app.pid(), session_.harness.socket_name(),
        (session_.app.transport() == Transport::xwayland ? "xwayland" : "native"));
    fmt::println("capture-backend set, debug={}, output-dir={}, stale-timeout={}s", options_.debug.describe(),
        options_.output_dir, options_.stale_timeout);
}

// ── frame / cursor bookkeeping ──────────────────────────────────────────────

/// Live size of the app's current frame. Updated from the session whenever input
/// happens, so movement tracks a window that was resized after launch instead of
/// staying bounded by its startup size.
void Shell::refresh_frame_size() {
    if (const auto size = session_.app.frame_size()) frame_ = *size;
}

void Shell::clamp_cursor() {
    mouse_x_ = clamp_axis(mouse_x_, frame_.width);
    mouse_y_ = clamp_axis(mouse_y_, frame_.height);
}

void Shell::sync_cursor_from_device() {
    auto device = session_.app.cursor_position();
    if (!device) return;
    if (device_x_ < 0) {
        // First observation: adopt it only if the CLI has no position yet.
        device_x_ = device->x;
        device_y_ = device->y;
        if (mouse_x_ < 0 || mouse_y_ < 0) {
            mouse_x_ = device->x;
            mouse_y_ = device->y;
            cursor_is_device_ = true;
        }
        return;
    }
    // Only a *change* in the device position re-seeds the cursor, so a move we
    // just injected (which does not move the physical pointer) is not clobbered
    // by the stale device position.
    if (device->x == device_x_ && device->y == device_y_) return;
    // The physical mouse moved: adopt where the user actually is.
    device_x_ = device->x;
    device_y_ = device->y;
    mouse_x_ = device->x;
    mouse_y_ = device->y;
    cursor_is_device_ = true;
}

FrameBarrier Shell::frame_barrier() const {
    return FrameBarrier{session_.app.total_commit_count(), session_.app.presented_frames()};
}

/// After injecting input the app redraws asynchronously; block until a new
/// frame is on screen so the next capture reflects the input instead of a frame
/// that was already in flight. Prefer `presented_frames` (the app's
/// `wl_surface.frame` -> `wl_callback.done`), which is the strongest "new frame
/// presented" signal; fall back to commits for apps that never request frame
/// callbacks. No-op for XWayland (both counters are Wayland-only).
bool Shell::wait_for_new_frame(const FrameBarrier &before, int timeout_ms) const {
    if (session_.app.transport() != Transport::native_wayland) return false;
    const bool use_frames = before.frames > 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (session_.app.presented_frames() > before.frames) return true;
        if (!use_frames && session_.app.total_commit_count() > before.commits) {
            // Give the commit handler a beat to publish the frame metadata.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// ── input helpers ───────────────────────────────────────────────────────────

void Shell::report_error(const std::error_code &error) const {
    fmt::println(stderr, "[{}] {}", error, error.message());
}

void Shell::sleep_for_key_delay() const {
    if (options_.key_delay <= 0.0) return;
    fmt::println("key-delay: waiting {:.1f}s — focus the target window now", options_.key_delay);
    std::this_thread::sleep_for(std::chrono::duration<double>(options_.key_delay));
}

void Shell::move_cursor() {
    const auto barrier = frame_barrier();
    auto result = controller_.move({mouse_x_, mouse_y_});
    if (!result) {
        report_error(result.error());
        return;
    }
    cursor_is_device_ = false;
    wait_for_new_frame(barrier, 1'000);
}

void Shell::move_to(double x, double y) {
    const auto barrier = frame_barrier();
    auto result = controller_.move({x, y});
    if (!result) {
        report_error(result.error());
        return;
    }
    mouse_x_ = x;
    mouse_y_ = y;
    cursor_is_device_ = false;
    wait_for_new_frame(barrier, 1'000);
}

void Shell::click_at(const std::string &arguments) {
    double x = mouse_x_;
    double y = mouse_y_;
    std::istringstream nums(arguments);
    if (!(nums >> x >> y)) {
        if (mouse_x_ < 0 || mouse_y_ < 0) {
            fmt::println(stderr, "no known cursor position yet; run screenshot or move X Y");
            return;
        }
        x = mouse_x_;
        y = mouse_y_;
    }
    x = clamp_axis(x, frame_.width);
    y = clamp_axis(y, frame_.height);
    const auto barrier = frame_barrier();
    auto result = controller_.click({x, y});
    if (!result) {
        report_error(result.error());
        return;
    }
    mouse_x_ = x;
    mouse_y_ = y;
    cursor_is_device_ = false;
    wait_for_new_frame(barrier, 1'000);
}

void Shell::nudge(bool vertical, bool positive) {
    refresh_frame_size();
    sync_cursor_from_device();
    if (mouse_x_ < 0 || mouse_y_ < 0) {
        fmt::println(stderr, "no known cursor position yet; run screenshot first");
        return;
    }
    // Scale the step with the current window so a full traversal takes a similar
    // number of key presses at any window size (8% of the axis, matching the old
    // 100px step on a 1280px window).
    const double step = vertical ? (frame_.height > 0 ? frame_.height * 0.08 : 100.0)
                                 : (frame_.width > 0 ? frame_.width * 0.08 : 100.0);
    const double delta = positive ? step : -step;
    if (vertical)
        mouse_y_ += delta;
    else
        mouse_x_ += delta;
    clamp_cursor();
    move_cursor();
}

void Shell::inject_key(const std::string &combination, std::chrono::milliseconds hold) {
    const auto barrier = frame_barrier();
    sleep_for_key_delay();
    auto result = controller_.key(combination, hold);
    if (!result) {
        report_error(result.error());
        return;
    }
    wait_for_new_frame(barrier, 1'000);
}

bool Shell::direction_delta(std::string direction, double distance, double &dx, double &dy) const {
    for (char &c : direction)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (direction == "r" || direction == "right")
        dx = distance;
    else if (direction == "l" || direction == "left")
        dx = -distance;
    else if (direction == "d" || direction == "down")
        dy = distance;
    else if (direction == "t" || direction == "top" || direction == "up")
        dy = -distance;
    else if (direction == "c" || direction == "center")
        dx = dy = 0.0;
    else
        return false;
    return true;
}

/// Handles `click <hold> <direction> [distance]`; returns false when `token` is
/// not a hold duration, so the caller can treat the line as `click [x y]`.
bool Shell::try_click_hold(const std::string &token, std::istringstream &args) {
    const auto hold = parse_hold(token);
    if (!hold) return false;

    std::string direction;
    double distance = 0.0;
    args >> direction;
    args >> distance;
    double dx = 0.0;
    double dy = 0.0;
    if (!direction_delta(direction, distance, dx, dy)) {
        fmt::println(stderr,
            "usage: click <hold> <Top|Down|Left|Right|Center> [distance]  "
            "e.g. `click 4s R 300`");
        return true;
    }

    double x = mouse_x_;
    double y = mouse_y_;
    if (x < 0 || y < 0) {
        x = frame_.width > 0 ? frame_.width / 2.0 : 0.0;
        y = frame_.height > 0 ? frame_.height / 2.0 : 0.0;
    }
    const auto barrier = frame_barrier();
    auto result = controller_.press_drag({x, y}, {x + dx, y + dy}, *hold);
    if (!result) {
        report_error(result.error());
        return true;
    }
    mouse_x_ = x + dx;
    mouse_y_ = y + dy;
    cursor_is_device_ = false;
    wait_for_new_frame(barrier, 1'000);
    return true;
}

std::string Shell::target_app_id() const {
    for (const auto &w : session_.app.windows()) {
        if (w.selected && !w.app_id.empty()) return w.app_id;
    }
    for (const auto &w : session_.app.windows()) {
        if (!w.app_id.empty()) return w.app_id;
    }
    return {};
}

void Shell::select_index(int index) {
    auto result = session_.app.select_window(index);
    if (!result) {
        report_error(result.error());
        return;
    }
    fmt::println("target = [{}]", index);
}

void Shell::select_by_name(const std::string &name) {
    const auto wins = session_.app.windows();
    int found = -1;
    for (size_t i = 0; i < wins.size(); ++i) {
        if (wins[i].app_id == name || wins[i].title == name) {
            found = static_cast<int>(i);
            break;
        }
    }
    if (found < 0) {
        fmt::println(stderr, "no window matches '{}' (run `list`)", name);
        return;
    }
    auto result = session_.app.select_window(found);
    if (!result) {
        report_error(result.error());
        return;
    }
    fmt::println("target = [{}] app_id='{}' title='{}'", found, wins[found].app_id, wins[found].title);
}

void Shell::capture_screenshot(bool wait_fresh) {
    bool attempted = false;
    bool fresh = false;
    // Hard real-time: wait until a frame newer than this command exists.
    // wait_for_new_frame uses `presented_frames` (wl_surface.frame ->
    // wl_callback.done) when the app requests those, otherwise it falls back to
    // the commit counter (apps that only use wl_display.sync).
    if (wait_fresh && session_.app.transport() == Transport::native_wayland) {
        attempted = true;
        fresh = wait_for_new_frame(frame_barrier(), 1'000);
    }

    auto captured = session_.app.capture();
    if (!captured) {
        fmt::println(stderr, "capture failed: [{}] {}", captured.error(), captured.error().message());
        return;
    }
    auto name = fmt::format("screenshot-{:02}.png", ++screenshot_);
    const auto path = std::filesystem::path(options_.output_dir) / name;
    cv::imwrite(path.string(), captured.value());
    auto log = std::format("saved {} ({}x{})", path.string(), captured.value().cols, captured.value().rows);
    if (attempted) log += fmt::format("{}", fresh ? " fresh" : " stale");
    fmt::println("{}", log);
    frame_ = cv::Size(captured.value().cols, captured.value().rows);
    if (mouse_x_ < 0 || mouse_y_ < 0) {
        mouse_x_ = frame_.width / 2.0;
        mouse_y_ = frame_.height / 2.0;
    }
}

// ── commands ────────────────────────────────────────────────────────────────

void Shell::cmd_status(std::istringstream &) {
    refresh_frame_size();
    fmt::print("pid={} alive={} commit={} frames={}", session_.app.pid(), session_.app.is_alive(),
        session_.app.total_commit_count(), session_.app.presented_frames());
    if (frame_.width > 0 && frame_.height > 0) fmt::print(" window={}x{}", frame_.width, frame_.height);
    for (const auto &w : session_.app.windows()) {
        if (!w.selected) continue;
        fmt::print(" target_pid={} target='{}'", w.pid, w.title.empty() ? w.app_id : w.title);
    }
    fmt::println("");
}

void Shell::cmd_cursor(std::istringstream &) {
    refresh_frame_size();
    optional<double> cx;
    optional<double> cy;
    const char *source = "?";
    // Adopt the physical position only if the mouse actually moved since we last
    // looked; otherwise report where we last injected.
    sync_cursor_from_device();
    if (mouse_x_ >= 0 && mouse_y_ >= 0) {
        cx = mouse_x_;
        cy = mouse_y_;
        source = cursor_is_device_ ? "device" : "injected";
    }
    if (!cx || !cy) {
        fmt::println(
            "cursor: unknown (move the physical mouse over the app, "
            "or run screenshot/move/center first)");
        return;
    }
    fmt::print("cursor={},{} source={}", static_cast<ssize_t>(*cx), static_cast<ssize_t>(*cy), source);
    if (frame_.width > 0 && frame_.height > 0) fmt::print(" window={}x{}", frame_.width, frame_.height);
    fmt::println("");
}

void Shell::cmd_list(std::istringstream &) {
    const auto wins = session_.app.windows();
    if (wins.empty()) {
        fmt::println("no windows ({} sessions, none rendered) — try `sessions`", session_.app.sessions().size());
        return;
    }
    for (size_t i = 0; i < wins.size(); ++i) {
        const auto &w = wins[i];
        const char *origin = w.origin == WindowOrigin::Toplevel   ? "toplevel"
                           : w.origin == WindowOrigin::Subsurface ? "subsurface"
                                                                  : "bare";
        fmt::println("[{}]{} pid={} sid={} {}x{} [{}] app_id='{}' title='{}'", i, w.selected ? "*" : " ", w.pid,
            w.surface_id, w.width, w.height, origin, w.app_id, w.title);
    }
}

void Shell::cmd_surfaces(std::istringstream &) {
    const auto surfaces = session_.app.surfaces();
    if (surfaces.empty()) {
        fmt::println("no tracked surfaces");
        return;
    }
    for (const auto &s : surfaces) {
        fmt::println("pid={} sid={} role='{}' parent={} toplevel={} {}x{} frame={} seq={} app_id='{}' title='{}'",
            s.pid, s.surface_id, s.role, s.parent_surface_id, s.toplevel_id, s.width, s.height, s.has_frame ? 1 : 0,
            s.last_commit_seq, s.app_id, s.title);
    }
}

void Shell::cmd_activation(std::istringstream &input) {
    std::string value;
    input >> value;
    if (value == "on")
        options_.allow_activation = true;
    else if (value == "off")
        options_.allow_activation = false;
    session_.app.set_allow_activation(options_.allow_activation);
    fmt::println("activation = {}", options_.allow_activation ? "on" : "off");
}

void Shell::cmd_monitor(std::istringstream &input) {
    std::string name;
    input >> name;
    if (name.empty()) {
        fmt::println(stderr, "usage: monitor <DP-1|DP-2|HDMI-A-1|...>");
        return;
    }
    const auto cls = target_app_id();
    if (cls.empty()) {
        fmt::println("monitor: no window with an app_id (run `list`)");
        return;
    }
    const int rc = hyprctl_move_to_monitor(cls, name);
    fmt::println("monitor: '{}' -> {} ({})", cls, name, rc == 0 ? "ok" : "failed");
}

void Shell::cmd_sessions(std::istringstream &) {
    const auto sessions = session_.app.sessions();
    if (sessions.empty()) {
        fmt::println("no sessions");
        return;
    }
    for (const auto &s : sessions) {
        fmt::println(
            "pid={} connected={} matched={} windows={} surfaces={} toplevels={} kbd={} ptr={} keymap={} "
            "cmd='{}'",
            s.pid, s.connected, s.matched, s.windows, s.surfaces, s.toplevels, s.has_keyboard ? 1 : 0,
            s.has_pointer ? 1 : 0, s.has_keymap ? 1 : 0, s.command);
    }
}

void Shell::cmd_target(std::istringstream &input) {
    int index = -1;
    if (input >> index) {
        select_index(index);
        return;
    }
    std::string name;
    std::getline(input >> std::ws, name);
    select_by_name(name);
}

void Shell::cmd_screenshot(std::istringstream &input) {
    // Hard real-time by default: wait until the app presents a frame that is at
    // least as new as this command, so `ss` never returns a frame that predates
    // the interaction. `ss now` skips the wait.
    std::string mode;
    input >> mode;
    capture_screenshot(mode != "now" && mode != "raw");
}

void Shell::cmd_drag(std::istringstream &input) {
    // `drag <dx> <dy>`: move the view/cursor by a 2D delta **without** pressing
    // any button, so a pointer-locked camera follows the motion and the game
    // never sees a click.
    refresh_frame_size();
    sync_cursor_from_device();
    std::string arguments;
    std::getline(input >> std::ws, arguments);
    std::istringstream args(arguments);
    double dx = 0.0;
    double dy = 0.0;
    if (!(args >> dx >> dy)) {
        fmt::println(stderr, "usage: drag <dx> <dy>  e.g. `drag 300 -120`");
        return;
    }
    double x = mouse_x_;
    double y = mouse_y_;
    if (x < 0 || y < 0) {
        x = frame_.width > 0 ? frame_.width / 2.0 : 0.0;
        y = frame_.height > 0 ? frame_.height / 2.0 : 0.0;
    }
    const auto barrier = frame_barrier();
    auto result = controller_.rel_drag({x, y}, {x + dx, y + dy});
    if (!result) {
        report_error(result.error());
        return;
    }
    mouse_x_ = x + dx;
    mouse_y_ = y + dy;
    cursor_is_device_ = false;
    wait_for_new_frame(barrier, 1'000);
}

void Shell::cmd_move(std::istringstream &input) {
    refresh_frame_size();
    sync_cursor_from_device();
    double x = mouse_x_;
    double y = mouse_y_;
    if (!(input >> x >> y)) {
        if (mouse_x_ < 0 || mouse_y_ < 0) {
            fmt::println(stderr, "no known cursor position yet; run screenshot or move X Y");
            return;
        }
        x = mouse_x_;
        y = mouse_y_;
    }
    move_to(clamp_axis(x, frame_.width), clamp_axis(y, frame_.height));
}

void Shell::cmd_click(std::istringstream &input) {
    refresh_frame_size();
    sync_cursor_from_device();
    std::string arguments;
    std::getline(input >> std::ws, arguments);
    std::istringstream args(arguments);
    std::string first;
    if ((args >> first) && try_click_hold(first, args)) return;
    click_at(arguments);
}

void Shell::cmd_up(std::istringstream &) { nudge(true, false); }

void Shell::cmd_down(std::istringstream &) { nudge(true, true); }

void Shell::cmd_left(std::istringstream &) { nudge(false, false); }

void Shell::cmd_right(std::istringstream &) { nudge(false, true); }

void Shell::cmd_center(std::istringstream &) {
    auto captured = session_.app.capture();
    if (!captured) {
        fmt::println("capture failed: {}", captured.error());
        return;
    }
    frame_ = cv::Size(captured.value().cols, captured.value().rows);
    mouse_x_ = frame_.width / 2.0;
    mouse_y_ = frame_.height / 2.0;
    const auto barrier = frame_barrier();
    auto result = controller_.move({mouse_x_, mouse_y_});
    if (!result) {
        report_error(result.error());
        return;
    }
    cursor_is_device_ = false;
    wait_for_new_frame(barrier, 1'000);
}

void Shell::cmd_key(std::istringstream &input) {
    std::string combination;
    std::getline(input >> std::ws, combination);
    if (combination.empty()) {
        fmt::println(stderr,
            "usage: key <combination> [hold]  e.g. `key Enter`, `key Alt-C`, "
            "`key ctrl+s`, `key Space 3s`, `key Enter 500ms`");
        return;
    }
    // An optional trailing duration makes it a long press.
    std::chrono::milliseconds hold{0};
    split_hold(combination, hold);
    inject_key(combination, hold);
}

void Shell::cmd_escape(std::istringstream &) { inject_key("Escape", std::chrono::milliseconds{0}); }

void Shell::cmd_type(std::istringstream &input) {
    std::string text;
    std::getline(input >> std::ws, text);
    const auto barrier = frame_barrier();
    sleep_for_key_delay();
    auto result = controller_.type_text(text);
    if (!result) {
        report_error(result.error());
        return;
    }
    wait_for_new_frame(barrier, 1'000);
}

void Shell::cmd_testdmabuf(std::istringstream &input) {
    int fd = options_.test_dmabuf_fd.value_or(-1);
    input >> fd;
    if (fd >= 0) {
        run_dmabuf_probe(fd);
        return;
    }
    const auto tracked = tracked_dmabuf_fds();
    if (tracked.empty()) {
        fmt::println("no tracked DMA-BUF fds (use --probe-dmabuf)");
        return;
    }
    for (const int candidate : tracked)
        run_dmabuf_probe(candidate);
}

}   // namespace

int main(int argc, char **argv) {
    // cmdly writes the prompt, the echoed line and completion output straight to
    // fd 1 with write(2), bypassing stdio. Keep stdout unbuffered so fmt output
    // cannot sit in the FILE buffer and be flushed after later prompt/echo
    // writes (visible as "wcas> ..." appearing before startup/command output
    // when stdout is a pipe or a file). Must run before any stdout I/O.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Options options;
    if (const auto exit_code = parse_arguments(argc, argv, options)) return *exit_code;

    // --test-dmabuf <FD>: probe a specific DMA-BUF fd and exit.
    if (options.test_dmabuf_fd) {
        run_dmabuf_probe(*options.test_dmabuf_fd);
        return 0;
    }

    if (options.program.empty()) {
        print_usage();
        return 2;
    }

    if (options.detach && ::setsid() < 0) {
        fmt::println(stderr, "warning: setsid failed: {}", std::strerror(errno));
    }

    auto session = launch_session(options);
    if (!session) return 1;

    const bool serve = options.serve;
    Shell shell(std::move(*session), std::move(options));
    if (serve) {
        shell.serve();
    } else {
        shell.run();
    }
    return 0;
}
