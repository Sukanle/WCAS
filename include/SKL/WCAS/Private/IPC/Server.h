#pragma once

#include "SKL/WCAS/Private/IPC/Protocol.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace skl::wcas::ipc {

/// A small, single-client Unix-domain-socket JSON-RPC server.
///
/// The handler receives a request object (with `method` and optional `params`)
/// and returns an object containing either `result` or `error`. The server adds
/// `jsonrpc` and echoes `id`. Requests without an `id` are treated as
/// notifications and receive no response.
class IpcServer {
public:
    using Handler = std::function<boost::json::object(const boost::json::object &)>;

    IpcServer(std::string socket_path, Handler handler);
    ~IpcServer();

    IpcServer(const IpcServer &) = delete;
    IpcServer &operator=(const IpcServer &) = delete;

    bool start(std::string &error);
    void stop();

    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    void run();
    void serve_client(local_socket &socket);

    std::string path_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::unique_ptr<boost::asio::io_context> io_;
    std::unique_ptr<boost::asio::local::stream_protocol::acceptor> acceptor_;
    std::thread thread_;
    std::mutex client_mutex_;
    std::shared_ptr<local_socket> client_;
    bool owns_latest_ = false;
};

}   // namespace skl::wcas::ipc
