#include "SKL/WCAS/Private/IPC/Protocol.h"
#include "SKL/WCAS/Private/SDM/Channel.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace ipc = skl::wcas::ipc;
namespace sdm = skl::wcas::sdm;

namespace {

void usage() {
    std::cerr << "usage: wcasctl [--socket PATH] [--out FILE] COMMAND [ARGS]\n"
                 "\n"
                 "commands:\n"
                 "  list                       list available WCAS IPC sockets\n"
                 "  hello                      handshake and print capabilities\n"
                 "  status                     session/process/frame status\n"
                 "  windows                    list windows\n"
                 "  select INDEX               select a window\n"
                 "  capture [--out FILE]       capture a PNG (default capture.png)\n"
                 "  capture-sdm [--out FILE]   capture a raw BGR frame via shared memory (default capture.bgr)\n"
                 "  move X Y                   move the pointer\n"
                 "  click X Y                  click at frame coordinates\n"
                 "  scroll X Y DX DY           scroll at frame coordinates\n"
                 "  key COMBO                  inject a key combination (e.g. ctrl+a)\n"
                 "  type TEXT                  inject Unicode text\n"
                 "  stop                       stop the controlled app\n";
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
        std::cerr << "write failed: " << error.message() << "\n";
        return false;
    }

    boost::json::value response;
    if (!ipc::read_message(socket, response, error)) {
        std::cerr << "read failed: " << error.message() << "\n";
        return false;
    }
    if (!response.is_object()) {
        std::cerr << "invalid response\n";
        return false;
    }

    const auto &object = response.as_object();
    if (const auto it = object.find("error"); it != object.end()) {
        const auto &detail = it->value().as_object();
        std::cerr << "error " << detail.at("code").as_int64() << ": " << detail.at("message").as_string() << "\n";
        return false;
    }
    if (const auto it = object.find("result"); it != object.end()) {
        result = it->value();
        return true;
    }
    std::cerr << "response without result/error\n";
    return false;
}

void print_json(const boost::json::value &value) { std::cout << boost::json::serialize(value) << "\n"; }

bool save_png(const boost::json::value &result, const std::string &output) {
    if (!result.is_object()) {
        std::cerr << "capture: invalid result\n";
        return false;
    }
    const auto &object = result.as_object();
    const auto data_it = object.find("data");
    if (data_it == object.end() || !data_it->value().is_string()) {
        std::cerr << "capture: missing data\n";
        return false;
    }
    const auto bytes = ipc::base64_decode(data_it->value().as_string());
    if (bytes.empty()) {
        std::cerr << "capture: invalid base64 payload\n";
        return false;
    }
    std::ofstream file(output, std::ios::binary);
    if (!file) {
        std::cerr << "capture: cannot open " << output << "\n";
        return false;
    }
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::cout << "saved " << output << " (" << bytes.size() << " bytes)\n";
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
    std::vector<std::string> arguments;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (argument == "--out" && i + 1 < argc) {
            output = argv[++i];
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        } else {
            arguments.emplace_back(argument);
        }
    }

    if (arguments.empty()) {
        usage();
        return 2;
    }

    const std::string command = arguments.front();
    if (command == "list") {
        const std::string directory = ipc::runtime_dir();
        if (directory.empty()) {
            std::cerr << "XDG_RUNTIME_DIR is not set\n";
            return 1;
        }
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) break;
            if (!entry.is_socket(error) || entry.path().extension() != ".sock") continue;
            if (!socket_is_live(entry.path())) continue;
            std::cout << entry.path().string() << "\n";
        }
        return 0;
    }

    if (socket_path.empty()) socket_path = discover_socket();
    if (socket_path.empty()) {
        std::cerr << "no WCAS IPC socket found (start proton-inject with --serve)\n";
        return 1;
    }

    boost::asio::io_context io;
    ipc::local_socket socket(io);
    boost::system::error_code error;
    socket.connect(boost::asio::local::stream_protocol::endpoint(socket_path), error);
    if (error) {
        std::cerr << "connect " << socket_path << ": " << error.message() << "\n";
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
    } else if (command == "status") {
        // no params
    } else if (command == "windows") {
        rpc_method = "window.list";
    } else if (command == "select" && arguments.size() >= 2) {
        rpc_method = "window.select";
        params["index"] = std::stoi(arguments[1]);
        has_params = true;
    } else if ((command == "move" || command == "click") && arguments.size() >= 3) {
        rpc_method = "pointer." + command;
        params["x"] = std::stod(arguments[1]);
        params["y"] = std::stod(arguments[2]);
        has_params = true;
    } else if (command == "scroll" && arguments.size() >= 5) {
        rpc_method = "pointer.scroll";
        params["x"] = std::stod(arguments[1]);
        params["y"] = std::stod(arguments[2]);
        params["dx"] = std::stod(arguments[3]);
        params["dy"] = std::stod(arguments[4]);
        has_params = true;
    } else if (command == "key" && arguments.size() >= 2) {
        params["combo"] = join_arguments(arguments, 1);
        has_params = true;
    } else if (command == "type" && arguments.size() >= 2) {
        params["text"] = join_arguments(arguments, 1);
        has_params = true;
    } else if (command != "capture" && command != "capture-sdm" && command != "stop") {
        std::cerr << "unknown or incomplete command: " << command << "\n";
        usage();
        return 2;
    }

    if (!call(socket, id++, rpc_method, has_params ? &params : nullptr, result)) return 1;

    if (command == "capture") return save_png(result, output.empty() ? "capture.png" : output) ? 0 : 1;
    if (command == "capture-sdm") {
        const auto &object = result.as_object();
        const auto channel_it = object.find("channel");
        if (channel_it == object.end() || !channel_it->value().is_string()) {
            std::cerr << "capture-sdm: missing channel\n";
            return 1;
        }
        std::string sdm_error;
        auto channel = sdm::Channel::open(std::string(channel_it->value().as_string()), sdm_error);
        if (!channel) {
            std::cerr << "capture-sdm: open failed: " << sdm_error << "\n";
            return 1;
        }
        sdm::FrameView view;
        if (!channel->read_latest(view, sdm_error)) {
            std::cerr << "capture-sdm: read failed: " << sdm_error << "\n";
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
            std::cerr << "capture-sdm: cannot open " << path << "\n";
            return 1;
        }
        file.write(reinterpret_cast<const char *>(view.data), static_cast<std::streamsize>(view.size));
        channel->release(view);
        std::cout
            << "saved "
            << path
            << " ("
            << frame_size
            << " bytes, "
            << frame_width
            << "x"
            << frame_height
            << ", stride="
            << frame_stride
            << ")\n";
        return 0;
    }
    if (command == "stop") return 0;
    print_json(result);
    return 0;
}
