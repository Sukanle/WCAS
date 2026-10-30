#include "SKL/WCAS/Private/IPC/Server.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace ipc = skl::wcas::ipc;

namespace {

boost::json::object handle(const boost::json::object &request) {
    const auto method = request.at("method").as_string();
    if (method == "echo") {
        boost::json::object result;
        result["echo"] = request.at("params").as_object().at("value");
        return ipc::success(std::move(result));
    }
    if (method == "stop-test") {
        return ipc::success(boost::json::object{});
    }
    return ipc::error(-32'601, "method not found");
}

}   // namespace

int main() {
    const std::filesystem::path socket_path =
        std::filesystem::temp_directory_path() / ("wcas-ipc-test-" + std::to_string(::getpid()) + ".sock");
    std::filesystem::remove(socket_path);

    ipc::IpcServer server(socket_path.string(), &handle);
    std::string error;
    assert(server.start(error));
    assert(error.empty());

    boost::asio::io_context io;
    ipc::local_socket client(io);
    boost::system::error_code asio_error;
    client.connect(boost::asio::local::stream_protocol::endpoint(socket_path.string()), asio_error);
    assert(!asio_error);

    boost::json::object request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "echo";
    request["params"] = boost::json::object{
        {"value", 42}
    };
    assert(ipc::write_message(client, request, asio_error));
    assert(!asio_error);

    boost::json::value response;
    assert(ipc::read_message(client, response, asio_error));
    assert(!asio_error);
    assert(response.as_object().at("id").as_int64() == 1);
    assert(response.as_object().at("result").as_object().at("echo").as_int64() == 42);

    request = boost::json::object{
        {"jsonrpc",     "2.0"},
        {     "id",         2},
        { "method", "missing"}
    };
    assert(ipc::write_message(client, request, asio_error));
    assert(!asio_error);
    assert(ipc::read_message(client, response, asio_error));
    assert(!asio_error);
    assert(response.as_object().at("error").as_object().at("code").as_int64() == -32'601);

    client.close();
    server.stop();
    assert(!std::filesystem::exists(socket_path));

    std::cout << "ipc server: ok\n";
    return 0;
}
