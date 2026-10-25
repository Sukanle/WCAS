#pragma once

#include <string>

namespace skl::wcas::proxy {

/// Registry global filtering. `is_allowed_global` is true for the interface
/// names the proxy knows how to forward; `is_denied_global` is an explicit
/// override for names that must never be exposed even if listed as allowed.
///
/// This is a plain name table on purpose: the wire translator uses the
/// generated `wire_meta` tables, not these `wl_interface` descriptions, so no
/// protocol binding headers are needed here.
bool is_allowed_global(const std::string &name);
bool is_denied_global(const std::string &name);

}   // namespace skl::wcas::proxy
