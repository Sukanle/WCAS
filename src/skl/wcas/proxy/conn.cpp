#include "conn.h"
#include "input.h"
#include "interfaces.h"
#include "capture_scheduler.h"
#include "reactor.h"
#include "vulkan_dmabuf.h"
#include "wire_meta.h"


#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <opencv2/core.hpp>
#include <fmt/base.h>

namespace skl::wcas::proxy {

Role role_from_interface(const std::string &name) {
    if (name == "wl_registry") return Role::Registry;
    if (name == "wl_surface") return Role::Surface;
    if (name == "wl_shm") return Role::Shm;
    if (name == "wl_shm_pool") return Role::ShmPool;
    if (name == "wl_buffer") return Role::Buffer;
    if (name == "wl_callback") return Role::Callback;
    if (name == "xdg_wm_base") return Role::XdgWmBase;
    if (name == "xdg_surface") return Role::XdgSurface;
    if (name == "xdg_toplevel") return Role::XdgToplevel;
    if (name == "xdg_popup") return Role::XdgPopup;
    if (name == "xdg_positioner") return Role::XdgPositioner;
    if (name == "wl_seat") return Role::Seat;
    if (name == "wl_pointer") return Role::Pointer;
    if (name == "wl_keyboard") return Role::Keyboard;
    if (name == "wl_touch") return Role::Touch;
    if (name == "wl_output") return Role::Output;
    if (name == "wl_compositor") return Role::Compositor;
    if (name == "wl_subcompositor") return Role::Subcompositor;
    if (name == "wl_subsurface") return Role::Subsurface;
    if (name == "wl_region") return Role::Region;
    if (name == "wl_data_device_manager") return Role::DataDeviceManager;
    if (name == "zwp_linux_dmabuf_v1") return Role::DmaBuf;
    if (name == "zwp_linux_buffer_params_v1") return Role::DmaBufParams;
    if (name == "zwp_linux_dmabuf_feedback_v1") return Role::DmaBufFeedback;
    if (name == "wp_viewporter") return Role::Viewporter;
    if (name == "wp_viewport") return Role::Viewport;
    if (name == "wp_presentation") return Role::Presentation;
    if (name == "wp_presentation_feedback") return Role::PresentationFeedback;
    if (name == "wp_fractional_scale_manager_v1") return Role::FractionalScaleManager;
    if (name == "wp_fractional_scale_v1") return Role::FractionalScale;
    if (name == "wp_cursor_shape_manager_v1") return Role::CursorShapeManager;
    if (name == "wp_cursor_shape_device_v1") return Role::CursorShapeDevice;
    if (name == "zxdg_decoration_manager_v1") return Role::DecorationManager;
    if (name == "zxdg_toplevel_decoration_v1") return Role::ToplevelDecoration;
    if (name == "zxdg_output_manager_v1") return Role::XdgOutputManager;
    if (name == "zxdg_output_v1") return Role::XdgOutput;
    if (name == "zwp_text_input_manager_v3") return Role::TextInputManager;
    if (name == "zwp_text_input_v3") return Role::TextInput;
    if (name == "wp_single_pixel_buffer_manager_v1") return Role::SinglePixelBufferManager;
    if (name == "xdg_activation_v1") return Role::Activation;
    if (name == "xdg_activation_token_v1") return Role::ActivationToken;
    if (name == "zwp_idle_inhibit_manager_v1") return Role::IdleInhibitManager;
    if (name == "zwp_idle_inhibitor_v1") return Role::IdleInhibitor;
    if (name == "zwp_pointer_constraints_v1") return Role::PointerConstraints;
    if (name == "zwp_locked_pointer_v1") return Role::LockedPointer;
    if (name == "zwp_confined_pointer_v1") return Role::ConfinedPointer;
    if (name == "zwp_relative_pointer_manager_v1") return Role::RelativePointerManager;
    if (name == "zwp_relative_pointer_v1") return Role::RelativePointer;
    if (name == "zwp_pointer_gestures_v1") return Role::PointerGestures;
    if (name == "zwp_tablet_manager_v2") return Role::TabletManager;
    if (name == "zwp_input_timestamps_manager_v1") return Role::InputTimestampsManager;
    return Role::Generic;
}

namespace {

std::uint32_t word(const char *p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void put_word(char *p, std::uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

struct ObjectMap {
    std::unordered_map<std::uint32_t, std::uint32_t> app_to_upstream;
    std::unordered_map<std::uint32_t, std::uint32_t> upstream_to_app;
    std::unordered_map<std::uint32_t, std::string> upstream_to_interface;
    // Upstream object ids must be handed out *contiguously* from 2
    // onwards: a real Wayland server rejects a client `new_id` that is not
    // below its own next-id watermark (wl_map_insert_at), so a high base
    // like 0x10000000 is refused. Id 1 is always wl_display.
    std::uint32_t next_upstream = 2;

    void link(std::uint32_t app, std::uint32_t upstream) {
        if (!app || !upstream) return;
        app_to_upstream[app] = upstream;
        upstream_to_app[upstream] = app;
    }
    void link_with_interface(std::uint32_t app, std::uint32_t upstream, const std::string &iface) {
        link(app, upstream);
        if (upstream) upstream_to_interface[upstream] = iface;
    }
    void erase_app(std::uint32_t app) {
        auto it = app_to_upstream.find(app);
        if (it != app_to_upstream.end()) {
            upstream_to_app.erase(it->second);
            upstream_to_interface.erase(it->second);
            app_to_upstream.erase(it);
        }
    }
    std::uint32_t to_upstream(std::uint32_t id) const {
        if (!id) return 0;
        auto it = app_to_upstream.find(id);
        return it == app_to_upstream.end() ? 0 : it->second;
    }
    std::uint32_t to_app(std::uint32_t id) const {
        if (!id) return 0;
        auto it = upstream_to_app.find(id);
        return it == upstream_to_app.end() ? 0 : it->second;
    }
    std::uint32_t allocate(std::uint32_t app) {
        auto upstream = next_upstream++;
        link(app, upstream);
        return upstream;
    }
    std::uint32_t allocate_with_interface(std::uint32_t app, const std::string &iface) {
        auto upstream = next_upstream++;
        link_with_interface(app, upstream, iface);
        return upstream;
    }
    /// Interface the upstream object was created as, empty when unknown.
    const std::string &interface_of_upstream(std::uint32_t upstream) const {
        static const std::string none;
        auto it = upstream_to_interface.find(upstream);
        return it == upstream_to_interface.end() ? none : it->second;
    }
};

std::string wire_string(const std::vector<char> &msg, std::size_t at, std::size_t *next) {
    if (at + 4 > msg.size()) return {};
    const auto n = word(msg.data() + at);
    const auto padded = (static_cast<std::size_t>(n) + 3u) & ~std::size_t(3u);
    if (at + 4 + padded > msg.size()) return {};
    if (next) *next = at + 4 + padded;
    return std::string(msg.data() + at + 4, strnlen(msg.data() + at + 4, n));
}

/// Close every plane fd retained in a DMA-BUF snapshot and mark it empty.
void release_dmabuf_planes(const std::array<std::optional<DmaBufPlane>, 4> &planes) {
    for (const auto &plane : planes) {
        if (plane && plane->fd >= 0) ::close(plane->fd);
    }
}

// Wire metadata comes from the protocol XML (see codegen/generate_wire_meta.py).
// The `wl_interface` tables shipped by libwayland/libwayland++ are client-side
// tables: they fold the version-gated `release` request into the preceding
// opcode, which renumbers every later opcode (wl_seat.get_pointer is opcode 1
// there and 0 on the wire), so they cannot drive wire translation.
const wire::MessageMeta *wire_request(const std::string &interface_name, std::uint16_t opcode) {
    const auto *iface = wire::find(interface_name.c_str());
    if (!iface || opcode >= iface->request_count) return nullptr;
    return &iface->requests[opcode];
}

const wire::MessageMeta *wire_event(const std::string &interface_name, std::uint16_t opcode) {
    const auto *iface = wire::find(interface_name.c_str());
    if (!iface || opcode >= iface->event_count) return nullptr;
    return &iface->events[opcode];
}

/// Whether the interface's method at `opcode` creates a new object. Such a
/// request never destroys its sender, whatever the method is called.
bool new_object_id(const std::string &interface_name, std::uint16_t opcode) {
    const auto *request = wire_request(interface_name, opcode);
    return request != nullptr && request->new_interface != nullptr;
}

/// Whether the interface's method at `opcode` is the object's own `destroy`.
bool destroys_its_sender(const std::string &interface_name, std::uint16_t opcode) {
    const auto *request = wire_request(interface_name, opcode);
    if (!request || !request->name) return false;
    return std::strcmp(request->name, "destroy") == 0;
}

struct MessageSpec {
    const char *signature = nullptr;
    const char *new_interface = nullptr;
    /// True when the protocol metadata defines this opcode at all. An unknown
    /// opcode must fail loudly instead of being forwarded untranslated.
    bool known = false;
    /// True when the message is newer than the interface version we know
    /// about, so it must never legitimately appear on the wire.
    bool too_new = false;
};

MessageSpec request_spec(const std::string &interface_name, std::uint16_t opcode) {
    const auto *meta = wire_request(interface_name, opcode);
    if (!meta) return {};
    const auto *iface = wire::find(interface_name.c_str());
    MessageSpec spec;
    spec.known = true;
    spec.signature = meta->signature;
    spec.new_interface = meta->new_interface;
    spec.too_new = meta->since > iface->version;
    return spec;
}

MessageSpec event_spec(const std::string &interface_name, std::uint16_t opcode);

// Input injection writes synthetic server events through the app-facing
// socket so they can use the same object-ID translation path as application
// traffic. Such messages arrive app->upstream but are described by the event
// table rather than the request table.
MessageSpec request_or_injected_event_spec(const std::string &interface_name, std::uint16_t opcode) {
    // The public input API injects text-input-v3 events through the same
    // app-facing socket used for requests. These opcodes overlap with
    // request opcodes, so select the event metadata explicitly.
    if (interface_name == "zwp_text_input_v3" && (opcode == 3 || opcode == 5))
        return event_spec(interface_name, opcode);
    auto spec = request_spec(interface_name, opcode);
    if (spec.known) return spec;
    return event_spec(interface_name, opcode);
}

MessageSpec event_spec(const std::string &interface_name, std::uint16_t opcode) {
    const auto *meta = wire_event(interface_name, opcode);
    if (!meta) return {};
    const auto *iface = wire::find(interface_name.c_str());
    MessageSpec spec;
    spec.known = true;
    spec.signature = meta->signature;
    spec.new_interface = meta->new_interface;
    spec.too_new = meta->since > iface->version;
    return spec;
}

void translate_object(std::vector<char> &msg, std::size_t offset, bool app_to_upstream, ObjectMap &map) {
    if (offset + 4 > msg.size()) return;
    const auto id = word(msg.data() + offset);
    if (!id) return;
    const auto mapped = app_to_upstream ? map.to_upstream(id) : map.to_app(id);
    if (!mapped) return;
    put_word(msg.data() + offset, mapped);
}

bool transform_arguments(std::vector<char> &msg, bool app_to_upstream, const std::string &role, std::uint16_t opcode,
    ObjectMap &map, ConnState &state, const std::vector<int> &fds, std::size_t *fd_index) {
    const auto spec = app_to_upstream ? request_or_injected_event_spec(role, opcode) : event_spec(role, opcode);

    // The interface or opcode is outside the metadata we can translate.
    // Forwarding it anyway could leave object ids untranslated, so the
    // message is rejected instead.
    // The peer is speaking a version above what we announced; treat that
    // as a protocol error rather than guessing the payload.
    if (!spec.known && spec.too_new) return false;

    std::size_t offset = 8;
    for (const char *cursor = spec.signature; *cursor; ++cursor) {
        if (*cursor == '?') continue;
        if (offset + 4 > msg.size()) return false;
        switch (*cursor) {
            case 's':
            case 'a': {
                const auto length = word(msg.data() + offset);
                const auto padded = (static_cast<std::size_t>(length) + 3u) & ~std::size_t(3u);
                if (offset + 4 + padded > msg.size()) {
                    return false;
                }
                offset += 4 + padded;
                break;
            }
            case 'o': {
                translate_object(msg, offset, app_to_upstream, map);
                offset += 4;
                break;
            }
            case 'n': {
                const auto app_id = word(msg.data() + offset);
                if (app_id) {
                    if (app_to_upstream) {
                        const char *new_iface = spec.new_interface;
                        // The upstream connection is dedicated to this one app and
                        // therefore carries a single client-owned id space, so the
                        // app id is passed through unchanged. A real Wayland server
                        // rejects any client new_id that is not the next id its own
                        // allocator would hand out (wl_map_insert_at), which rules
                        // out both a high base and any independent counter that
                        // drifts when the client recycles ids.
                        std::string role_name = new_iface ? new_iface : "";
                        if (role_name.empty()) {
                            // Prefer a role recorded by interface-specific tracking
                            // (wl_registry.bind carries its interface as a runtime
                            // string). `interface_of_upstream` can be stale when the
                            // client recycled this id for a different object.
                            const auto existing = state.object_roles.find(app_id);
                            if (existing != state.object_roles.end()) role_name = existing->second;
                            if (role_name.empty()) role_name = map.interface_of_upstream(app_id);
                        }
                        if (!role_name.empty())
                            map.link_with_interface(app_id, app_id, role_name);
                        else
                            map.link(app_id, app_id);
                        // Every new object needs a role, or its later requests
                        // cannot be translated. When the protocol does not name the
                        // new interface in the signature, fall back to the
                        // interface recorded for the sent upstream id (for example
                        // wl_seat.get_pointer -> wl_pointer).
                        if (!role_name.empty()) {
                            std::lock_guard lock(state.data_mutex);
                            state.object_roles[app_id] = role_name;
                            state.object_role_enum[app_id] = role_from_interface(role_name);
                        }
                    } else {
                        auto app_object = map.to_app(app_id);
                        if (!app_object) {
                            // A server-allocated id (>= 0xFF000000) is brand new to
                            // the client: adopt it verbatim and remember the mapping
                            // so later events for it translate. (e.g. wl_data_offer,
                            // wl_pointer enter ids created by the compositor.)
                            if (app_id >= 0XFF000000U) {
                                const std::string iface = spec.new_interface ? spec.new_interface : "";
                                if (!iface.empty())
                                    map.link_with_interface(app_id, app_id, iface);
                                else
                                    map.link(app_id, app_id);
                                if (!iface.empty()) {
                                    std::lock_guard lock(state.data_mutex);
                                    state.object_roles[app_id] = iface;
                                    state.object_role_enum[app_id] = role_from_interface(iface);
                                }
                                app_object = app_id;
                            }
                        }
                        if (!app_object) {
                            return false;
                        }
                        put_word(msg.data() + offset, app_object);
                    }
                }
                offset += 4;
                break;
            }
            case 'i':
            case 'u':
            case 'f': offset += 4; break;
            case 'h':
                // A file descriptor is NOT part of the message body: it is carried
                // as SCM_RIGHTS ancillary data and consumed in signature order.
                // Both directions must consume it so `transfer` forwards it on.
                if (fd_index) {
                    if (*fd_index >= fds.size()) {
                        return false;
                    }
                    ++*fd_index;
                }
                break;
            default: return false;
        }
    }
    return offset <= msg.size();
}

bool rewrite(std::vector<char> &msg, bool app_to_upstream, ObjectMap &map, ConnState &state,
    const std::vector<int> &fds, std::size_t *fd_index, bool *drop) {
    *drop = false;
    // Serialize the whole translation against external readers. The proxy is a
    // single writer; readers (`list_windows`, `dump_surfaces`, capture, ...)
    // take the same recursive mutex, so none of them can observe a map
    // mid-mutation.
    std::lock_guard lock(state.data_mutex);
    if (msg.size() < 8 || (msg.size() & 3U)) return false;
    const auto sender = word(msg.data());
    const auto opcode = static_cast<std::uint16_t>(word(msg.data() + 4) & 0XFFFFU);
    const auto original_sender = sender;
    const auto translated_sender = app_to_upstream ? map.to_upstream(sender) : map.to_app(sender);
    if (!translated_sender) {
        // A compositor may still deliver an event for an object the client has
        // already destroyed (for example wl_buffer.release after
        // wl_buffer.destroy). libwayland swallows those; drop them here too
        // rather than tearing the connection down.
        if (app_to_upstream) return false;
        *drop = true;
        return true;
    }
    put_word(msg.data(), translated_sender);
    // `object_roles` is keyed by the app-side id. In the upstream->app
    // direction the sender on the wire is already the upstream id, so map it
    // back; in the app->upstream direction the sender *is* the app id.
    const auto role_id = app_to_upstream ? original_sender : map.to_app(original_sender);
    const auto role_it = state.object_roles.find(role_id);
    const std::string role =
        role_it == state.object_roles.end() ? (original_sender == 1 ? "wl_display" : "") : role_it->second;

    // `xdg_activation_v1.activate` (opcode 2) lets the app ask the compositor to
    // focus its window. A synthetic click injected by wcas reaches the app,
    // which (via Wine's Wayland driver) may then request activation, stealing
    // focus from the terminal during automation. By default it is dropped. It
    // is forwarded only for a session the caller explicitly selected (so the
    // window can be activated and then accept synthetic keyboard input).
    if (app_to_upstream && role == "xdg_activation_v1" && opcode == 2) {
        const bool allow = state.allow_activation.load();
        if (std::getenv("WCAS_DEBUG"))
            std::fprintf(stderr, "DEBUG activation: xdg_activation.activate forwarded=%d\n", allow ? 1 : 0);
        if (!allow) {
            *drop = true;
            return true;
        }
    }

    if (!app_to_upstream && opcode == 0 && role == "wl_registry") {
        // wl_registry.global = `usu`: name(uint), interface(string), version(uint)
        std::size_t next = 0;
        const auto name = wire_string(msg, 12, &next);
        if (is_denied_global(name) || !is_allowed_global(name)) {
            *drop = true;
            return true;
        }
        // The app will bind this global to an id it chooses; the version
        // argument does not identify anything, but the name does.
        // if (!name.empty() && next + 4 <= msg.size()) {
        //     (void)next;
        // }
    }

    const auto size = static_cast<std::uint16_t>(word(msg.data() + 4) >> 16);
    const auto words = size >= 8 ? (size - 8) / 4 : 0;

    // Track input object creation before transform_arguments overwrites new_id
    if (app_to_upstream && role == "wl_seat") {
        if (opcode == 0 && words >= 1)
            state.pointer_obj = word(msg.data() + 8);
        else if (opcode == 1 && words >= 1)
            state.keyboard_obj = word(msg.data() + 8);
    }
    if (app_to_upstream && role == "zwp_text_input_manager_v3" && opcode == 1 && words >= 1)
        state.text_input_obj = word(msg.data() + 8);

    // wl_registry.bind = `usun`: observe it on the original app message, before
    // transform_arguments rewrites the new_id into upstream id space.
    if (app_to_upstream && role == "wl_registry" && opcode == 0) {
        std::size_t next = 0;
        const auto iface = wire_string(msg, 12, &next);
        // `wire_string` leaves `next` at the word *after* the string, which is
        // the version argument; the new id follows it.
        if (next + 4 <= msg.size()) {
            const auto app_id = word(msg.data() + next + 4);
            if (app_id && !iface.empty()) {
                std::lock_guard lock(state.data_mutex);
                state.object_roles[app_id] = iface;
                state.object_role_enum[app_id] = role_from_interface(iface);
            }
        }
    }
    if (!transform_arguments(msg, app_to_upstream, role, opcode, map, state, fds, fd_index)) {
        // `zwp_linux_dmabuf_feedback_v1` events are purely informational: they
        // advertise supported formats/modifiers, and a client that never sees
        // them simply falls back to defaults. When one cannot be translated,
        // drop it instead of tearing down the connection -- a real
        // compositor keeps serving the client, and the Rust reference likewise
        // ignores failed forwards for this interface.
        if (role == "zwp_linux_dmabuf_feedback_v1") {
            *drop = true;
            return true;
        }
        return false;
    }
    if (fd_index && *fd_index > fds.size()) return false;

    if (app_to_upstream && role == "zwp_linux_dmabuf_v1" && opcode == 1) {
        const auto params = map.to_app(word(msg.data() + 8));
        if (!params) return false;
        std::lock_guard lock(state.data_mutex);
        state.dmabuf_params.try_emplace(params);
        state.object_roles[params] = "zwp_linux_buffer_params_v1";
        state.object_role_enum[params] = Role::DmaBufParams;
    }


    if (app_to_upstream && role == "wl_subcompositor" && opcode == 1 && words >= 3) {
        // get_subsurface(new_id id, wl_surface surface, wl_surface parent)
        const auto subsurface = word(msg.data() + 8);
        const auto surface = word(msg.data() + 12);
        const auto parent = word(msg.data() + 16);
        if (subsurface && surface && parent) {
            std::lock_guard lock(state.data_mutex);
            state.subsurface_to_surface[subsurface] = surface;
            state.surface_to_parent_surface[surface] = parent;
        }
    }
    if (app_to_upstream && role == "xdg_wm_base" && opcode == 2) {
        const auto xdg_surface = word(msg.data() + 8);
        const auto surface = word(msg.data() + 12);
        if (xdg_surface && surface) {
            std::lock_guard lock(state.data_mutex);
            state.xdg_surface_to_surface[xdg_surface] = surface;
        }
    }
    if (app_to_upstream && role == "xdg_surface" && opcode == 1) {
        const auto toplevel = word(msg.data() + 8);
        std::lock_guard lock(state.data_mutex);
        const auto surface_it = state.xdg_surface_to_surface.find(role_id);
        if (toplevel && surface_it != state.xdg_surface_to_surface.end()) {
            state.surface_to_toplevel[surface_it->second] = toplevel;
            state.toplevels[toplevel] = {surface_it->second, std::nullopt, std::nullopt};
        }
    }
    if (app_to_upstream && role == "xdg_toplevel" && (opcode == 2 || opcode == 3)) {
        std::lock_guard lock(state.data_mutex);
        const auto toplevel_it = state.toplevels.find(role_id);
        if (toplevel_it != state.toplevels.end()) {
            std::size_t str_next = 0;
            const auto value = wire_string(msg, 8, &str_next);
            if (opcode == 2)
                toplevel_it->second.title = value;
            else
                toplevel_it->second.app_id = value;
            if (toplevel_it->second.surface_id) {
                auto &meta = state.surface_metadata[toplevel_it->second.surface_id];
                if (opcode == 2)
                    meta.title = value;
                else
                    meta.app_id = value;
            }
        }
    }

    if (app_to_upstream
        && role == "wl_shm"
        && opcode == 0
        && words >= 2
        && fd_index
        && *fd_index > 0
        && *fd_index <= fds.size()) {
        const auto pool = map.to_app(word(msg.data() + 8));
        const auto size_value = static_cast<std::int32_t>(word(msg.data() + 12));
        const int retained = ::dup(fds[*fd_index - 1]);
        if (retained >= 0) {
            std::lock_guard lock(state.data_mutex);
            state.shm_pools[pool] = {retained, size_value};
        }
        state.object_roles[pool] = "wl_shm_pool";
        state.object_role_enum[pool] = Role::ShmPool;
    }
    if (app_to_upstream && role == "wl_shm_pool" && opcode == 0 && words >= 6) {
        const auto buffer = map.to_app(word(msg.data() + 8));
        std::lock_guard lock(state.data_mutex);
        state.shm_buffers[buffer] = {role_id, static_cast<std::int32_t>(word(msg.data() + 12)),
            static_cast<std::int32_t>(word(msg.data() + 16)), static_cast<std::int32_t>(word(msg.data() + 20)),
            static_cast<std::int32_t>(word(msg.data() + 24)), word(msg.data() + 28)};
        state.object_roles[buffer] = "wl_buffer";
        state.object_role_enum[buffer] = Role::Buffer;
    }
    if (app_to_upstream && role == "wl_shm_pool" && opcode == 2 && words >= 1) {
        const auto size = static_cast<std::int32_t>(word(msg.data() + 8));
        std::lock_guard lock(state.data_mutex);
        const auto pool = state.shm_pools.find(role_id);
        if (pool != state.shm_pools.end() && size > 0) pool->second.size = size;
    }

    // DMA-BUF parameter tracking is deliberately independent from capture:
    // retain duplicated plane FDs so later Vulkan import can outlive the
    // protocol message and validate the parameter object lifecycle now.
    if (app_to_upstream
        && role == "zwp_linux_buffer_params_v1"
        && opcode == 1
        && words >= 5
        && fd_index
        && *fd_index > 0
        && *fd_index <= fds.size()) {
        const auto plane_index = word(msg.data() + 8);
        if (plane_index >= 4) return false;
        const auto offset = word(msg.data() + 12);
        const auto stride = word(msg.data() + 16);
        const auto modifier = (static_cast<std::uint64_t>(word(msg.data() + 20)) << 32) | word(msg.data() + 24);
        std::lock_guard lock(state.data_mutex);
        auto params = state.dmabuf_params.find(role_id);
        if (params == state.dmabuf_params.end() || params->second.planes[plane_index]) return false;
        const int retained = ::dup(fds[*fd_index - 1]);
        if (retained < 0) return false;
        params->second.planes[plane_index] = DmaBufPlane{retained, offset, stride, modifier};
    }
    if (app_to_upstream && role == "zwp_linux_buffer_params_v1" && opcode == 2 && words >= 4) {
        // Asynchronous `create`: the wl_buffer id only arrives later in the
        // `created` event, so stash the geometry here and register the buffer
        // when that event is seen. Missing this left every `create`-based
        // client (notably Wine/winewayland) without a tracked dmabuf buffer,
        // so its surface commits were never capturable.
        std::lock_guard lock(state.data_mutex);
        auto params = state.dmabuf_params.find(role_id);
        if (params == state.dmabuf_params.end()) return false;
        params->second.width = static_cast<std::int32_t>(word(msg.data() + 8));
        params->second.height = static_cast<std::int32_t>(word(msg.data() + 12));
        params->second.format = word(msg.data() + 16);
        params->second.flags = word(msg.data() + 20);
        params->second.create_requested = true;
    }
    if (app_to_upstream && role == "zwp_linux_buffer_params_v1" && opcode == 3 && words >= 5) {
        const auto buffer = map.to_app(word(msg.data() + 8));
        std::lock_guard lock(state.data_mutex);
        const auto params = state.dmabuf_params.find(role_id);
        if (params == state.dmabuf_params.end() || !buffer) return false;
        state.dmabuf_buffers[buffer] = {role_id, static_cast<std::int32_t>(word(msg.data() + 12)),
            static_cast<std::int32_t>(word(msg.data() + 16)), word(msg.data() + 20), word(msg.data() + 24),
            params->second.planes};
        state.object_roles[buffer] = "wl_buffer";
        state.object_role_enum[buffer] = Role::Buffer;
        for (auto &plane : state.dmabuf_buffers[buffer].planes) {
            if (plane) {
                const int copy = ::dup(plane->fd);
                if (copy < 0) return false;
                plane->fd = copy;
            }
        }
    }
    // zwp_linux_buffer_params_v1.created (upstream->app, new_id wl_buffer):
    // completes an earlier asynchronous `create`. Register the buffer with the
    // geometry stashed by that request so later attach/commit is capturable.
    if (!app_to_upstream && role == "zwp_linux_buffer_params_v1" && opcode == 0 && words >= 1) {
        const auto buffer = map.to_app(word(msg.data() + 8));
        std::lock_guard lock(state.data_mutex);
        const auto params = state.dmabuf_params.find(role_id);
        if (buffer && params != state.dmabuf_params.end() && params->second.create_requested) {
            DmaBufBuffer entry;
            entry.params_id = role_id;
            entry.width = params->second.width;
            entry.height = params->second.height;
            entry.format = params->second.format;
            entry.flags = params->second.flags;
            bool complete = true;
            for (std::size_t i = 0; i < entry.planes.size(); ++i) {
                if (!params->second.planes[i]) continue;
                const int copy = ::dup(params->second.planes[i]->fd);
                if (copy < 0) {
                    complete = false;
                    break;
                }
                auto plane = *params->second.planes[i];
                plane.fd = copy;
                entry.planes[i] = plane;
            }
            if (complete) {
                state.dmabuf_buffers[buffer] = std::move(entry);
                state.object_roles[buffer] = "wl_buffer";
                state.object_role_enum[buffer] = Role::Buffer;
            } else {
                for (auto &plane : entry.planes)
                    if (plane && plane->fd >= 0) ::close(plane->fd);
            }
        }
    }
    // zwp_linux_buffer_params_v1.failed: the pending `create` produced nothing.
    if (!app_to_upstream && role == "zwp_linux_buffer_params_v1" && opcode == 1) {
        std::lock_guard lock(state.data_mutex);
        auto params = state.dmabuf_params.find(role_id);
        if (params != state.dmabuf_params.end()) params->second.create_requested = false;
    }

    // Input coordinate mapping: buffer vs surface-local (logical) size.
    if (app_to_upstream && role == "wp_viewporter" && opcode == 1 && words >= 2) {
        const auto viewport = word(msg.data() + 8);
        const auto surf = map.to_app(word(msg.data() + 12));
        if (viewport && surf) state.viewport_to_surface[viewport] = surf;
    }
    if (app_to_upstream && role == "wp_viewport" && opcode == 2 && words >= 2) {
        // wp_viewport.set_destination(width, height): the surface-local size the
        // buffer is scaled to. -1/-1 unsets it.
        const auto width = static_cast<std::int32_t>(word(msg.data() + 8));
        const auto height = static_cast<std::int32_t>(word(msg.data() + 12));
        const auto viewport = state.viewport_to_surface.find(role_id);
        if (viewport != state.viewport_to_surface.end()) {
            if (width > 0 && height > 0)
                state.surface_logical_size[viewport->second] = {width, height};
            else
                state.surface_logical_size.erase(viewport->second);
        }
    }
    if (app_to_upstream && role == "wl_surface" && opcode == 8 && words >= 1)
        state.surface_buffer_scale[role_id] = word(msg.data() + 8);
    // wl_surface.frame(new_id wl_callback): remember which surface the callback
    // belongs to so the matching `done` counts as a presented frame.
    if (app_to_upstream && role == "wl_surface" && opcode == 3 && words >= 1) {
        const auto callback = word(msg.data() + 8);
        if (callback) {
            std::lock_guard lock(state.data_mutex);
            state.callback_to_surface[callback] = role_id;
        }
    }
    if (!app_to_upstream && role == "wl_callback" && opcode == 0) {
        // wl_callback.done. Only a `wl_surface.frame` callback means a frame
        // was presented; `wl_display.sync` callbacks share wl_callback but are
        // not frames, so they must not inflate the presented-frame counter.
        std::lock_guard lock(state.data_mutex);
        if (state.callback_to_surface.erase(map.to_app(original_sender)) > 0) ++state.presented_frames;
    }
    if (app_to_upstream && role == "wp_fractional_scale_manager_v1" && opcode == 1 && words >= 2) {
        const auto fractional = word(msg.data() + 8);
        const auto surf = map.to_app(word(msg.data() + 12));
        if (fractional && surf) state.fractional_scale_to_surface[fractional] = surf;
    }
    if (!app_to_upstream && role == "wp_fractional_scale_v1" && opcode == 0 && words >= 1) {
        // preferred_scale is in 120ths (120 == 1.0).
        const auto fractional = state.fractional_scale_to_surface.find(role_id);
        if (fractional != state.fractional_scale_to_surface.end())
            state.surface_preferred_scale_120[fractional->second] = word(msg.data() + 8);
    }

    if (app_to_upstream && role == "wl_surface" && opcode == 1 && words >= 1) {
        auto buffer_id = word(msg.data() + 8);
        auto buffer = map.to_app(buffer_id);
        if (!buffer) buffer = buffer_id;
        std::lock_guard lock(state.data_mutex);
        auto &surf = state.surfaces[role_id];
        if (buffer == 0) {
            surf.pending_buffer = 0;
            surf.pending_dmabuf = 0;
        } else if (state.shm_buffers.find(buffer) != state.shm_buffers.end()) {
            surf.pending_buffer = buffer;
            surf.pending_dmabuf.reset();
        } else if (state.dmabuf_buffers.find(buffer) != state.dmabuf_buffers.end()) {
            surf.pending_dmabuf = buffer;
            surf.pending_buffer.reset();
        }
    }
    if (app_to_upstream && role == "wl_surface" && opcode == 6) {
        ++state.commit_count;
        std::lock_guard lock(state.data_mutex);
        auto &surf = state.surfaces[role_id];
        surf.commit_count++;
        surf.last_commit_seq = state.commit_count.load();

        if (surf.pending_dmabuf.has_value()) {
            // A DMA-BUF frame is snapshotted with duplicated plane FDs so it
            // stays capturable after the client destroys the wl_buffer or its
            // params object.
            const auto dma_it =
                *surf.pending_dmabuf ? state.dmabuf_buffers.find(*surf.pending_dmabuf) : state.dmabuf_buffers.end();
            if (*surf.pending_dmabuf == 0 || dma_it == state.dmabuf_buffers.end()) {
                surf.committed_dmabuf.reset();
                if (surf.committed_dmabuf_frame.has_value()) {
                    release_dmabuf_planes(surf.committed_dmabuf_frame->planes);
                    surf.committed_dmabuf_frame.reset();
                }
            } else {
                surf.committed_dmabuf = *surf.pending_dmabuf;
                RetainedDmaBuf retained;
                retained.width = dma_it->second.width;
                retained.height = dma_it->second.height;
                retained.format = dma_it->second.format;
                bool complete = true;
                for (std::size_t i = 0; i < retained.planes.size(); ++i) {
                    if (!dma_it->second.planes[i]) continue;
                    const int copy = ::dup(dma_it->second.planes[i]->fd);
                    if (copy < 0) {
                        complete = false;
                        break;
                    }
                    auto plane = *dma_it->second.planes[i];
                    plane.fd = copy;
                    retained.planes[i] = plane;
                }
                if (!complete) {
                    release_dmabuf_planes(retained.planes);
                    surf.committed_dmabuf.reset();
                } else {
                    if (surf.committed_dmabuf_frame.has_value())
                        release_dmabuf_planes(surf.committed_dmabuf_frame->planes);
                    surf.committed_dmabuf_frame = std::move(retained);
                }
            }
            surf.pending_dmabuf.reset();
        }
        if (surf.pending_buffer.has_value()) {
            if (*surf.pending_buffer == 0) {
                surf.committed_buffer.reset();
                if (surf.committed_frame.has_value()) {
                    if (surf.committed_frame->fd >= 0) ::close(surf.committed_frame->fd);
                    surf.committed_frame.reset();
                }
            } else {
                surf.committed_buffer = *surf.pending_buffer;
                const auto buffer_it = state.shm_buffers.find(*surf.pending_buffer);
                if (buffer_it != state.shm_buffers.end()) {
                    const auto pool_it = state.shm_pools.find(buffer_it->second.pool_id);

                    if (pool_it != state.shm_pools.end()) {
                        const int retained_fd = ::dup(pool_it->second.fd);
                        if (retained_fd >= 0) {
                            if (surf.committed_frame.has_value() && surf.committed_frame->fd >= 0)
                                ::close(surf.committed_frame->fd);
                            surf.committed_frame = RetainedShm{retained_fd, pool_it->second.size,
                                buffer_it->second.offset, buffer_it->second.width, buffer_it->second.height,
                                buffer_it->second.stride, buffer_it->second.format};
                        }
                    }
                }
            }
        }
        state.capturable_surface = false;
        for (const auto &[sid, s] : state.surfaces) {
            if (s.committed_frame.has_value() || s.committed_dmabuf_frame.has_value()) {
                state.capturable_surface = true;
                break;
            }
        }
    }

    if (app_to_upstream && role == "wl_buffer" && opcode == 0) {
        std::lock_guard lock(state.data_mutex);
        state.shm_buffers.erase(role_id);
    }
    if (app_to_upstream && role == "wl_shm_pool" && opcode == 1) {
        std::lock_guard lock(state.data_mutex);
        auto pool = state.shm_pools.find(role_id);
        if (pool != state.shm_pools.end()) {
            if (pool->second.fd >= 0) ::close(pool->second.fd);
            state.shm_pools.erase(pool);
        }
    }

    auto role_enum_it = state.object_role_enum.find(role_id);
    Role role_enum = role_enum_it == state.object_role_enum.end() ? Role::Generic : role_enum_it->second;

    if (destroys_its_sender(role, opcode)) {
        std::lock_guard lock(state.data_mutex);
        if (role == "zwp_linux_buffer_params_v1") {
            const auto params = state.dmabuf_params.find(role_id);
            if (params != state.dmabuf_params.end()) {
                for (const auto &plane : params->second.planes)
                    if (plane && plane->fd >= 0) ::close(plane->fd);
                state.dmabuf_params.erase(params);
            }
        } else if (role == "wl_buffer") {
            const auto buffer = state.dmabuf_buffers.find(role_id);
            if (buffer != state.dmabuf_buffers.end()) {
                for (const auto &plane : buffer->second.planes)
                    if (plane && plane->fd >= 0) ::close(plane->fd);
                state.dmabuf_buffers.erase(buffer);
            }
        }
    }

    // Track wl_keyboard focus changes from the compositor. Without this our
    // own `keyboard_focus` bookkeeping goes stale: the compositor sends a real
    // `leave` when another window takes focus, and `ensure_keyboard_focus`
    // would then believe the surface is still focused and skip re-sending
    // `enter`, so injected keys are dropped by the app.
    if (!app_to_upstream && role == "wl_keyboard" && opcode == 1 && words >= 2) {
        std::lock_guard lock(state.data_mutex);
        state.keyboard_focus = word(msg.data() + 12);
    }
    if (!app_to_upstream && role == "wl_keyboard" && opcode == 2 && words >= 2) {
        std::lock_guard lock(state.data_mutex);
        const auto surface = word(msg.data() + 12);
        if (state.keyboard_focus == surface) state.keyboard_focus = 0;
    }

    // Track focused surface and position from pointer.enter event (upstream→app)
    if (!app_to_upstream && role == "wl_pointer" && opcode == 0 && words >= 3) {
        // wl_pointer.enter: serial(4), surface(4), sx(4), sy(4)
        // After transform_arguments, surface at offset 12 is in app ID space
        state.focused_surface = word(msg.data() + 12);
        state.pointer_focus = state.focused_surface;
        if (words >= 4) {
            state.pointer_sx = word(msg.data() + 16);
            state.pointer_sy = word(msg.data() + 20);
            state.pointer_position_known = true;
        }
    }
    // wl_pointer.motion: time(4), surface_x(4), surface_y(4). Records the real
    // device position so callers can query where the user's cursor actually is.
    if (!app_to_upstream && role == "wl_pointer" && opcode == 2 && words >= 3) {
        state.pointer_sx = word(msg.data() + 12);
        state.pointer_sy = word(msg.data() + 16);
        state.pointer_position_known = true;
    }
    // Track pointer leave to clear focus
    if (!app_to_upstream && role == "wl_pointer" && opcode == 1 && words >= 1) {
        state.focused_surface = 0;
        state.pointer_focus = 0;
    }

    // Track wl_keyboard.keymap event (upstream→app): format(u), fd(h), size(u)
    // The 'h' arg is NOT consumed by transform_arguments for events, so we
    // consume it here for both tracking and forwarding.
    if (!app_to_upstream && role == "wl_keyboard" && opcode == 0 && fd_index && words >= 2) {
        if (*fd_index > 0 && *fd_index <= fds.size()) {
            if (state.keymap_fd >= 0) ::close(state.keymap_fd);
            state.keymap_fd = ::dup(fds[*fd_index - 1]);
            state.keymap_size = word(msg.data() + 12);
        }
    }

    // Track zwp_text_input_v3 enable/disable/done (upstream→app). The opcodes
    // follow the interface table: 0=enable, 1=disable, 5=done(serial).
    if (!app_to_upstream && role == "zwp_text_input_v3") {
        if (opcode == 0)
            state.text_input_pending_enabled = true;
        else if (opcode == 1)
            state.text_input_pending_enabled = false;
        else if (opcode == 5) {
            ++state.text_input_commit_serial;
            state.text_input_enabled = state.text_input_pending_enabled;
        }
    }

    // A request destroys its sender only when that exact opcode is the
    // interface's `destroy` method. Inferring "opcode 0 means destroy" is
    // wrong: wl_pointer.set_cursor is opcode 0, wl_shm_pool.destroy is 1 and
    // wl_callback has no destroy at all, so the guess erased live objects
    // (and therefore their id mappings) on ordinary requests.
    const bool destroys_sender = app_to_upstream && !new_object_id(role, opcode) && destroys_its_sender(role, opcode);

    if (destroys_sender) {
        state.object_roles.erase(role_id);
        state.object_role_enum.erase(role_id);
        map.erase_app(role_id);
        if (role_enum == Role::TextInput && state.text_input_obj == role_id) {
            state.text_input_obj = 0;
            state.text_input_pending_enabled = false;
            state.text_input_enabled = false;
            state.text_input_commit_serial = 0;
        }
        // Window bookkeeping must be dropped with the objects it describes. Without
        // this a closed window leaves its xdg_toplevel metadata behind forever,
        // and `list` keeps showing a 0x0 ghost that no longer has a surface.
        if (role_enum == Role::XdgToplevel) {
            if (const auto it = state.toplevels.find(role_id); it != state.toplevels.end()) {
                state.surface_to_toplevel.erase(it->second.surface_id);
                state.toplevels.erase(it);
            }
        }
        if (role_enum == Role::XdgSurface) state.xdg_surface_to_surface.erase(role_id);
        if (role_enum == Role::Subsurface) {
            if (const auto it = state.subsurface_to_surface.find(role_id); it != state.subsurface_to_surface.end()) {
                state.surface_to_parent_surface.erase(it->second);
                state.subsurface_to_surface.erase(it);
            }
        }
        if (role_enum == Role::Surface) {
            // A window's render surface is often a subsurface of the surface
            // that carries the toplevel. When the shell surface is destroyed
            // (popup dismissed, window recreated) the child keeps rendering, so
            // hand it the last known name before dropping it; otherwise the
            // still-visible window loses its identity.
            if (const auto meta = state.surface_metadata.find(role_id); meta != state.surface_metadata.end()) {
                // Copy first: inserting into the map below may rehash and
                // invalidate `meta`, and reading through the stale iterator
                // corrupted the heap.
                const SurfaceMetadata saved = meta->second;
                for (const auto &[child, parent] : state.surface_to_parent_surface) {
                    if (parent == role_id && state.surface_metadata.find(child) == state.surface_metadata.end())
                        state.surface_metadata[child] = saved;
                }
            }
            state.surface_to_toplevel.erase(role_id);
            state.surface_metadata.erase(role_id);
            // Drop this surface as anyone's parent and as anyone's child.
            state.surface_to_parent_surface.erase(role_id);
            for (auto it = state.surface_to_parent_surface.begin(); it != state.surface_to_parent_surface.end();) {
                if (it->second == role_id)
                    it = state.surface_to_parent_surface.erase(it);
                else
                    ++it;
            }
            for (auto it = state.subsurface_to_surface.begin(); it != state.subsurface_to_surface.end();) {
                if (it->second == role_id)
                    it = state.subsurface_to_surface.erase(it);
                else
                    ++it;
            }
        }
    }
    return true;
}

bool transfer(int from, int to, bool app_to_upstream, ObjectMap &map, const std::shared_ptr<ConnState> &state,
    std::vector<char> &pending, std::vector<int> &pending_fds) {
    std::array<char, 65'536> bytes{};
    std::array<char, CMSG_SPACE(sizeof(int) * 32)> control{};
    iovec iov{bytes.data(), bytes.size()};
    msghdr in{};
    in.msg_iov = &iov;
    in.msg_iovlen = 1;
    in.msg_control = control.data();
    in.msg_controllen = control.size();
    const auto n = ::recvmsg(from, &in, 0);
    if (n <= 0) return false;
    std::vector<int> received_fds;
    for (auto *c = CMSG_FIRSTHDR(&in); c; c = CMSG_NXTHDR(&in, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const auto *values = reinterpret_cast<const int *>(CMSG_DATA(c));
            received_fds.insert(received_fds.end(), values, values + count);
        }
    }
    pending_fds.insert(pending_fds.end(), received_fds.begin(), received_fds.end());
    pending.insert(pending.end(), bytes.data(), bytes.data() + n);
    while (pending.size() >= 8) {
        const auto header = word(pending.data() + 4);
        const auto message_size = static_cast<std::uint16_t>(header >> 16);
        if (message_size < 8 || (message_size & 3u) != 0 || message_size > 1u << 16) return false;
        if (pending.size() < message_size) break;
        std::vector<char> message(pending.begin(), pending.begin() + message_size);
        pending.erase(pending.begin(), pending.begin() + message_size);
        bool drop = false;
        std::size_t fd_index = 0;
        if (!rewrite(message, app_to_upstream, map, *state, pending_fds, &fd_index, &drop)
            || (fd_index > pending_fds.size()))
            return false;

        std::vector<int> message_fds(pending_fds.begin(), pending_fds.begin() + fd_index);
        pending_fds.erase(pending_fds.begin(), pending_fds.begin() + fd_index);
        if (drop) {
            for (const auto fd : message_fds)
                ::close(fd);
            continue;
        }

        msghdr out{};
        iovec out_iov{message.data(), message.size()};
        out.msg_iov = &out_iov;
        out.msg_iovlen = 1;
        std::vector<char> out_control(message_fds.empty() ? 0 : CMSG_SPACE(sizeof(int) * message_fds.size()));
        if (!message_fds.empty()) {
            out.msg_control = out_control.data();
            out.msg_controllen = out_control.size();
            auto *c = CMSG_FIRSTHDR(&out);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * message_fds.size());
            std::memcpy(CMSG_DATA(c), message_fds.data(), sizeof(int) * message_fds.size());
        }
        const auto sent = ::sendmsg(to, &out, MSG_NOSIGNAL);
        for (const auto fd : message_fds)
            ::close(fd);
        if (sent != static_cast<ssize_t>(message.size())) return false;
    }
    return true;
}

}   // namespace

ConnState::~ConnState() {
    for (const auto &entry : shm_pools) {
        if (entry.second.fd >= 0) ::close(entry.second.fd);
    }
    for (const auto &[sid, surf] : surfaces) {
        if (surf.committed_frame.has_value() && surf.committed_frame->fd >= 0) ::close(surf.committed_frame->fd);
    }
    for (const auto &[sid, surf] : surfaces) {
        if (surf.committed_dmabuf_frame.has_value()) release_dmabuf_planes(surf.committed_dmabuf_frame->planes);
    }
    if (keymap_fd >= 0) ::close(keymap_fd);
    for (const auto &[id, params] : dmabuf_params) {
        for (const auto &plane : params.planes)
            if (plane && plane->fd >= 0) ::close(plane->fd);
    }
    for (const auto &[id, buffer] : dmabuf_buffers) {
        for (const auto &plane : buffer.planes)
            if (plane && plane->fd >= 0) ::close(plane->fd);
    }
}

Connection::Connection(int app_fd, int upstream_fd, std::shared_ptr<ConnState> state)
    : app_fd_(app_fd)
    , upstream_fd_(upstream_fd)
    , state_(std::move(state)) {}

Connection::~Connection() {
    if (app_fd_ >= 0) ::close(app_fd_);
    if (upstream_fd_ >= 0) ::close(upstream_fd_);
}

bool capture_shm(const std::shared_ptr<ConnState> &state, cv::Mat &image) {
    if (!state) return false;
    if (state->backend_unsupported.load()) return false;
    std::lock_guard lock(state->data_mutex);

    const RetainedShm *selected_frame = nullptr;
    // With no explicit target, use the same surface input is routed to, so the
    // captured image and the movement/cursor coordinate space agree (important
    // with several windows/outputs).
    if (!state->capture_app_id && !state->capture_title) {
        const auto target = target_surface(state);
        if (const auto it = state->surfaces.find(target);
            it != state->surfaces.end() && it->second.committed_frame.has_value()) {
            selected_frame = &*it->second.committed_frame;
        }
    }
    for (const auto &[sid, surf] : state->surfaces) {
        if (selected_frame) break;
        if (!surf.committed_frame.has_value()) continue;
        if (!state->capture_app_id && !state->capture_title) {
            selected_frame = &*surf.committed_frame;
            break;
        }
        // A capture target was requested: only an exact app_id/title match
        // qualifies. Returning an unrelated surface's frame here would make
        // the target silently meaningless.
        const auto toplevel_id = state->surface_to_toplevel.find(sid);
        if (toplevel_id == state->surface_to_toplevel.end()) continue;
        const auto top = state->toplevels.find(toplevel_id->second);
        if (top == state->toplevels.end()) continue;
        const bool app_match =
            !state->capture_app_id || (top->second.app_id && *top->second.app_id == *state->capture_app_id);
        const bool title_match =
            !state->capture_title || (top->second.title && *top->second.title == *state->capture_title);
        if (app_match && title_match) {
            selected_frame = &*surf.committed_frame;
            break;
        }
    }
    if (!selected_frame) return false;


    const auto &buffer = *selected_frame;
    if (buffer.width <= 0
        || buffer.height <= 0
        || buffer.stride < buffer.width * 4
        || buffer.offset < 0
        || buffer.pool_size <= 0
        || static_cast<std::int64_t>(buffer.offset) + static_cast<std::int64_t>(buffer.stride) * buffer.height
               > buffer.pool_size)
        return false;
    void *mapped = ::mmap(nullptr, static_cast<std::size_t>(buffer.pool_size), PROT_READ, MAP_SHARED, buffer.fd, 0);
    if (mapped == MAP_FAILED) return false;
    cv::Mat result(buffer.height, buffer.width, CV_8UC3);
    const auto *base = static_cast<const unsigned char *>(mapped) + buffer.offset;
    for (int y = 0; y < buffer.height; ++y) {
        const auto *row = base + static_cast<std::size_t>(y) * buffer.stride;
        for (int x = 0; x < buffer.width; ++x) {
            const auto *pixel = row + x * 4;
            auto &destination = result.at<cv::Vec3b>(y, x);
            // `wl_shm.format` is the wl_shm enum (ARGB8888=0, XRGB8888=1), not a
            // DRM fourcc. Real clients send the enum values, so checking only
            // the fourcc (0x3432524x) made every real wl_shm frame undecodable.
            // The fourcc forms are still accepted for buffers that carry them.
            constexpr std::uint32_t wl_shm_argb8888 = 0u, wl_shm_xrgb8888 = 1u;
            constexpr std::uint32_t drm_argb8888 = 0x34325241u, drm_xrgb8888 = 0x34325258u;
            if (buffer.format == wl_shm_argb8888
                || buffer.format == wl_shm_xrgb8888
                || buffer.format == drm_argb8888
                || buffer.format == drm_xrgb8888) {
                destination = cv::Vec3b(pixel[0], pixel[1], pixel[2]);
            } else {
                ::munmap(mapped, static_cast<std::size_t>(buffer.pool_size));
                return false;
            }
        }
    }
    ::munmap(mapped, static_cast<std::size_t>(buffer.pool_size));
    image = std::move(result);
    return true;
}

bool capture_dmabuf(const std::shared_ptr<ConnState> &state, cv::Mat &image) {
    if (!state) return false;
    std::lock_guard lock(state->data_mutex);

    const RetainedDmaBuf *selected = nullptr;
    // Mirror capture_shm: with no explicit target, use the input-routed surface
    // so capture and the movement coordinate space stay consistent.
    if (!state->capture_app_id && !state->capture_title) {
        const auto target = target_surface(state);
        if (const auto it = state->surfaces.find(target);
            it != state->surfaces.end() && it->second.committed_dmabuf_frame.has_value()) {
            selected = &*it->second.committed_dmabuf_frame;
        }
    }
    for (const auto &[sid, surf] : state->surfaces) {
        if (selected) break;
        if (!surf.committed_dmabuf_frame.has_value()) continue;
        if (!state->capture_app_id && !state->capture_title) {
            selected = &*surf.committed_dmabuf_frame;
            break;
        }
        const auto toplevel_id = state->surface_to_toplevel.find(sid);
        if (toplevel_id == state->surface_to_toplevel.end()) continue;
        const auto top = state->toplevels.find(toplevel_id->second);
        if (top == state->toplevels.end()) continue;
        const bool app_match =
            !state->capture_app_id || (top->second.app_id && *top->second.app_id == *state->capture_app_id);
        const bool title_match =
            !state->capture_title || (top->second.title && *top->second.title == *state->capture_title);
        if (app_match && title_match) {
            selected = &*surf.committed_dmabuf_frame;
            break;
        }
    }
    if (!selected) return false;


    int plane_count = 0;
    for (const auto &plane : selected->planes) {
        if (plane) ++plane_count;
    }
    // Multi-planar (usually YUV) buffers need GPU interop to decode.
    if (plane_count != 1 || !selected->planes[0]) return false;

    const auto &plane = *selected->planes[0];
    return capture_dmabuf_plane(plane.fd, selected->width, selected->height, plane.stride, plane.modifier,
        selected->format, plane.offset, image);
}

void set_capture_target(
    const std::shared_ptr<ConnState> &state, std::optional<std::string> app_id, std::optional<std::string> title) {
    if (!state) return;
    std::lock_guard lock(state->data_mutex);
    state->capture_app_id = std::move(app_id);
    state->capture_title = std::move(title);
}

std::vector<WindowInfo> list_windows(const std::shared_ptr<ConnState> &state) {
    std::vector<WindowInfo> out;
    if (!state) return out;
    std::lock_guard lock(state->data_mutex);
    const auto size_of = [&state](std::uint32_t sid) -> std::pair<std::int32_t, std::int32_t> {
        const auto it = state->surfaces.find(sid);
        if (it == state->surfaces.end()) return {0, 0};
        if (it->second.committed_dmabuf_frame.has_value())
            return {it->second.committed_dmabuf_frame->width, it->second.committed_dmabuf_frame->height};
        if (it->second.committed_frame.has_value())
            return {it->second.committed_frame->width, it->second.committed_frame->height};
        return {0, 0};
    };
    // Walk subsurface parents until we reach a surface that owns toplevel
    // metadata. Games routinely put the visible window content in a subsurface
    // so the rendering surface itself carries no app_id/title.
    const auto inherited_from = [&state](std::uint32_t sid) -> SurfaceMetadata {
        for (int depth = 0; depth < 16 && sid; ++depth) {
            const auto top = state->surface_to_toplevel.find(sid);
            if (top != state->surface_to_toplevel.end()) {
                const auto it = state->toplevels.find(top->second);
                if (it != state->toplevels.end() && (it->second.app_id || it->second.title))
                    return SurfaceMetadata{it->second.app_id, it->second.title};
            }
            const auto parent = state->surface_to_parent_surface.find(sid);
            if (parent == state->surface_to_parent_surface.end()) break;
            sid = parent->second;
        }
        return {};
    };
    // Same walk, but falling back to the last name recorded for any surface in
    // the parent chain whose toplevel has since been destroyed.
    const auto stored_from = [&state](std::uint32_t sid) -> SurfaceMetadata {
        for (int depth = 0; depth < 16 && sid; ++depth) {
            if (const auto meta = state->surface_metadata.find(sid);
                meta != state->surface_metadata.end() && (meta->second.app_id || meta->second.title))
                return meta->second;
            const auto parent = state->surface_to_parent_surface.find(sid);
            if (parent == state->surface_to_parent_surface.end()) break;
            sid = parent->second;
        }
        return {};
    };
    std::unordered_set<std::uint32_t> listed;
    for (const auto &[tid, top] : state->toplevels) {
        (void)tid;
        WindowInfo info;
        info.surface_id = top.surface_id;
        info.app_id = top.app_id;
        info.title = top.title;
        info.origin = WindowOrigin::Toplevel;
        std::tie(info.width, info.height) = size_of(top.surface_id);
        out.push_back(std::move(info));
        listed.insert(top.surface_id);
    }
    // A session often has exactly one named toplevel whose own surface never
    // renders (a 0x0 shell), while the app draws into a separate (or freshly
    // recreated) surface with no explicit parent link. Only in that shape do we
    // attribute the most recently committed nameless surface to the toplevel;
    // if the toplevel renders itself we leave other surfaces alone, and stale
    // leftovers stay "bare".
    std::optional<SurfaceMetadata> sole_toplevel;
    std::size_t shell_toplevels = 0;
    for (const auto &[tid, top] : state->toplevels) {
        (void)tid;
        if (!top.app_id && !top.title) continue;
        const auto [width, height] = size_of(top.surface_id);
        if (width != 0 || height != 0) continue;   // renders itself: not a shell
        ++shell_toplevels;
        sole_toplevel = SurfaceMetadata{top.app_id, top.title};
    }
    if (shell_toplevels != 1) sole_toplevel.reset();

    std::uint32_t most_recent = 0;
    std::uint64_t most_recent_seq = 0;
    for (const auto &[sid, surf] : state->surfaces) {
        if (listed.count(sid)) continue;
        if (!surf.committed_frame.has_value() && !surf.committed_dmabuf_frame.has_value()) continue;
        if (most_recent == 0 || surf.last_commit_seq >= most_recent_seq) {
            most_recent_seq = surf.last_commit_seq;
            most_recent = sid;
        }
    }

    // Surfaces that render but never became a toplevel (subsurfaces, overlays
    // created without xdg_toplevel) are still capturable/controllable. Sort
    // them by recency so the actively drawn surface comes first.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> rendered;
    for (const auto &[sid, surf] : state->surfaces) {
        if (listed.count(sid)) continue;
        if (!surf.committed_frame.has_value() && !surf.committed_dmabuf_frame.has_value()) continue;
        rendered.emplace_back(surf.last_commit_seq, sid);
    }
    std::sort(rendered.begin(), rendered.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &[seq, sid] : rendered) {
        (void)seq;
        WindowInfo info;
        info.surface_id = sid;
        auto meta = inherited_from(sid);
        // Only the most recently committed surface may claim a fallback name;
        // older surfaces are stale leftovers and stay anonymous.
        if ((!meta.app_id && !meta.title) && sid == most_recent) {
            meta = stored_from(sid);
            if ((!meta.app_id && !meta.title) && sole_toplevel.has_value()) meta = *sole_toplevel;
        }
        if (meta.app_id || meta.title) {
            info.app_id = meta.app_id;
            info.title = meta.title;
            info.origin = WindowOrigin::Subsurface;
        } else {
            info.origin = WindowOrigin::Bare;
        }
        std::tie(info.width, info.height) = size_of(sid);
        out.push_back(std::move(info));
    }
    return out;
}

SessionCounters session_counters(const std::shared_ptr<ConnState> &state) {
    SessionCounters counters;
    if (!state) return counters;
    std::lock_guard lock(state->data_mutex);
    counters.surfaces = state->surfaces.size();
    counters.toplevels = state->toplevels.size();
    counters.has_keyboard = state->keyboard_obj != 0;
    counters.has_pointer = state->pointer_obj != 0;
    counters.has_keymap = state->keymap_fd >= 0;
    return counters;
}

std::vector<SurfaceDebug> dump_surfaces(const std::shared_ptr<ConnState> &state) {
    std::vector<SurfaceDebug> out;
    if (!state) return out;
    std::lock_guard lock(state->data_mutex);
    for (const auto &[sid, surf] : state->surfaces) {
        SurfaceDebug info;
        info.surface_id = sid;
        if (const auto role = state->object_roles.find(sid); role != state->object_roles.end())
            info.role = role->second;
        if (const auto parent = state->surface_to_parent_surface.find(sid);
            parent != state->surface_to_parent_surface.end())
            info.parent_surface_id = parent->second;
        if (const auto top = state->surface_to_toplevel.find(sid); top != state->surface_to_toplevel.end()) {
            info.toplevel_id = top->second;
            if (const auto entry = state->toplevels.find(top->second); entry != state->toplevels.end()) {
                info.app_id = entry->second.app_id;
                info.title = entry->second.title;
            }
        }
        if (const auto meta = state->surface_metadata.find(sid); meta != state->surface_metadata.end()) {
            if (!info.app_id) info.app_id = meta->second.app_id;
            if (!info.title) info.title = meta->second.title;
        }
        if (surf.committed_dmabuf_frame.has_value()) {
            info.width = surf.committed_dmabuf_frame->width;
            info.height = surf.committed_dmabuf_frame->height;
        } else if (surf.committed_frame.has_value()) {
            info.width = surf.committed_frame->width;
            info.height = surf.committed_frame->height;
        }
        info.has_frame = surf.committed_frame.has_value() || surf.committed_dmabuf_frame.has_value();
        info.last_commit_seq = surf.last_commit_seq;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
        [](const SurfaceDebug &a, const SurfaceDebug &b) { return a.surface_id < b.surface_id; });
    return out;
}

double input_scale(const std::shared_ptr<ConnState> &state, std::uint32_t surface_id) {
    if (!state || !surface_id) return 1.0;
    std::lock_guard lock(state->data_mutex);
    const auto scale_of = [&state](std::uint32_t sid) -> std::optional<double> {
        std::int32_t buffer_width = 0;
        const auto surface = state->surfaces.find(sid);
        if (surface != state->surfaces.end()) {
            if (surface->second.committed_dmabuf_frame.has_value())
                buffer_width = surface->second.committed_dmabuf_frame->width;
            else if (surface->second.committed_frame.has_value())
                buffer_width = surface->second.committed_frame->width;
        }
        if (buffer_width <= 0) return std::nullopt;
        // wp_viewport.set_destination is authoritative when present.
        if (const auto it = state->surface_logical_size.find(sid);
            it != state->surface_logical_size.end() && it->second.first > 0) {
            return static_cast<double>(buffer_width) / static_cast<double>(it->second.first);
        }
        if (const auto it = state->surface_buffer_scale.find(sid);
            it != state->surface_buffer_scale.end() && it->second > 0) {
            return static_cast<double>(it->second);
        }
        if (const auto it = state->surface_preferred_scale_120.find(sid);
            it != state->surface_preferred_scale_120.end() && it->second > 0) {
            return static_cast<double>(it->second) / 120.0;
        }
        return std::nullopt;
    };
    double scale = 1.0;
    if (const auto value = scale_of(surface_id)) {
        scale = *value;
    } else {
        // The input target is often a wrapper/subsurface with no committed frame
        // of its own; the scale belongs to the app's main rendered surface.
        for (const auto &[sid, unused] : state->surfaces) {
            (void)unused;
            if (sid == surface_id) continue;
            if (const auto value = scale_of(sid)) {
                scale = *value;
                break;
            }
        }
    }
    return scale;
}

namespace {

/// Build a capture job from the primary committed DMA-BUF frame, duplicating
/// the plane fd so it outlives the protocol message.
std::optional<CaptureJob> make_capture_job(const std::shared_ptr<ConnState> &state) {
    std::lock_guard lock(state->data_mutex);
    const RetainedDmaBuf *selected = nullptr;
    for (const auto &[sid, surf] : state->surfaces) {
        if (surf.committed_dmabuf_frame.has_value()) {
            selected = &*surf.committed_dmabuf_frame;
            break;
        }
    }
    if (!selected || !selected->planes[0]) return std::nullopt;
    const auto &plane = *selected->planes[0];
    if (plane.fd < 0) return std::nullopt;
    const int dup_fd = ::dup(plane.fd);
    if (dup_fd < 0) return std::nullopt;
    CaptureJob job;
    job.fd = dup_fd;
    job.width = selected->width;
    job.height = selected->height;
    job.format = selected->format;
    job.modifier = plane.modifier;
    job.stride = plane.stride;
    job.offset = plane.offset;
    return job;
}

}   // namespace

void Connection::run() {
    ObjectMap map;
    map.link(1, 1);
    state_->connected = true;
    std::vector<char> app_pending;
    std::vector<char> upstream_pending;
    std::vector<int> app_pending_fds;
    std::vector<int> upstream_pending_fds;

    Reactor reactor;
    CaptureScheduler scheduler;
    if (reactor.valid()) {
        reactor.add_fd(key_server, app_fd_);
        reactor.add_fd(key_upstream, upstream_fd_);
        // A periodic tick drives the background slot scheduler when the caller
        // asked for DMA-BUF capture; capture() then reads the latest completed
        // snapshot instead of blocking on I/O.
        if (state_->background_capture.load()) reactor.start_timer(100);
    }

    while (true) {
        if (state_->stop.load()) break;
        std::vector<ReactorEvent> events;
        if (reactor.wait(events, 200) < 0) break;

        bool server_ready = false;
        bool upstream_ready = false;
        bool capture_timer = false;
        std::vector<int> sync_slots;
        for (const auto &event : events) {
            if (event.key == key_server) {
                server_ready = true;
            } else if (event.key == key_upstream) {
                upstream_ready = true;
            } else if (event.key == key_capture_timer) {
                capture_timer = true;
            } else if (event.key >= key_sync_file_base && event.key < key_sync_file_base + 16) {
                sync_slots.push_back(static_cast<int>(event.key - key_sync_file_base));
            }
        }

        if (capture_timer && scheduler.valid() && scheduler.has_free_slot()) {
            if (auto job = make_capture_job(state_)) {
                scheduler.on_timer_tick(std::move(*job), reactor);
            }
        }
        for (const int slot : sync_slots)
            scheduler.on_sync_ready(slot);

        if (server_ready && !transfer(app_fd_, upstream_fd_, true, map, state_, app_pending, app_pending_fds)) break;
        if (upstream_ready
            && !transfer(upstream_fd_, app_fd_, false, map, state_, upstream_pending, upstream_pending_fds))
            break;

        scheduler.check_timeouts();
        if (auto snapshot = scheduler.take_latest_snapshot()) {
            std::lock_guard lock(state_->data_mutex);
            state_->pre_captured_frame = std::move(*snapshot);
        }
    }

    for (const auto fd : app_pending_fds)
        ::close(fd);
    for (const auto fd : upstream_pending_fds)
        ::close(fd);
    state_->connected = false;
}

}   // namespace skl::wcas::proxy
