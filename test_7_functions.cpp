#include "skl/wcas/wcas.h"
#include "skl/wcas/dmabuf_probe.h"

#include <opencv2/imgcodecs.hpp>
#include <boost/program_options.hpp>
#include <fmt/std.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace skl::wcas;

/// Debug log categories, mirroring the Rust `Debug` bitflags.
struct DebugFlags {
    bool capture = false;
    bool dmabuf = false;
    bool all = false;

    [[nodiscard]] std::string describe() const {
        std::vector<std::string> parts;
        if (all || capture) parts.emplace_back("capture");
        if (all || dmabuf) parts.emplace_back("dmabuf");
        if (parts.empty()) return "none";
        std::string joined;
        for (const auto &part : parts) {
            if (!joined.empty()) joined += ", ";
            joined += part;
        }
        return joined;
    }
};

std::optional<CaptureBackend> parse_backend(const std::string &value) {
    if (value == "client" || value == "client-buffer" || value == "cache") return CaptureBackend::client_buffer;
    if (value == "kms" || value == "drm") return CaptureBackend::kms;
    if (value == "shm" || value == "wlshm") return CaptureBackend::shm;
    if (value == "dmabuf") return CaptureBackend::dmabuf;
    return std::nullopt;
}

void print_help() {
    fmt::print(
        "commands: screenshot|ss [now], up|w, down|s, left|a, right|d, center|c,\n"
        "          move <x> <y>, click [<x> <y>], cursor|pxy,\n"
        "          key|press <combo> (e.g. Enter, Space, ctrl+s, alt+F4), escape|esc, type <text>,\n"
        "          list|ls, sessions, target <index|app_id|title>,\n"
        "          testdmabuf [<fd>], status, help|h, quit|exit|q\n"
        "          (ss waits for a freshly presented frame; `ss now` captures immediately)\n");
}

}   // namespace

int main(int argc, char **argv) {
    std::string program;
    std::string mode = "native";
    bool cli = false;
    bool inherit_output = false;
    bool gpu_accelerated = false;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> environment;
    auto graphics_api = GraphicsApi::vulkan;
    auto capture_backend = CaptureBackend::client_buffer;
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
    std::optional<int> test_dmabuf_fd;

    namespace po = boost::program_options;
    po::options_description options("wcas-test-7-functions options");

    // clang-format off
    options.add_options()
        ("help,h", "help message")
        ("program,p", po::value<std::string>(&program), "executable to launch")
        ("arg,s", po::value<std::vector<std::string>>(&args), "argument passed to the program (repeatable)")
        ("env,e", po::value<std::vector<std::string>>(), "environment variable KEY=VALUE (repeatable)")
        ("cli,c", po::bool_switch(&cli), "use CLI mode")
        ("output-dir,o", po::value<std::string>(&output_dir)->default_value(output_dir), "output directory")
        ("bridge-width,x", po::value<int>(&bridge_width)->default_value(bridge_width), "bridge width")
        ("bridge-height,y", po::value<int>(&bridge_height)->default_value(bridge_height), "bridge height")
        ("graphics-api,g", po::value<std::string>(), "vulkan (vk/dxvk) or opengl (gl/wined3d)")
        ("capture-backend,b", po::value<std::string>(), "client (default), kms, shm, dmabuf")
        ("match-regex,r", po::value<std::string>(&match_regex), "extra regex for child-window processes (default: program dir)")
        ("debug-all", po::bool_switch(&debug.all), "enable all debug categories")
        ("debug-capture", po::bool_switch(&debug.capture), "enable capture debug logs")
        ("debug-dmabuf", po::bool_switch(&debug.dmabuf), "enable dmabuf debug logs")
        ("debug-rawdump", po::bool_switch(), "dump raw frames to disk")
        ("test-dmabuf", po::value<int>(), "probe a specific DMA-BUF fd and exit")
        ("probe-dmabuf", po::bool_switch(&probe_dmabuf), "probe DMA-BUF sync capability")
        ("stale-timeout", po::value<int>(&stale_timeout)->default_value(stale_timeout), "stale frame timeout in seconds")
        ("native-timeout", po::value<int>(&native_timeout)->default_value(native_timeout), "native startup timeout in seconds")
        ("fallback-timeout", po::value<int>(&fallback_timeout)->default_value(fallback_timeout), "fallback timeout in seconds")
        ("content-timeout", po::value<int>(&content_timeout)->default_value(content_timeout), "content timeout in seconds")
        ("inherit-output", po::bool_switch(&inherit_output), "inherit child diagnostics output")
        ("gpu-accelerated", po::bool_switch(&gpu_accelerated), "enable GPU acceleration")
        ("no-gpu", po::bool_switch(), "disable GPU acceleration")
        ("proton", po::bool_switch(), "launch through Proton")
        ("wine", po::bool_switch(), "launch through Wine")
        ("umu", po::bool_switch(), "launch through umu");
    // clang-format on

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(options).run(), vm);
        po::notify(vm);
    } catch (const po::error &error) {
        fmt::println(stderr, "error: [{}] {}", error, error.what());
        return 2;
    }

    if (vm.count("help")) {
        fmt::print("Usage: {} [options]\n\n", argv[0]);
        fmt::print("{}", fmt::streamed(options));
        return 0;
    }

    if (vm.count("graphics-api")) {
        const auto value = vm["graphics-api"].as<std::string>();
        if (value == "vulkan" || value == "vk" || value == "dxvk")
            graphics_api = GraphicsApi::vulkan;
        else if (value == "opengl" || value == "gl" || value == "wined3d")
            graphics_api = GraphicsApi::opengl;
        else {
            fmt::println(stderr, "error: 未知图形 API: {}，可选值: vulkan (vk/dxvk), opengl (gl/wined3d)", value);
            return 2;
        }
    }

    if (vm.count("capture-backend")) {
        const auto value = vm["capture-backend"].as<std::string>();
        const auto parsed = parse_backend(value);
        if (!parsed) {
            fmt::println(stderr, "error: 未知截图后端: {}，可选值: client (默认), kms, shm, dmabuf", value);
            return 2;
        }
        capture_backend = *parsed;
    }

    if (vm.count("env")) {
        for (const auto &entry : vm["env"].as<std::vector<std::string>>()) {
            const auto separator = entry.find('=');
            if (separator == std::string::npos || separator == 0) {
                fmt::println(stderr, "error: --env expects KEY=VALUE");
                return 2;
            }
            environment.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
        }
    }

    if (vm["debug-rawdump"].as<bool>()) setenv("WCAS_DUMP_RAW_DEBUG", "1", 1);
    if (vm.count("test-dmabuf")) test_dmabuf_fd = vm["test-dmabuf"].as<int>();
    if (vm["no-gpu"].as<bool>()) gpu_accelerated = false;
    if (vm["proton"].as<bool>())
        mode = "proton";
    else if (vm["wine"].as<bool>())
        mode = "wine";
    else if (vm["umu"].as<bool>())
        mode = "umu";

    if (stale_timeout <= 0) {
        fmt::println("error: --stale-timeout requires a positive value");
        return 2;
    }
    if (native_timeout <= 0) {
        fmt::println("error: --native-timeout requires a positive value");
        return 2;
    }
    if (fallback_timeout <= 0) {
        fmt::println("error: --fallback-timeout requires a positive value");
        return 2;
    }
    if (content_timeout <= 0) {
        fmt::println("error: --content-timeout requires a positive value");
        return 2;
    }

    // Debug categories are surfaced through WCAS_DEBUG (the C++ log gate).
    if (debug.all || debug.capture || debug.dmabuf) setenv("WCAS_DEBUG", "1", 1);
    if (probe_dmabuf) setenv("WCAS_DMABUF_PROBE", "1", 1);

    // --test-dmabuf <FD>: probe a specific DMA-BUF fd and exit.
    if (test_dmabuf_fd.has_value()) {
        fmt::print("\n═══ DMA-BUF 同步能力测试 (FD={}) ═══\n", *test_dmabuf_fd);
        DmaBufSyncProbe probe(*test_dmabuf_fd);
        fmt::print("{}", probe.run_all().report());
        return 0;
    }

    if (program.empty()) {
        fmt::println(
            "usage: wcas-test-7-functions --program EXEC [--arg ARG] "
            "[--proton|--wine|--umu] [--cli] [--capture-backend B] [--debug-all] "
            "[--native-timeout SEC] [--output-dir DIR] [--test-dmabuf FD]");
        return 2;
    }

    std::error_code error;
    std::filesystem::create_directories(output_dir, error);

    auto harness_result = ComputerUse::builder()
                              .native_timeout(std::chrono::seconds(native_timeout))
                              .fallback_timeout(std::chrono::seconds(fallback_timeout))
                              .content_timeout(std::chrono::seconds(content_timeout))
                              .bridge_geometry(bridge_width, bridge_height)
                              .build();
    if (!harness_result) {
        fmt::println("{}", harness_result.error());
        return 1;
    }
    // std::move is required: `result` is move-only.
    auto harness = std::move(harness_result);

    LaunchConfig config = mode == "proton" ? LaunchConfig::proton(program)
                        : mode == "wine"   ? LaunchConfig::wine(program)
                        : mode == "umu"    ? LaunchConfig::umu(program)
                                           : LaunchConfig(program);
    for (const auto &arg : args)
        config.arg(arg);
    config.graphics_api(graphics_api);
    config.gpu_accelerated(gpu_accelerated);
    config.inherit_diagnostics(inherit_output);
    if (!match_regex.empty()) config.match_regex(match_regex);
    for (const auto &entry : environment)
        config.env(entry.first, entry.second);

    auto launched = harness.value().launch(config);
    if (!launched) {
        fmt::println("{}", launched.error());
        return 1;
    }
    auto app = std::move(launched.value());
    app.set_capture_backend(capture_backend);

    fmt::println("started pid= {} socket= {} transport= {}", app.pid(), harness.value().socket_name(),
        (app.transport() == Transport::xwayland ? "xwayland" : "native"));
    fmt::println(
        "capture-backend set, debug={}, output-dir={}, stale-timeout={}s", debug.describe(), output_dir, stale_timeout);

    print_help();

    unsigned screenshot = 0;
    double mouse_x = -1;
    double mouse_y = -1;
    // Live size of the app's current frame. Updated from the session whenever
    // input happens, so movement tracks a window that was resized after launch
    // instead of staying bounded by its startup size.
    int frame_width = 0;
    int frame_height = 0;
    auto refresh_frame_size = [&] {
        if (auto size = app.frame_size()) {
            frame_width = size->width;
            frame_height = size->height;
        }
    };
    auto clamp_cursor = [&] {
        if (frame_width > 0) mouse_x = std::clamp(mouse_x, 0.0, static_cast<double>(frame_width - 1));
        if (frame_height > 0) mouse_y = std::clamp(mouse_y, 0.0, static_cast<double>(frame_height - 1));
    };
    // Last compositor-reported position we saw, and whether the CLI cursor was
    // last set by the physical device (true) or by our own injection (false).
    // Only a *change* in the device position re-seeds the cursor, so a move we
    // just injected (which does not move the physical pointer) is not clobbered
    // by the stale device position.
    double device_x = -1.0;
    double device_y = -1.0;
    bool cursor_is_device = false;
    auto sync_cursor_from_device = [&] {
        auto device = app.cursor_position();
        if (!device) return;
        if (device_x < 0) {
            // First observation: adopt it only if the CLI has no position yet.
            device_x = device->x;
            device_y = device->y;
            if (mouse_x < 0 || mouse_y < 0) {
                mouse_x = device->x;
                mouse_y = device->y;
                cursor_is_device = true;
            }
            return;
        }
        if (device->x != device_x || device->y != device_y) {
            // The physical mouse moved: adopt where the user actually is.
            device_x = device->x;
            device_y = device->y;
            mouse_x = device->x;
            mouse_y = device->y;
            cursor_is_device = true;
        }
    };
    // After injecting input the app redraws asynchronously; block until a new
    // frame is on screen so the next capture reflects the input instead of a
    // frame that was already in flight. Prefer `presented_frames` (the app's
    // `wl_surface.frame` -> `wl_callback.done`), which is the strongest "new
    // frame presented" signal; fall back to commits for apps that never request
    // frame callbacks. No-op for XWayland (both counters are Wayland-only).
    struct FrameBarrier {
        std::uint64_t commits = 0;
        std::uint64_t frames = 0;
    };
    auto frame_barrier = [&] { return FrameBarrier{app.total_commit_count(), app.presented_frames()}; };
    auto wait_for_new_frame = [&](const FrameBarrier &before, int timeout_ms) {
        if (app.transport() != Transport::native_wayland) return false;
        const bool use_frames = before.frames > 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (app.presented_frames() > before.frames) return true;
            if (!use_frames && app.total_commit_count() > before.commits) {
                // Give the commit handler a beat to publish the frame metadata.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    std::string line;
    while (app.is_alive() && std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty()) continue;

        if (command == "quit" || command == "exit" || command == "q") break;

        if (command == "status") {
            refresh_frame_size();
            fmt::print("pid={} alive={} commit={} frames={}", app.pid(), app.is_alive(), app.total_commit_count(),
                app.presented_frames());
            if (frame_width > 0 && frame_height > 0) fmt::print(" window={}x{}", frame_width, frame_height);
            for (const auto &w : app.windows()) {
                if (!w.selected) continue;
                fmt::print(" target_pid={} target='{}'", w.pid, w.title.empty() ? w.app_id : w.title);
            }
            fmt::println("");
            continue;
        }
        if (command == "cursor" || command == "pxy") {
            refresh_frame_size();
            std::optional<double> cx;
            std::optional<double> cy;
            const char *source = "?";
            // Adopt the physical position only if the mouse actually moved since
            // we last looked; otherwise report where we last injected.
            sync_cursor_from_device();
            if (mouse_x >= 0 && mouse_y >= 0) {
                cx = mouse_x;
                cy = mouse_y;
                source = cursor_is_device ? "device" : "injected";
            }
            if (!cx || !cy) {
                fmt::println(
                    "cursor: unknown (move the physical mouse over the app, "
                    "or run screenshot/move/center first)");
            } else {
                fmt::print("cursor={},{} source={}", static_cast<ssize_t>(*cx), static_cast<ssize_t>(*cy), source);
                if (frame_width > 0 && frame_height > 0) fmt::print(" window={}x{}", frame_width, frame_height);
                fmt::println("");
            }
            continue;
        }
        if (command == "help" || command == "h") {
            print_help();
            continue;
        }
        if (command == "list" || command == "windows" || command == "ls") {
            const auto wins = app.windows();
            if (wins.empty()) {
                fmt::println("no windows ({} sessions, none rendered) — try `sessions`", app.sessions().size());
            } else {
                for (std::size_t i = 0; i < wins.size(); ++i) {
                    const auto &w = wins[i];
                    fmt::println("[{}]{} pid={} {}x{} app_id='{}' title='{}'", i, w.selected ? "*" : " ", w.pid,
                        w.width, w.height, w.app_id, w.title);
                }
            }
            continue;
        }
        if (command == "sessions") {
            const auto sessions = app.sessions();
            if (sessions.empty()) {
                fmt::println("no sessions");
            } else {
                for (const auto &s : sessions) {
                    fmt::println("pid={} connected={} matched={} windows={}", s.pid, s.connected, s.matched, s.windows);
                }
            }
            continue;
        }
        if (command == "target" || command == "select") {
            int index = -1;
            if (input >> index) {
                auto result = app.select_window(index);
                if (!result)
                    fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
                else
                    fmt::println("target = [{}]", index);
            } else {
                std::string name;
                std::getline(input >> std::ws, name);
                const auto wins = app.windows();
                int found = -1;
                for (std::size_t i = 0; i < wins.size(); ++i) {
                    if (wins[i].app_id == name || wins[i].title == name) {
                        found = static_cast<int>(i);
                        break;
                    }
                }
                if (found < 0) {
                    fmt::println(stderr, "no window matches '{}' (run `list`)", name);
                } else if (auto result = app.select_window(found); !result) {
                    fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
                } else {
                    fmt::println("target = [{}] app_id='{}' title='{}'", found, wins[found].app_id, wins[found].title);
                }
            }
            continue;
        }
        if (command == "screenshot" || command == "ss") {
            // Hard real-time by default: wait until the app presents a frame
            // that is at least as new as this command, so `ss` never returns a
            // frame that predates the interaction. `ss now` skips the wait.
            std::string mode;
            input >> mode;
            const bool wait_fresh = mode != "now" && mode != "raw";
            bool attempted = false;
            bool fresh = false;
            // Hard real-time: wait until a frame newer than this command exists.
            // wait_for_new_frame uses `presented_frames` (wl_surface.frame ->
            // wl_callback.done) when the app requests those, otherwise it falls
            // back to the commit counter (apps that only use wl_display.sync).
            if (wait_fresh && app.transport() == Transport::native_wayland) {
                attempted = true;
                fresh = wait_for_new_frame(frame_barrier(), 1'000);
            }
            auto frame = app.capture();
            if (!frame) {
                fmt::println(stderr, "capture failed: [{}] {}", frame.error(), frame.error().message());
            } else {
                auto name = fmt::format("screenshot-{:02}.png", ++screenshot);
                const auto path = std::filesystem::path(output_dir) / name;
                cv::imwrite(path.string(), frame.value().image);
                auto log =
                    std::format("saved {} ({}x{})", path.string(), frame.value().width(), frame.value().height());
                if (attempted) log += fmt::format("{}", fresh ? " fresh" : " stale");
                fmt::println("{}", log);
                frame_width = frame.value().width();
                frame_height = frame.value().height();
                if (mouse_x < 0 || mouse_y < 0) {
                    mouse_x = frame_width / 2.0;
                    mouse_y = frame_height / 2.0;
                }
            }
            continue;
        }
        if (command == "move" || command == "click") {
            refresh_frame_size();
            sync_cursor_from_device();
            double x = mouse_x;
            double y = mouse_y;
            if (!(input >> x >> y)) {
                if (mouse_x < 0 || mouse_y < 0) {
                    fmt::println(stderr, "no known cursor position yet; run screenshot or move X Y");
                    continue;
                }
                x = mouse_x;
                y = mouse_y;
            }
            // Keep the coordinates inside the current window; a resized window
            // must not be addressed with the startup size.
            if (frame_width > 0) x = std::clamp(x, 0.0, static_cast<double>(frame_width - 1));
            if (frame_height > 0) y = std::clamp(y, 0.0, static_cast<double>(frame_height - 1));
            const auto barrier = frame_barrier();
            auto result = command == "click" ? app.click(x, y) : app.move_mouse(x, y);
            if (!result)
                fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
            else {
                mouse_x = x;
                mouse_y = y;
                cursor_is_device = false;
                wait_for_new_frame(barrier, 1'000);
            }
            continue;
        }
        if (command == "up"
            || command == "w"
            || command == "down"
            || command == "s"
            || command == "left"
            || command == "a"
            || command == "right"
            || command == "d") {
            refresh_frame_size();
            sync_cursor_from_device();
            if (mouse_x < 0 || mouse_y < 0) {
                fmt::println(stderr, "no known cursor position yet; run screenshot first");
                continue;
            }
            const bool vertical = command == "up" || command == "w" || command == "down" || command == "s";
            const bool positive = command == "down" || command == "s" || command == "right" || command == "d";
            // Scale the step with the current window so a full traversal takes a
            // similar number of key presses at any window size (8% of the axis,
            // matching the old 100px step on a 1280px window).
            const double step = vertical ? (frame_height > 0 ? frame_height * 0.08 : 100.0)
                                         : (frame_width > 0 ? frame_width * 0.08 : 100.0);
            const double delta = positive ? step : -step;
            if (vertical)
                mouse_y += delta;
            else
                mouse_x += delta;
            clamp_cursor();
            const auto barrier = frame_barrier();
            auto result = app.move_mouse(mouse_x, mouse_y);
            if (!result)
                fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
            else {
                cursor_is_device = false;
                wait_for_new_frame(barrier, 1'000);
            }
            continue;
        }
        if (command == "center" || command == "c") {
            auto frame = app.capture();
            if (!frame) {
                fmt::println("capture failed: {}", frame.error());
                continue;
            }
            frame_width = frame.value().width();
            frame_height = frame.value().height();
            mouse_x = frame_width / 2.0;
            mouse_y = frame_height / 2.0;
            const auto barrier = frame_barrier();
            auto result = app.move_mouse(mouse_x, mouse_y);
            if (!result)
                fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
            else {
                cursor_is_device = false;
                wait_for_new_frame(barrier, 1'000);
            }
            continue;
        }
        if (command == "key" || command == "press" || command == "k") {
            std::string combination;
            std::getline(input >> std::ws, combination);
            if (combination.empty()) {
                fmt::println(stderr,
                    "usage: key <combination>  e.g. `key Enter`, `key Space`, "
                    "`key ctrl+s`, `key alt+F4`");
                continue;
            }
            const auto barrier = frame_barrier();
            auto result = app.press_key(combination);
            if (!result)
                fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
            else
                wait_for_new_frame(barrier, 1'000);
            continue;
        }
        if (command == "escape" || command == "esc") {
            const auto barrier = frame_barrier();
            auto result = app.press_key("Escape");
            if (!result)
                fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
            else
                wait_for_new_frame(barrier, 1'000);
            continue;
        }
        if (command == "type") {
            std::string text;
            std::getline(input >> std::ws, text);
            const auto barrier = frame_barrier();
            auto result = app.type_text(text);
            if (!result)
                fmt::println(stderr, "[{}] {}", result.error(), result.error().message());
            else
                wait_for_new_frame(barrier, 1'000);
            continue;
        }
        if (command == "testdmabuf") {
            int fd = test_dmabuf_fd.value_or(-1);
            input >> fd;
            if (fd < 0) {
                const auto tracked = tracked_dmabuf_fds();
                if (tracked.empty()) {
                    fmt::println("no tracked DMA-BUF fds (use --probe-dmabuf)");
                } else {
                    for (const int candidate : tracked) {
                        fmt::print("\n═══ DMA-BUF 同步能力测试 (FD={}) ═══\n", candidate);
                        DmaBufSyncProbe probe(candidate);
                        fmt::print("{}", probe.run_all().report());
                    }
                }
            } else {
                fmt::print("\n═══ DMA-BUF 同步能力测试 (FD={}) ═══\n", fd);
                DmaBufSyncProbe probe(fd);
                fmt::print("{}", probe.run_all().report());
            }
            continue;
        }

        fmt::println("unknown command");
        (void)cli;
    }

    app.stop();
    return 0;
}
