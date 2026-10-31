# WCAS experimental Vulkan capture layer

This directory is an experimental proof of concept, not part of the production
WCAS capture path. It captures a Vulkan application's presented swapchain image
by copying it into a linear, DMA-BUF-exportable image and sending that fd to a
small receiver over a Unix socket.

The approach is inspired by `obs-vkcapture`, but this is an independent
implementation. It does not link to OBS and does not copy GPL code into WCAS.

## Build

```sh
cmake -S . -B build -DWCAS_BUILD_VK_LAYER=ON
cmake --build build --target wcas-vk-layer wcas-vk-receiver
```

## Run

Start the receiver:

```sh
./build/experimental/vk-capture/wcas-vk-receiver \
  --socket /tmp/wcas-vk.sock --out /tmp/vkcube.raw
```

Run a Vulkan application with the layer:

```sh
VK_LAYER_PATH="$PWD/build/experimental/vk-capture" \
VK_INSTANCE_LAYERS=VK_LAYER_WCAS_vk_capture \
WCAS_VK_SOCKET=/tmp/wcas-vk.sock \
vkcube
```

The receiver prints frame metadata and can write the first frame as a raw
`stride * height` buffer:

```text
frame 1: 1915x2110 stride=7680 format=37 fd=5
```

It can also publish the frames into the existing WCAS SDM ring:

```sh
./build/experimental/vk-capture/wcas-vk-receiver \
  --socket /tmp/wcas-vk.sock --sdm wcas-vk-test
```

The SDM name is the same kind of name accepted by `wcasctl capture-sdm`.

## Scope and limitations

- Linux/64-bit, Vulkan only.
- Requires the device to expose `VK_KHR_external_memory_fd` and a linear-exportable
  format. The layer tries to enable the required device extensions itself.
- The copy destination is linear, so this path avoids the compositor's compressed
  DMA-BUF modifier, at the cost of a GPU copy.
- One queue family (index 0) is assumed for the copy command pool.
- The receiver can write into a WCAS SDM channel, but it is not yet wired into the
  production `FrameBus` lifecycle or control plane.
- Present-driven: if the application stops rendering, no new frames are produced.
- Not suitable for QtWebEngine/XWayland-only targets; use the regular WCAS private
  proxy path for those.
