#pragma once
#include "SKL/WCAS/Types.h"

#include <string>

namespace skl::wcas::proxy {

/// X keysym for a named key.
/// WCAS Single characters map to their (lower-cased) ASCII value, named keys
///  map to the X11 keysym range, and `F1..F24` map to `0xffbe + n - 1`.
/// Unknown names return `std::nullopt` so callers fail loudly instead
/// of guessing a keycode.
optional<uint32_t> keysym_for_key(const std::string &name);

}   // namespace skl::wcas::proxy
