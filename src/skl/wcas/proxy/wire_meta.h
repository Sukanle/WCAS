#pragma once

#include <cstddef>
#include <cstdint>

namespace skl::wcas::proxy::wire {

/// One wire message: the signature uses the Wayland wire alphabet
/// `i u f s o n a h`, with a leading `?` marking a nullable object/string.
struct MessageMeta {
    const char *name;
    const char *signature;
    /// Interface created by a `new_id` argument, or nullptr when the created
    /// interface is chosen at runtime (for example wl_registry.bind).
    const char *new_interface;
    /// Interface version the message was introduced in.
    std::uint32_t since;
    /// Wire opcode; validated against the table index at generation time.
    std::uint32_t opcode;
};

struct InterfaceMeta {
    const char *name;
    /// Highest version defined by the protocol definition.
    std::uint32_t version;
    const MessageMeta *requests;
    std::uint32_t request_count;
    const MessageMeta *events;
    std::uint32_t event_count;
};

/// Extension protocols, generated from wayland-protocols XML.
const InterfaceMeta *extension_interfaces();
std::size_t extension_interface_count();

/// Core `wl_*` protocols, generated from wayland.xml plus the
/// scanner-generated server header. `wayland.xml` alone omits arguments that
/// wayland-scanner fills in from context, such as `wl_registry.bind`'s
/// `interface` and `version`, so the server header is authoritative here.
const InterfaceMeta *core_interfaces();
std::size_t core_interface_count();

/// Look up an interface's wire metadata by protocol name, or nullptr.
const InterfaceMeta *find(const char *name);

}   // namespace skl::wcas::proxy::wire
