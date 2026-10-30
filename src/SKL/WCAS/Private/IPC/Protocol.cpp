#include "SKL/WCAS/Private/IPC/Protocol.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <arpa/inet.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace skl::wcas::ipc {
namespace {

constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}   // namespace

std::string base64_encode(const std::uint8_t *data, std::size_t size) {
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? kBase64Alphabet[triple & 0x3F] : '=');
    }
    return out;
}

std::string base64_encode(std::string_view data) {
    return base64_encode(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char c : text) {
        if (c == '=') break;
        const int value = base64_value(c);
        if (value < 0) {
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return {};
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

bool write_message(local_socket &socket, const boost::json::value &value, boost::system::error_code &error) {
    const std::string payload = boost::json::serialize(value);
    if (payload.size() > kMaxMessageSize) {
        error = boost::asio::error::make_error_code(boost::asio::error::message_size);
        return false;
    }
    const std::uint32_t length = htonl(static_cast<std::uint32_t>(payload.size()));
    boost::asio::write(socket, boost::asio::buffer(&length, sizeof(length)), error);
    if (error) return false;
    boost::asio::write(socket, boost::asio::buffer(payload), error);
    return !error;
}

bool read_message(local_socket &socket, boost::json::value &value, boost::system::error_code &error) {
    std::uint32_t length = 0;
    boost::asio::read(socket, boost::asio::buffer(&length, sizeof(length)), error);
    if (error) return false;
    length = ntohl(length);
    if (length == 0 || length > kMaxMessageSize) {
        error = boost::asio::error::make_error_code(boost::asio::error::message_size);
        return false;
    }
    std::string payload(length, '\0');
    boost::asio::read(socket, boost::asio::buffer(payload), error);
    if (error) return false;
    value = boost::json::parse(payload, error);
    return !error;
}

boost::json::object success(boost::json::value result) {
    boost::json::object response;
    response["result"] = std::move(result);
    return response;
}

boost::json::object error(int code, std::string message) {
    boost::json::object detail;
    detail["code"] = code;
    detail["message"] = std::move(message);
    boost::json::object response;
    response["error"] = std::move(detail);
    return response;
}

std::string runtime_dir() {
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg == nullptr || *xdg == '\0') return {};
    return (std::filesystem::path(xdg) / "wcas").string();
}

std::string default_socket_path() {
    const std::string dir = runtime_dir();
    if (dir.empty()) return {};
    return (std::filesystem::path(dir) / (std::to_string(::getpid()) + ".sock")).string();
}

std::string latest_socket_path() {
    const std::string dir = runtime_dir();
    if (dir.empty()) return {};
    const std::filesystem::path latest = std::filesystem::path(dir) / "latest";
    std::error_code error;
    const auto target = std::filesystem::read_symlink(latest, error);
    if (!error) {
        if (target.is_absolute()) return target.string();
        return (latest.parent_path() / target).string();
    }
    // Fallback for filesystems without symlink support: `latest` may be a file
    // containing the socket path.
    std::ifstream file(latest);
    std::string line;
    if (file && std::getline(file, line) && !line.empty()) return line;
    return {};
}

}   // namespace skl::wcas::ipc
