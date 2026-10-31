#include "SKL/WCAS/Private/IPC/Server.h"

#include <boost/asio/socket_base.hpp>
#include <boost/asio/error.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>
#include <utility>

namespace skl::wcas::ipc {

IpcServer::IpcServer(std::string socket_path, Handler handler)
    : path_(std::move(socket_path))
    , handler_(std::move(handler)) {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start(std::string &error) {
    if (path_.empty()) {
        error = "empty IPC socket path";
        return false;
    }
    if (!handler_) {
        error = "empty IPC handler";
        return false;
    }

    std::error_code fs_error;
    const std::filesystem::path socket_path(path_);
    const std::filesystem::path directory = socket_path.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, fs_error);
        if (fs_error) {
            error = "create " + directory.string() + ": " + fs_error.message();
            return false;
        }
        ::chmod(directory.c_str(), 0700);

        // Remove sockets left behind by crashed sessions. The file name is the
        // host PID, so a missing process is enough to identify a stale entry.
        std::error_code scan_error;
        for (const auto &entry : std::filesystem::directory_iterator(directory, scan_error)) {
            if (scan_error) break;
            if (entry.path().extension() != ".sock") continue;
            const std::string stem = entry.path().stem().string();
            char *end = nullptr;
            const long pid = std::strtol(stem.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || pid <= 0) continue;
            if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
            }
        }
    }

    ::unlink(path_.c_str());

    io_ = std::make_unique<boost::asio::io_context>();
    acceptor_ = std::make_unique<boost::asio::local::stream_protocol::acceptor>(*io_);

    boost::system::error_code asio_error;
    const boost::asio::local::stream_protocol::endpoint endpoint(path_);
    acceptor_->open(endpoint.protocol(), asio_error);
    if (asio_error) {
        error = "open " + path_ + ": " + asio_error.message();
        return false;
    }
    acceptor_->bind(endpoint, asio_error);
    if (asio_error) {
        error = "bind " + path_ + ": " + asio_error.message();
        return false;
    }
    ::chmod(path_.c_str(), 0600);
    acceptor_->listen(boost::asio::socket_base::max_listen_connections, asio_error);
    if (asio_error) {
        error = "listen " + path_ + ": " + asio_error.message();
        return false;
    }
    acceptor_->non_blocking(true, asio_error);
    if (asio_error) {
        error = "non-blocking " + path_ + ": " + asio_error.message();
        return false;
    }

    if (!directory.empty()) {
        const std::filesystem::path latest = directory / "latest";
        std::filesystem::remove(latest, fs_error);
        std::filesystem::create_symlink(socket_path.filename(), latest, fs_error);
        owns_latest_ = !fs_error;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    std::fprintf(stderr, "[wcas-ipc] listening on %s\n", path_.c_str());
    return true;
}

void IpcServer::run() {
    while (running_.load(std::memory_order_acquire)) {
        auto socket = std::make_shared<local_socket>(*io_);
        boost::system::error_code error;
        acceptor_->accept(*socket, error);
        if (error) {
            if (!running_.load(std::memory_order_acquire)) break;
            if (error != boost::asio::error::would_block && error != boost::asio::error::try_again) {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        {
            std::lock_guard lock(client_mutex_);
            client_ = socket;
        }
        serve_client(*socket);
        {
            std::lock_guard lock(client_mutex_);
            client_.reset();
        }
    }
}

void IpcServer::serve_client(local_socket &socket) {
    struct ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(socket.native_handle(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0
        || credentials.uid != ::getuid()) {
        std::fprintf(stderr, "[wcas-ipc] rejected client (peer credentials)\n");
        return;
    }

    while (running_.load(std::memory_order_acquire)) {
        boost::json::value request;
        boost::system::error_code io_error;
        if (!read_message(socket, request, io_error)) break;
        if (!request.is_object()) {
            const auto response = ipc::error(-32'600, "request must be a JSON object");
            write_message(socket, response, io_error);
            continue;
        }

        const auto &request_object = request.as_object();
        boost::json::object response;
        try {
            response = handler_(request_object);
        } catch (const std::exception &exception) {
            response = ipc::error(-32'603, exception.what());
        } catch (...) {
            response = ipc::error(-32'603, "internal handler error");
        }

        response["jsonrpc"] = "2.0";
        if (request_object.contains("id")) {
            response["id"] = request_object.at("id");
            if (!write_message(socket, response, io_error)) break;
        }
    }
}

void IpcServer::stop() {
    running_.store(false, std::memory_order_release);

    if (acceptor_) {
        boost::system::error_code ignored;
        acceptor_->close(ignored);
    }
    {
        std::lock_guard lock(client_mutex_);
        if (client_) {
            boost::system::error_code ignored;
            client_->close(ignored);
        }
    }
    if (thread_.joinable()) thread_.join();

    ::unlink(path_.c_str());
    if (owns_latest_) {
        std::error_code fs_error;
        const std::filesystem::path latest = std::filesystem::path(path_).parent_path() / "latest";
        const auto target = std::filesystem::read_symlink(latest, fs_error);
        if (!fs_error && target == std::filesystem::path(path_).filename()) std::filesystem::remove(latest, fs_error);
    }
}

}   // namespace skl::wcas::ipc
