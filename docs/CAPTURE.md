# DMA-BUF 采集

本文说明 WCAS 如何把目标窗口提交的缓冲区变成一张 PNG/`cv::Mat`，重点在 DMA-BUF 的 **后端选择、能力协商与回退**。

> 实现层：`ControlledApp::capture()` 通过 `CaptureInfo` / `CaptureSource` 接口委托给
> `AutoCaptureSource`（`src/skl/wcas/harness/capture_source.cpp`）；`CaptureKind`
> 是公开的后端选择，具体 shm / DMA-BUF / KMS / grim / X11 回退在 source 内部完成。

> DMA-BUF 解码层：`DmaBufDecoder`（`src/skl/wcas/proxy/dmabuf_decoder.h`）提供
> Auto / Vulkan / EGL / mmap providers；`capture_dmabuf_plane()` 绑定一次
> `AutoDecoder`，按 `WCAS_DMABUF_BACKEND` 与格式/modifier 能力选择解码器。

## 两类缓冲区

- **`wl_shm`** —— 客户端共享内存，ARGB8888/XRGB8888，CPU 直接 `mmap` 逐行读。简单、无 GPU 依赖。第四字节对 XRGB 是“don't care”，WCAS 解码时忽略它，输出不透明 BGR。
- **`DMA-BUF`** —— 游戏/合成器路径。真实布局由 **DRM modifier** 描述：可能是厂商 tiled / block-linear，甚至带**帧缓冲压缩**（NVIDIA modifier 的 `c` 字段非零）。**不能**把它当成紧凑线性像素数组来读。

## 后端选择

`WCAS_DMABUF_BACKEND=auto|vulkan|egl`（默认 `auto`）。

| 值       | 行为                                                                                                 |
| -------- | ---------------------------------------------------------------------------------------------------- |
| `vulkan` | 只用 Vulkan 导入路径                                                                                 |
| `egl`    | 只用 EGL/GBM 导入路径；失败即失败，不回退                                                            |
| `auto`   | 按能力选：线性或 Vulkan 声明支持 → Vulkan；否则 EGL 广播了该格式+modifier → EGL；都不支持 → 明确拒绝 |

`auto` 的判定逻辑（`proxy/vulkan_dmabuf.cpp`）：

```
vulkan_legal = (backend==vulkan)
            || (backend==auto && (modifier 线性 || vulkan 声明支持该 modifier))
选 EGL       = (backend==egl)
            || (backend==auto && !vulkan_legal && egl 可用 && egl 广播该格式+modifier)
```

两条路都不满足、且 modifier 非线性时，**明确拒绝**而不是把垃圾当成功截图。

## 能力查询（不是猜测）

- **Vulkan**：`vkGetPhysicalDeviceImageFormatProperties2` + `VkPhysicalDeviceImageDrmFormatModifierInfoEXT`，检查 `(format, modifier, TRANSFER_SRC)` 组合是否被设备支持。
- **EGL**：`eglQueryDmaBufFormatsEXT` / `eglQueryDmaBufModifiersEXT`（来自 `EGL_EXT_image_dma_buf_import_modifiers`），检查该格式与 modifier 是否被广播。

关键认识：**`vkCreateImage` 或 DMA-BUF 导入成功 ≠ 像素正确。** 驱动可以接受一个它并不列出支持的 modifier，然后按错的布局解码，得到“像截图但花屏”的结果。因此 Vulkan 路径对未被声明支持的 modifier 会标 `modifier_supported=NO | pixels=unverified`，而不是谎称“OK”。

## 为什么需要 EGL 后端

NVIDIA 的帧缓冲压缩 surface（block-linear + `c != 0`）在 **Vulkan** 上： modifier 不在设备列表里，`transfer_src`/`sampled` 能力均为否 —— 于是 `vkCmdCopyImageToBuffer` 在没有受支持保证的布局上解码，产生固定的块状纹理（花屏）。

同一个 surface 走 **EGL/GBM**（合成器自身用的路径）可以被正确解释。这里不是“EGL 更懂压缩”，而是 NVIDIA 对 EGL 导入提供了可用路径，对当前 Vulkan `VK_EXT_image_drm_format_modifier` 组合没有。EGL 后端因此是**兼容性路径**，不是“永远更快”。

两条读回路径：

- **Vulkan**：`vkCmdCopyImageToBuffer` → host-visible staging → CPU 解码。
- **EGL**：`eglCreateImageKHR`（带 plane 的 fd/offset/pitch/modifier）→ `glEGLImageTargetTexture2DOES` → FBO → `glReadPixels(GL_RGBA)`。 EGLImage 把源缓冲区首行映射为 GL texel row 0，所以读回**不需要额外上下翻转**；读回是逻辑 RGBA，解码走 `R,G,B,A` 分支。

导入**不复制**整张图（零拷贝导入）；但截图要交给 CPU 则必然有一次 GPU→CPU 读回，两种后端都一样。

## 合成器回退（窗口对齐）

窗口自身的缓冲区经 Vulkan 与 EGL 都无法解码时，`auto` 回退到合成结果：

1. 取窗口尺寸（`frame_size()`）与标题；
2. 用 `swaymsg` → `hyprctl` → `xdotool` 求窗口矩形（`find_window_position_by_size_and_title`）；
3. `grim -g x,y WxH`（合成器**布局坐标**，输出缩放安全）优先，其次 KMS 裁剪（进程内 libdrmtap）。

回退是**窗口对齐**的：不会返回整屏或多显示器拼接。注意 KMS 裁剪按**物理像素**，缩放非 1:1 时可能与布局矩形不一致，因此 grim 优先。

## 计时（默认关闭）

`WCAS_CAPTURE_TIMING=1` 时，每次成功采集打印：

```
[wcas] capture timing: backend=egl|vulkan|cpu  <ms>  <WxH>
```

直接写 stderr、故意不参与日志去重。用于对比各后端延迟，不代表像素正确性。

## 环境变量

| 变量                  |  默认  | 说明                          |
| --------------------- | :----: | ----------------------------- |
| `WCAS_DMABUF_BACKEND` | `auto` | `auto` / `vulkan` / `egl`     |
| `WCAS_CAPTURE_TIMING` |   关   | 设 `1` 打开每次采集的计时日志 |

## 验证状态

- 普通线性 RGB DMA-BUF：Vulkan/CPU 路径，测试覆盖。
- NVIDIA 压缩 DMA-BUF：`auto` 路由到 EGL；实测 3D 游戏截图与合成器（`grim`）一致。
- 未被任何后端声明支持的布局：明确拒绝，不输出花屏。
- 窗口缓冲不可解码：窗口对齐回退到合成器截图。
