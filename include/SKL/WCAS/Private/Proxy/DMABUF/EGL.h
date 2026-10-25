#pragma once

#include <vector>

#include "SKL/WCAS/Types.h"

namespace skl::wcas::proxy {

/// True when an EGL/GBM stack able to import DMA-BUFs is available.
///
/// This is the same decoding route the compositor uses (EGL + GL), which is the
/// only one that handles NVIDIA framebuffer-compressed surfaces (modifier
/// `c != 0`). The Vulkan import path accepts those modifiers but does not list
/// them as supported, so it decodes them wrong.
bool egl_dmabuf_available();

/// Whether the EGL display advertises `fourcc`+`modifier` for import (via
/// `eglQueryDmaBufFormatsEXT`/`eglQueryDmaBufModifiersEXT`). Lets `auto` pick a
/// backend by real capability instead of attempting an import that cannot work.
/// Returns true when the query is unavailable and the import must be tried.
bool egl_dmabuf_import_supported(uint32_t fourcc, uint64_t modifier);

/// Import `fd` through EGL/GBM and read it back as tightly packed *logical*
/// RGBA bytes (`width * height * 4`, R, G, B, A order, top-left origin).
///
/// Returns `nullopt` when EGL is unavailable, rejects the modifier, or the
/// framebuffer read fails. A successful return only means EGL produced bytes;
/// callers must still interpret them with the source format's channel order.
optional<std::vector<uint8_t>> egl_read_dmabuf(
    int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t modifier, uint32_t fourcc, uint32_t offset);

}   // namespace skl::wcas::proxy
