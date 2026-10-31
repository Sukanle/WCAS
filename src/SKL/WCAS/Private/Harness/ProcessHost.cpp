#include <signal.h>

#ifdef _WIN32
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/prctl.h>
#  include <sys/wait.h>
#endif

#include <stdlib.h>
#include <fstream>
#include <string>
#include <vector>

#include "SKL/WCAS/Error.h"

#include "SKL/WCAS/Private/Harness/Session.h"
#include "SKL/WCAS/Private/Harness/ProcessHost.h"

namespace skl::wcas {
namespace {
std::error_code code(errc value) { return ::skl::make_error_code(value); }

#ifdef __linux__
bool process_is_zombie(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(file, line)) return false;
    const auto close_paren = line.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= line.size()) return false;
    return line[close_paren + 2] == 'Z';
}
#endif
}   // namespace

ProcessHost::~ProcessHost() { terminate(); }

result<void> ProcessHost::spawn(const ProcessSpec &spec) {
    if (spec.program.empty()) return code(errc::process_spawn_failed);
    if (child_ > 0 && !exited_) return code(errc::process_spawn_failed);

    const pid_t child = ::fork();
    if (child < 0) return code(errc::process_spawn_failed);
    if (child == 0) {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (!spec.inherit_diagnostics) {
            const int fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                ::dup2(fd, 1);
                ::dup2(fd, 2);
                if (fd > 2) ::close(fd);
            }
        }
        if (spec.current_dir) ::chdir(spec.current_dir->c_str());
        if (spec.use_x11) {
            ::setenv("DISPLAY", spec.display.c_str(), 1);
            ::setenv("XAUTHORITY", spec.xauthority.c_str(), 1);
            ::setenv("XDG_SESSION_TYPE", "x11", 1);
            ::unsetenv("WAYLAND_DISPLAY");
        } else {
            ::setenv("WAYLAND_DISPLAY", spec.wayland_socket.c_str(), 1);
            ::setenv("GDK_BACKEND", "wayland", 1);
            ::setenv("QT_QPA_PLATFORM", "wayland", 1);
        }
        if (!spec.gpu_accelerated) ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        switch (spec.graphics_api) {
            case GraphicsApi::opengl: ::setenv("PROTON_USE_WINED3D", "1", 1); break;
            case GraphicsApi::vulkan:
                ::unsetenv("PROTON_USE_WINED3D");
                ::setenv("WINEDLLOVERRIDES", "d3d9,d3d10core,d3d11,dxgi=n", 1);
                break;
            case GraphicsApi::dxvk: ::unsetenv("PROTON_USE_WINED3D"); break;
        }
        for (const auto &e : spec.env) {
            if (e.second)
                ::setenv(e.first.c_str(), e.second->c_str(), 1);
            else
                ::unsetenv(e.first.c_str());
        }
        if (spec.use_x11) {
            // Force X11 last: the caller's env may enable Wayland (e.g.
            // PROTON_ENABLE_WAYLAND=1 from the wrapper) and is applied just
            // above. Without clearing it the fallback still runs winewayland,
            // which hits an unimplemented INBV path on some Proton builds
            // (ntoskrnl.exe.InbvAcquireDisplayOwnership) and aborts the game.
            ::unsetenv("WAYLAND_DISPLAY");
            ::unsetenv("PROTON_ENABLE_WAYLAND");
            ::unsetenv("PROTON_USE_WAYLAND");
            ::unsetenv("GDK_BACKEND");
            ::unsetenv("QT_QPA_PLATFORM");
        }
        std::vector<char *> argv{const_cast<char *>(spec.program.c_str())};
        for (const auto &arg : spec.args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::setpgid(child, child);
    child_ = child;
    process_group_ = child;
    exited_ = false;
    exit_status_ = 0;
    return boost::outcome_v2::success();
}

bool ProcessHost::alive() const noexcept {
    if (exited_ || child_ <= 0) return false;
    if (::kill(child_, 0) != 0) return false;
#ifdef __linux__
    if (process_is_zombie(child_)) return false;
#endif
    return true;
}

bool ProcessHost::poll_exited() noexcept {
    if (exited_ || child_ <= 0) return true;
    int status = 0;
    const pid_t result = ::waitpid(child_, &status, WNOHANG);
    if (result != child_) return false;
    exited_ = true;
    exit_status_ = status;
    return true;
}

void ProcessHost::terminate() noexcept {
    if (child_ > 0 && !exited_) {
        detail::terminate_group(process_group_ > 0 ? process_group_ : child_);
        exited_ = true;
    }
    child_ = -1;
    process_group_ = -1;
}

}   // namespace skl::wcas
