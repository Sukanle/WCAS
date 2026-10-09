#include "xwayland.h"

#include <xcb/xcb.h>

#include <csignal>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace skl {
namespace wcas {
namespace {

/// 128-bit MIT-MAGIC-COOKIE-1 value rendered as hex.
std::string generate_cookie() {
    std::random_device device;
    std::mt19937_64 generator(device());
    std::uniform_int_distribution<int> distribution(0, 15);
    static constexpr char digits[] = "0123456789abcdef";
    std::string cookie;
    cookie.reserve(32);
    for (int i = 0; i < 32; ++i)
        cookie += digits[distribution(generator)];
    return cookie;
}

/// Write a FamilyWild Xauthority file so the cookie matches any local display.
bool write_xauthority(const std::string &path, const std::string &cookie) {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "ffff 0000 0000 MIT-MAGIC-COOKIE-1 " << cookie << "\n";
    return file.good();
}

/// Probe the display until Xwayland starts serving, or `timeout_ms` elapses.
bool wait_until_serving(const std::string &display, int timeout_ms) {
    constexpr int step_ms = 50;
    for (int waited = 0; waited <= timeout_ms; waited += step_ms) {
        xcb_connection_t *connection = xcb_connect(display.c_str(), nullptr);
        if (connection != nullptr && !xcb_connection_has_error(connection)) {
            xcb_disconnect(connection);
            return true;
        }
        if (connection != nullptr) xcb_disconnect(connection);
        ::usleep(step_ms * 1'000);
    }
    return false;
}

}   // namespace

XwaylandBridge::~XwaylandBridge() { stop(); }

bool XwaylandBridge::start(const std::string &wayland_display, int display_number) {
    if (running()) return false;
    if (::access("/usr/bin/Xwayland", X_OK) != 0 && ::access("/usr/local/bin/Xwayland", X_OK) != 0) {
        return false;
    }

    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    runtime_dir_ =
        std::string(runtime != nullptr ? runtime : "/tmp") + "/wcas-xwayland-" + std::to_string(display_number);
    if (::mkdir(runtime_dir_.c_str(), 0700) != 0 && errno != EEXIST) return false;

    display_ = ":" + std::to_string(display_number);
    xauthority_ = runtime_dir_ + "/Xauthority";
    const std::string cookie = generate_cookie();
    if (!write_xauthority(xauthority_, cookie)) return false;

    child_ = ::fork();
    if (child_ < 0) {
        child_ = -1;
        stop();
        return false;
    }
    if (child_ == 0) {
        // Child: bind the bridge to the private Wayland display and serve X.
        ::setenv("WAYLAND_DISPLAY", wayland_display.c_str(), 1);
        ::unsetenv("DISPLAY");
        if (::getenv("WCAS_XWAYLAND_TRACE") != nullptr) {
            ::setenv("WAYLAND_DEBUG", "1", 1);
            const std::string log = "/tmp/wcas-xwayland-trace.log";
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                ::dup2(fd, 2);
                ::close(fd);
            }
        }
        ::execlp("Xwayland", "Xwayland", display_.c_str(), "-auth", xauthority_.c_str(), "-noreset",
            static_cast<char *>(nullptr));
        _exit(127);
    }

    if (!wait_until_serving(display_, 5'000)) {
        stop();
        return false;
    }
    return true;
}

void XwaylandBridge::stop() noexcept {
    if (child_ > 0) {
        ::kill(child_, SIGTERM);
        int status = 0;
        while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
        child_ = -1;
    }
    if (!runtime_dir_.empty()) {
        if (!xauthority_.empty()) ::unlink(xauthority_.c_str());
        ::rmdir(runtime_dir_.c_str());
        runtime_dir_.clear();
        xauthority_.clear();
    }
    display_.clear();
}

}   // namespace wcas
}   // namespace skl
