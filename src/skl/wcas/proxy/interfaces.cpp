#include "interfaces.h"

#include <cstddef>
#include <cstring>

namespace skl::wcas::proxy {
namespace {

/// Interface names the proxy forwards (core Wayland + the extensions the
/// translation tables cover). Keep in sync with `codegen/generate_*.py`.
constexpr const char *allowed_globals[] = {
    "wl_display",
    "wl_registry",
    "wl_compositor",
    "wl_subcompositor",
    "wl_shm",
    "wl_shm_pool",
    "wl_seat",
    "wl_output",
    "wl_data_device_manager",
    "wl_data_device",
    "wl_data_offer",
    "wl_data_source",
    "wl_surface",
    "wl_buffer",
    "wl_callback",
    "wl_region",
    "wl_pointer",
    "wl_keyboard",
    "wl_touch",
    "xdg_wm_base",
    "xdg_positioner",
    "xdg_surface",
    "xdg_toplevel",
    "xdg_popup",
    "zwp_linux_dmabuf_v1",
    "zwp_linux_buffer_params_v1",
    "zwp_linux_dmabuf_feedback_v1",
    "wp_viewporter",
    "wp_viewport",
    "wp_presentation",
    "wp_presentation_feedback",
    "zwp_tablet_manager_v2",
    "zwp_tablet_seat_v2",
    "zwp_tablet_tool_v2",
    "zwp_tablet_v2",
    "zwp_tablet_pad_v2",
    "zwp_text_input_manager_v3",
    "zwp_text_input_v3",
    "zxdg_decoration_manager_v1",
    "zxdg_toplevel_decoration_v1",
    "zxdg_output_manager_v1",
    "zxdg_output_v1",
    "zwp_pointer_constraints_v1",
    "zwp_locked_pointer_v1",
    "zwp_confined_pointer_v1",
    "zwp_relative_pointer_manager_v1",
    "zwp_relative_pointer_v1",
    "zwp_pointer_gestures_v1",
    "zwp_pointer_gesture_swipe_v1",
    "zwp_pointer_gesture_pinch_v1",
    "zwp_pointer_gesture_hold_v1",
    "zwp_input_timestamps_manager_v1",
    "zwp_input_timestamps_v1",
    "zwp_idle_inhibit_manager_v1",
    "zwp_idle_inhibitor_v1",
    "wp_fractional_scale_manager_v1",
    "wp_fractional_scale_v1",
    "wp_cursor_shape_manager_v1",
    "wp_cursor_shape_device_v1",
    "wp_single_pixel_buffer_manager_v1",
    "xdg_activation_v1",
    "xdg_activation_token_v1",
};
constexpr std::size_t allowed_global_count = sizeof(allowed_globals) / sizeof(allowed_globals[0]);

/// Names that must never be exposed even if they were added above.
constexpr const char *denied_globals[] = {
    "ext_session_lock_manager_v1",
    "wp_security_context_manager_v1",
    "wp_drm_lease_device_v1",
    "ext_transient_seat_manager_v1",
    "zwp_input_method_manager_v2",
    "zwp_keyboard_shortcuts_inhibit_manager_v1",
    "zwp_xwayland_keyboard_grab_manager_v1",
    "ext_data_control_manager_v1",
    "ext_foreign_toplevel_list_v1",
    "zxdg_exporter_v1",
    "zxdg_exporter_v2",
    "zxdg_importer_v1",
    "zxdg_importer_v2",
    "zwlr_foreign_toplevel_manager_v1",
    "zwlr_layer_shell_v1",
    "zwp_virtual_keyboard_manager_v1",
    "zwlr_data_control_manager_v1",
    "zwp_primary_selection_device_manager_v1",
};
constexpr std::size_t denied_global_count = sizeof(denied_globals) / sizeof(denied_globals[0]);

bool in_list(const char *name, const char * const *list, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(name, list[i]) == 0) return true;
    }
    return false;
}

}   // namespace

bool is_allowed_global(const std::string &name) { return in_list(name.c_str(), allowed_globals, allowed_global_count); }

bool is_denied_global(const std::string &name) { return in_list(name.c_str(), denied_globals, denied_global_count); }

}   // namespace skl::wcas::proxy
