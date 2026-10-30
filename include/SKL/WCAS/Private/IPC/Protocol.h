#pragma once

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skl::wcas::ipc {

/// Wire protocol version reported by `hello`.
inline constexpr std::uint32_t kProtocolVersion = 1;
/// Upper bound for one length-prefixed JSON message.
inline constexpr std::size_t kMaxMessageSize = 32u * 1'024u * 1'024u;

using local_socket = boost::asio::local::stream_protocol::socket;

std::string base64_encode(std::string_view data);
std::string base64_encode(const std::uint8_t *data, std::size_t size);
std::vector<std::uint8_t> base64_decode(std::string_view text);

/// Sends one `uint32_be length + JSON` message.
bool write_message(local_socket &socket, const boost::json::value &value, boost::system::error_code &error);
/// Reads one `uint32_be length + JSON` message.
bool read_message(local_socket &socket, boost::json::value &value, boost::system::error_code &error);

boost::json::object success(boost::json::value result);
boost::json::object error(int code, std::string message);

/// `$XDG_RUNTIME_DIR/wcas`, or an empty string when XDG_RUNTIME_DIR is unset.
std::string runtime_dir();
/// Default per-session socket: `$XDG_RUNTIME_DIR/wcas/<pid>.sock`.
std::string default_socket_path();
/// Reads the `latest` symlink/file under the runtime directory.
std::string latest_socket_path();

}   // namespace skl::wcas::ipc
