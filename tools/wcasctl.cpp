#include "SKL/WCAS/Private/IPC/Protocol.h"
#include "SKL/WCAS/Private/SDM/Channel.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include <fmt/std.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace ipc = skl::wcas::ipc;
namespace sdm = skl::wcas::sdm;
namespace po = boost::program_options;

namespace {

/// Commands are positional, but Boost only knows how to format options. Prefix
/// every command line with `--` in the description, then drop it again for the
/// help output, so the command table shares Boost's alignment and wrapping.
std::string strip_command_prefix(std::string text) {
    std::string result;
    result.reserve(text.size());
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line_end = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(start, line_end - start);
        if (line.starts_with("  --")) line.erase(2, 2);
        result += line;
        if (end != std::string::npos) result.push_back('\n');
        start = line_end + 1;
    }
    return result;
}

void usage(const po::options_description &commands, const po::options_description &options, FILE *stream = stderr) {
    fmt::print(stream, "usage: wcasctl [--socket PATH] [--out FILE] COMMAND [ARGS]\n\n");
    fmt::print(stream, "{}", strip_command_prefix(fmt::format("{}", fmt::streamed(commands))));
    fmt::print(stream, "\n{}", fmt::streamed(options));
}

/// Treat dash-prefixed tokens that are not global options as positional values.
/// Coordinates may be negative (`move -5 10`) and text/key arguments may start
/// with a dash (`type --flag`), which Boost would otherwise reject as options.
std::vector<po::option> positional_dash_arguments(std::vector<std::string> &tokens) {
    std::vector<po::option> parsed;
    if (tokens.empty()) return parsed;

    const std::string &token = tokens.front();
    if (token.size() < 2 || token.front() != '-') return parsed;
    if (token == "--") return parsed;

    static constexpr std::string_view global_options[] = {"-h", "--help", "--socket", "--out"};
    for (const std::string_view option : global_options) {
        if (token == option) return parsed;
    }
    if (token.starts_with("--socket=") || token.starts_with("--out=")) return parsed;

    po::option option;
    option.value.push_back(token);
    option.original_tokens.push_back(token);
    parsed.push_back(std::move(option));
    tokens.erase(tokens.begin());
    return parsed;
}

bool socket_is_live(const std::filesystem::path &path) {
    const std::string stem = path.stem().string();
    char *end = nullptr;
    const long pid = std::strtol(stem.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || pid <= 0) return true;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

std::string discover_socket() {
    if (const std::string latest = ipc::latest_socket_path();
        !latest.empty() && socket_is_live(std::filesystem::path(latest))) {
        return latest;
    }

    const std::string directory = ipc::runtime_dir();
    if (directory.empty()) return {};
    std::error_code error;
    std::string newest;
    std::filesystem::file_time_type newest_time{};
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_socket(error) || entry.path().extension() != ".sock") continue;
        if (!socket_is_live(entry.path())) continue;
        const auto time = entry.last_write_time(error);
        if (error) continue;
        if (newest.empty() || time > newest_time) {
            newest = entry.path().string();
            newest_time = time;
        }
    }
    return newest;
}

bool call(ipc::local_socket &socket, int id, std::string_view method, const boost::json::object *params,
    boost::json::value &result) {
    boost::json::object request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = std::string(method);
    if (params != nullptr) request["params"] = *params;

    boost::system::error_code error;
    if (!ipc::write_message(socket, request, error)) {
        fmt::println(stderr, "write failed: {}", error.message());
        return false;
    }

    boost::json::value response;
    if (!ipc::read_message(socket, response, error)) {
        fmt::println(stderr, "read failed: {}", error.message());
        return false;
    }
    if (!response.is_object()) {
        fmt::println(stderr, "invalid response");
        return false;
    }

    const auto &object = response.as_object();
    if (const auto it = object.find("error"); it != object.end()) {
        const auto &detail = it->value().as_object();
        fmt::println(stderr, "error {}: {}", detail.at("code").as_int64(), detail.at("message").as_string().c_str());
        return false;
    }
    if (const auto it = object.find("result"); it != object.end()) {
        result = it->value();
        return true;
    }
    fmt::println(stderr, "response without result/error");
    return false;
}

void print_json(const boost::json::value &value) { fmt::println("{}", boost::json::serialize(value)); }

bool save_png(const boost::json::value &result, const std::string &output) {
    if (!result.is_object()) {
        fmt::println(stderr, "capture: invalid result");
        return false;
    }
    const auto &object = result.as_object();
    const auto data_it = object.find("data");
    if (data_it == object.end() || !data_it->value().is_string()) {
        fmt::println(stderr, "capture: missing data");
        return false;
    }
    const auto bytes = ipc::base64_decode(data_it->value().as_string());
    if (bytes.empty()) {
        fmt::println(stderr, "capture: invalid base64 payload");
        return false;
    }
    std::ofstream file(output, std::ios::binary);
    if (!file) {
        fmt::println(stderr, "capture: cannot open {}", output);
        return false;
    }
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    fmt::println("saved {} ({} bytes)", output, bytes.size());
    return true;
}

std::string join_arguments(const std::vector<std::string> &arguments, std::size_t start) {
    std::string joined;
    for (std::size_t i = start; i < arguments.size(); ++i) {
        if (!joined.empty()) joined.push_back(' ');
        joined += arguments[i];
    }
    return joined;
}

}   // namespace

int main(int argc, char **argv) {
    std::string socket_path;
    std::string output;
    std::string command;
    std::vector<std::string> arguments;

    po::options_description options("options");
    // clang-format off
    options.add_options()
        ("help,h", "show this help")
        ("socket", po::value<std::string>(&socket_path),
            "IPC socket path (default: auto-discover)")
        ("out", po::value<std::string>(&output),
            "output file for capture commands");
    // clang-format on

    // Help-only description. Commands are positional arguments, so this table
    // is not added to the parser; it only drives the generated help output.
    po::options_description commands("commands", 120, 40);
    // clang-format off
    commands.add_options()
        ("list", "list available WCAS IPC sockets")
        ("hello", "handshake and print capabilities")
        ("status", "session/process/frame status")
        ("windows", "list windows")
        ("select", po::value<std::string>()->value_name("INDEX"), "select a window")
        ("capture", po::value<std::string>()->value_name("[--out FILE]"),
            "capture a PNG (default capture.png)")
        ("capture-sdm", po::value<std::string>()->value_name("[--out FILE]"),
            "capture a raw BGR frame via shared memory (default capture.bgr)")
        ("frames-subscribe", po::value<std::string>()->value_name("[FPS]"),
            "start continuous SDM frame publishing")
        ("frames-unsubscribe", "stop continuous SDM frame publishing")
        ("frames-status", "show SDM publisher status")
        ("move", po::value<std::string>()->value_name("X Y"), "move the pointer")
        ("relative", po::value<std::string>()->value_name("DX DY"), "buttonless relative pointer motion")
        ("click", po::value<std::string>()->value_name("X Y"), "click at frame coordinates")
        ("down", po::value<std::string>()->value_name("X Y [BUTTON]"),
            "press a pointer button (left|right|middle)")
        ("up", po::value<std::string>()->value_name("[BUTTON]"), "release a pointer button")
        ("touch-down", po::value<std::string>()->value_name("X Y [CONTACT]"), "touch contact down")
        ("touch-move", po::value<std::string>()->value_name("X Y [CONTACT]"), "touch contact move")
        ("touch-up", po::value<std::string>()->value_name("[CONTACT]"), "touch contact up")
        ("drag", po::value<std::string>()->value_name("X1 Y1 X2 Y2 [HOLD_MS]"),
            "press, move and release at frame coordinates")
        ("scroll", po::value<std::string>()->value_name("X Y DX DY"), "scroll at frame coordinates")
        ("key", po::value<std::string>()->value_name("COMBO"), "inject a key combination (e.g. ctrl+a)")
        ("key-down", po::value<std::string>()->value_name("COMBO"), "press and hold a key combination")
        ("key-up", po::value<std::string>()->value_name("COMBO"), "release a key combination")
        ("type", po::value<std::string>()->value_name("TEXT"), "inject Unicode text")
        ("stop", "stop the controlled app");
    // clang-format on

    po::options_description hidden;
    // clang-format off
    hidden.add_options()
        ("command", po::value<std::string>(&command), "command")
        ("args", po::value<std::vector<std::string>>(&arguments), "command arguments");
    // clang-format on

    po::options_description all;
    all.add(options).add(hidden);

    po::positional_options_description positional;
    positional.add("command", 1);
    positional.add("args", -1);

    try {
        po::variables_map variables;
        po::store(po::command_line_parser(argc, argv)
                      .options(all)
                      .positional(positional)
                      .extra_style_parser(positional_dash_arguments)
                      .run(),
            variables);
        po::notify(variables);
        if (variables.count("help") != 0) {
            usage(commands, options, stdout);
            return 0;
        }
    } catch (const po::error &error) {
        fmt::println(stderr, "wcasctl: {}", error.what());
        usage(commands, options);
        return 2;
    }

    if (command.empty()) {
        usage(commands, options);
        return 2;
    }

    if (command == "list") {
        const std::string directory = ipc::runtime_dir();
        if (directory.empty()) {
            fmt::println(stderr, "XDG_RUNTIME_DIR is not set");
            return 1;
        }
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) break;
            if (!entry.is_socket(error) || entry.path().extension() != ".sock") continue;
            if (!socket_is_live(entry.path())) continue;
            fmt::println("{}", entry.path().string());
        }
        return 0;
    }

    if (socket_path.empty()) socket_path = discover_socket();
    if (socket_path.empty()) {
        fmt::println(stderr, "no WCAS IPC socket found (start proton-inject with --serve)");
        return 1;
    }

    boost::asio::io_context io;
    ipc::local_socket socket(io);
    boost::system::error_code error;
    socket.connect(boost::asio::local::stream_protocol::endpoint(socket_path), error);
    if (error) {
        fmt::println(stderr, "connect {}: {}", socket_path, error.message());
        return 1;
    }

    boost::json::value result;
    int id = 1;
    if (!call(socket, id++, "hello", nullptr, result)) return 1;
    if (command == "hello") {
        print_json(result);
        return 0;
    }

    boost::json::object params;
    bool has_params = false;
    std::string rpc_method = command;
    if (command == "capture-sdm") {
        rpc_method = "capture_sdm";
    } else if (command == "frames-subscribe") {
        rpc_method = "frames.subscribe";
        if (!arguments.empty()) {
            params["fps"] = std::stod(arguments[0]);
            has_params = true;
        }
    } else if (command == "frames-unsubscribe") {
        rpc_method = "frames.unsubscribe";
    } else if (command == "frames-status") {
        rpc_method = "frames.status";
    } else if (command == "status") {
        // no params
    } else if (command == "windows") {
        rpc_method = "window.list";
    } else if (command == "select" && !arguments.empty()) {
        rpc_method = "window.select";
        params["index"] = std::stoi(arguments[0]);
        has_params = true;
    } else if ((command == "move" || command == "click") && arguments.size() >= 2) {
        rpc_method = "pointer." + command;
        params["x"] = std::stod(arguments[0]);
        params["y"] = std::stod(arguments[1]);
        has_params = true;
    } else if (command == "relative" && arguments.size() >= 2) {
        rpc_method = "pointer.relative";
        params["dx"] = std::stod(arguments[0]);
        params["dy"] = std::stod(arguments[1]);
        has_params = true;
    } else if (command == "down" && arguments.size() >= 2) {
        rpc_method = "pointer.down";
        params["x"] = std::stod(arguments[0]);
        params["y"] = std::stod(arguments[1]);
        if (arguments.size() >= 3) params["button"] = arguments[2];
        has_params = true;
    } else if (command == "up") {
        rpc_method = "pointer.up";
        if (!arguments.empty()) params["button"] = arguments[0];
        has_params = true;
    } else if (command == "drag" && arguments.size() >= 4) {
        rpc_method = "pointer.drag";
        params["from_x"] = std::stod(arguments[0]);
        params["from_y"] = std::stod(arguments[1]);
        params["to_x"] = std::stod(arguments[2]);
        params["to_y"] = std::stod(arguments[3]);
        if (arguments.size() >= 5) params["hold_ms"] = std::stod(arguments[4]);
        has_params = true;
    } else if ((command == "touch-down" || command == "touch-move") && arguments.size() >= 2) {
        rpc_method = "touch." + command.substr(6);
        params["x"] = std::stod(arguments[0]);
        params["y"] = std::stod(arguments[1]);
        if (arguments.size() >= 3) params["contact"] = std::stoi(arguments[2]);
        has_params = true;
    } else if (command == "touch-up") {
        rpc_method = "touch.up";
        if (!arguments.empty()) params["contact"] = std::stoi(arguments[0]);
        has_params = true;
    } else if (command == "scroll" && arguments.size() >= 4) {
        rpc_method = "pointer.scroll";
        params["x"] = std::stod(arguments[0]);
        params["y"] = std::stod(arguments[1]);
        params["dx"] = std::stod(arguments[2]);
        params["dy"] = std::stod(arguments[3]);
        has_params = true;
    } else if (command == "key" && !arguments.empty()) {
        params["combo"] = join_arguments(arguments, 0);
        has_params = true;
    } else if (command == "key-down" && !arguments.empty()) {
        rpc_method = "key.down";
        params["combo"] = join_arguments(arguments, 0);
        has_params = true;
    } else if (command == "key-up" && !arguments.empty()) {
        rpc_method = "key.up";
        params["combo"] = join_arguments(arguments, 0);
        has_params = true;
    } else if (command == "type" && !arguments.empty()) {
        params["text"] = join_arguments(arguments, 0);
        has_params = true;
    } else if (command != "capture"
               && command != "capture-sdm"
               && command != "stop"
               && command != "frames-subscribe"
               && command != "frames-unsubscribe"
               && command != "frames-status"
               && command != "down"
               && command != "up"
               && command != "drag"
               && command != "key-down"
               && command != "key-up"
               && command != "relative"
               && command != "touch-down"
               && command != "touch-move"
               && command != "touch-up") {
        fmt::println(stderr, "unknown or incomplete command: {}", command);
        usage(commands, options);
        return 2;
    }

    if (!call(socket, id++, rpc_method, has_params ? &params : nullptr, result)) return 1;

    if (command == "capture") return save_png(result, output.empty() ? "capture.png" : output) ? 0 : 1;
    if (command == "capture-sdm") {
        const auto &object = result.as_object();
        const auto channel_it = object.find("channel");
        if (channel_it == object.end() || !channel_it->value().is_string()) {
            fmt::println(stderr, "capture-sdm: missing channel");
            return 1;
        }
        std::string sdm_error;
        auto channel = sdm::Channel::open(std::string(channel_it->value().as_string()), sdm_error);
        if (!channel) {
            fmt::println(stderr, "capture-sdm: open failed: {}", sdm_error);
            return 1;
        }
        sdm::FrameView view;
        if (!channel->read_latest(view, sdm_error)) {
            fmt::println(stderr, "capture-sdm: read failed: {}", sdm_error);
            return 1;
        }
        const std::string path = output.empty() ? "capture.bgr" : output;
        const std::size_t frame_size = view.size;
        const auto frame_width = view.info.width;
        const auto frame_height = view.info.height;
        const auto frame_stride = view.info.stride;
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            channel->release(view);
            fmt::println(stderr, "capture-sdm: cannot open {}", path);
            return 1;
        }
        file.write(reinterpret_cast<const char *>(view.data), static_cast<std::streamsize>(view.size));
        channel->release(view);
        fmt::println(
            "saved {} ({} bytes, {}x{}, stride={})", path, frame_size, frame_width, frame_height, frame_stride);
        return 0;
    }
    if (command == "stop") return 0;
    print_json(result);
    return 0;
}
