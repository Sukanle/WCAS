#include "skl/wcas/wcas.h"
#include "proxy/conn.h"
#include "harness/session.h"

#include "skl/wcas/error.h"

#include "proxy/wire_meta.h"
#include "proxy/interfaces.h"
#include <cassert>
#include <memory>
#include <string>
#include <unistd.h>

int main() {
    const auto *shm = skl::wcas::proxy::wire::find("wl_shm");
    assert(shm != nullptr);
    assert(shm->request_count > 0);
    assert(std::string(shm->requests[0].name) == "create_pool");
    assert(std::string(shm->requests[0].signature) == "nhi");
    const auto *keyboard = skl::wcas::proxy::wire::find("wl_keyboard");
    assert(keyboard != nullptr);
    assert(keyboard->event_count > 3);
    assert(std::string(keyboard->events[3].name) == "key");
    assert(std::string(keyboard->events[3].signature) == "uuuu");
    const auto *text_input = skl::wcas::proxy::wire::find("zwp_text_input_v3");
    assert(text_input != nullptr);
    assert(std::string(text_input->events[3].name) == "commit_string");
    assert(std::string(text_input->events[3].signature) == "?s");
    assert(std::string(text_input->events[5].name) == "done");
    assert(std::string(text_input->events[5].signature) == "u");
    assert(skl::wcas::proxy::wire::find("wl_not_a_real_interface") == nullptr);
    for (std::size_t i = 0; i < skl::wcas::proxy::wire::core_interface_count(); ++i) {
        const auto &iface = skl::wcas::proxy::wire::core_interfaces()[i];
        for (std::size_t opcode = 0; opcode < iface.request_count; ++opcode)
            assert(iface.requests[opcode].opcode == opcode);
        for (std::size_t opcode = 0; opcode < iface.event_count; ++opcode)
            assert(iface.events[opcode].opcode == opcode);
    }
    for (std::size_t i = 0; i < skl::wcas::proxy::wire::extension_interface_count(); ++i) {
        const auto &iface = skl::wcas::proxy::wire::extension_interfaces()[i];
        for (std::size_t opcode = 0; opcode < iface.request_count; ++opcode)
            assert(iface.requests[opcode].opcode == opcode);
        for (std::size_t opcode = 0; opcode < iface.event_count; ++opcode)
            assert(iface.events[opcode].opcode == opcode);
    }

    cv::Mat blank(128, 128, CV_8UC3, cv::Scalar(0, 0, 0));
    assert(!skl::wcas::frame_has_visible_content({blank}));
    cv::Mat visible = blank.clone();
    visible(cv::Rect(0, 0, visible.cols / 2, visible.rows)).setTo(cv::Scalar(0, 255, 0));
    assert(skl::wcas::frame_has_visible_content({visible}));
    auto proton = skl::wcas::LaunchConfig::proton("game.exe");
    assert(proton.program() == "game.exe");
    auto builder = skl::wcas::ComputerUse::builder().for_proton().bridge_geometry(1'280, 800);
    (void)builder;
    // Registry filtering is a lightweight name table (no protocol bindings).
    assert(skl::wcas::proxy::is_allowed_global("wl_compositor"));
    assert(skl::wcas::proxy::is_allowed_global("wl_shm"));
    assert(skl::wcas::proxy::is_allowed_global("xdg_wm_base"));
    assert(skl::wcas::proxy::is_allowed_global("zwp_linux_dmabuf_v1"));
    assert(!skl::wcas::proxy::is_allowed_global("zwlr_foreign_toplevel_manager_v1"));
    assert(!skl::wcas::proxy::is_allowed_global("unknown_private_interface"));
    assert(skl::wcas::proxy::is_denied_global("ext_session_lock_manager_v1"));
    assert(!skl::wcas::proxy::is_denied_global("wl_compositor"));

    assert(skl::make_error_code(skl::wcas::errc::feature_not_implemented).message().find("not implemented")
           != std::string::npos);

    // One process can open several Wayland connections (Wine). They must all be
    // tracked: keying sessions by pid used to hide every connection but the
    // last, which made a second window invisible to `list`/`target`.
    {
        auto first = std::make_shared<skl::wcas::proxy::ConnState>();
        first->owner_pid = 4'242;
        first->connected = true;
        auto second = std::make_shared<skl::wcas::proxy::ConnState>();
        second->owner_pid = 4'242;
        second->connected = true;
        skl::wcas::detail::register_session(4'242, first);
        skl::wcas::detail::register_session(4'242, second);
        std::size_t seen = 0;
        for (const auto &session : skl::wcas::detail::sessions_for(4'242, "")) {
            if (session->owner_pid == 4'242) ++seen;
        }
        assert(seen == 2);
        skl::wcas::detail::erase_sessions_for(4'242);
        assert(skl::wcas::detail::sessions_for(4'242, "").empty());
    }

    // A detached `wineserver` is the normal case for umu/Proton: it connects to
    // the proxy but is not a descendant of the launched process once its
    // ancestors have exited. The pid chain alone must not be the only test, or
    // the native launch times out and kills the game even though a connected,
    // capturable session exists. Use this test process as a stand-in owner and
    // a pid it is provably not descended from.
    {
        const pid_t orphan_owner = ::getpid();
        const pid_t unrelated_root = 999'999'999;   // no process, not an ancestor
        char self_exe[4'096] = {};
        const auto exe_len = ::readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
        assert(exe_len > 0);

        auto state = std::make_shared<skl::wcas::proxy::ConnState>();
        state->owner_pid = orphan_owner;
        state->connected = true;
        state->capturable_surface = true;
        skl::wcas::detail::register_session(orphan_owner, state);

        // pid-only lookup must miss it: the chain from `orphan_owner` never
        // contains `unrelated_root`.
        assert(!skl::wcas::detail::session_for(unrelated_root));

        // The command line / executable fallback must find it.
        const std::string exe_pattern = std::string(self_exe, static_cast<std::size_t>(exe_len));
        assert(skl::wcas::detail::session_for(unrelated_root, exe_pattern) == state);

        // An unrelated pattern must still miss it, so the fallback cannot match
        // every session in the process.
        assert(!skl::wcas::detail::session_for(unrelated_root, "definitely-not-this-binary"));

        // An invalid regex must be ignored rather than throw.
        assert(!skl::wcas::detail::session_for(unrelated_root, "([unclosed"));

        skl::wcas::detail::erase_sessions_for(orphan_owner);
    }
    return 0;
}
