#include "SKL/WCAS/Private/IPC/Protocol.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/json.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace ipc = skl::wcas::ipc;

namespace {

void test_base64() {
    std::string data;
    for (int i = 0; i < 256; ++i)
        data.push_back(static_cast<char>(i));
    data += "你好，WCAS";

    const std::string encoded = ipc::base64_encode(data);
    assert(!encoded.empty());
    const auto decoded = ipc::base64_decode(encoded);
    assert(decoded.size() == data.size());
    assert(std::string(decoded.begin(), decoded.end()) == data);
}

void test_responses() {
    boost::json::object result;
    result["value"] = 42;
    const auto ok = ipc::success(std::move(result));
    assert(ok.contains("result"));
    assert(ok.at("result").as_object().at("value").as_int64() == 42);

    const auto failure = ipc::error(-32'601, "not found");
    assert(failure.at("error").as_object().at("code").as_int64() == -32'601);
    assert(failure.at("error").as_object().at("message").as_string() == "not found");
}

void test_framing() {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    boost::asio::io_context io;
    ipc::local_socket writer(io);
    ipc::local_socket reader(io);
    writer.assign(boost::asio::local::stream_protocol(), fds[0]);
    reader.assign(boost::asio::local::stream_protocol(), fds[1]);

    boost::json::object message;
    message["jsonrpc"] = "2.0";
    message["id"] = 7;
    message["method"] = "ping";

    boost::system::error_code error;
    assert(ipc::write_message(writer, message, error));
    assert(!error);

    boost::json::value received;
    assert(ipc::read_message(reader, received, error));
    assert(!error);
    assert(received.as_object().at("method").as_string() == "ping");
    assert(received.as_object().at("id").as_int64() == 7);

    writer.close();
    reader.close();
}

}   // namespace

int main() {
    test_base64();
    test_responses();
    test_framing();
    std::cout << "ipc protocol: ok\n";
    return 0;
}
