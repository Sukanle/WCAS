#pragma once

#include <string>

#include "SKL/WCAS/WCAS.h"
#include "SKL/WCAS/Transport.h"

namespace skl::wcas {

/// Platform-specific process launch description. The POSIX implementation
/// forks/execs and owns the child process group; a future Windows provider can
/// map the same fields to CreateProcess/job objects.
struct ProcessSpec : LaunchSpec {
    bool inherit_diagnostics = false;
    bool gpu_accelerated = false;
    GraphicsApi graphics_api = GraphicsApi::vulkan;

    bool use_x11 = false;
    std::string display;
    std::string xauthority;
    std::string wayland_socket;
};

/// Owns one launched process/process group. This is deliberately a component,
/// not an ABIX interface: transport providers compose it, while POSIX details
/// (fork, setsid/process group, PDEATHSIG, signals) stay inside the provider.
class ProcessHost {
public:
    ProcessHost() = default;
    ~ProcessHost();
    ProcessHost(const ProcessHost &) = delete;
    ProcessHost &operator=(const ProcessHost &) = delete;
    ProcessHost(ProcessHost &&) = delete;
    ProcessHost &operator=(ProcessHost &&) = delete;

    [[nodiscard]] result<void> spawn(const ProcessSpec &spec);
    [[nodiscard]] pid_t pid() const noexcept { return child_; }
    [[nodiscard]] pid_t process_group() const noexcept { return process_group_; }
    [[nodiscard]] bool alive() const noexcept;
    /// Reap the child if it exited. Returns true once it has exited.
    [[nodiscard]] bool poll_exited() noexcept;
    [[nodiscard]] int exit_status() const noexcept { return exit_status_; }

    void terminate() noexcept;

private:
    pid_t child_ = -1;
    pid_t process_group_ = -1;
    bool exited_ = false;
    int exit_status_ = 0;
};

}   // namespace skl::wcas
