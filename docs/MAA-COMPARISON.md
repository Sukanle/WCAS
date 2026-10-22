# WCAS 与 MAA 现有 Linux 控制器的差异、优势与局限

本文对比 MaaFramework 现有 Linux 控制器（`MaaLinuxControllerCreate`）与 WCAS（本仓库）的设计理念与工程取舍。

一句话概括：

> **MAA 是"显示器/合成器级"控制，WCAS 是"应用级私有 seat"控制。**

---

## 1. MAA 现有 Linux 实现

`MaaLinuxControllerCreate` 通过 `screencap_method` 与 `input_method` 选择后端，其可选项都依赖外部授权或特权：

| 路线                 | 关键字段 / 依赖                                                                     | 作用粒度           |
| -------------------- | ----------------------------------------------------------------------------------- | ------------------ |
| Wlroots              | `wlr_socket_path`；仅 wlroots 系合成器；`wlr-screencopy` + virtual pointer/keyboard | 整屏               |
| PipeWire（portal）   | `pw_socket_fd` / `pw_node_id`；xdg-desktop-portal ScreenCast，需用户弹窗授权        | 整屏 / 显示器      |
| PipeWire 会话 daemon | `pw_node_id`；gamescope 等会话 daemon 节点                                          | gamescope 会话     |
| UInput               | `uinput_path` / `uinput_screen_width` / `uinput_screen_height`；`/dev/uinput` 权限  | 操作系统级虚拟设备 |
| Libei (EIS)          | `eis_socket_path`（如 gamescope-0-ei）                                              | 操作系统级         |

即：**捕获的是合成器输出/屏幕**，**注入的是往合成器或内核塞虚拟输入设备**。

---

## 2. WCAS 的机制

WCAS 为被测应用创建一个**私有 Wayland socket**，并把应用透明代理到真实合成器：

- 应用照常显示、可被用户手动操作；
- 应用的 `wl_shm` / `zwp_linux_dmabuf_v1` 缓冲、`wl_pointer` / `wl_keyboard` / `zwp_text_input_v3` 对象都经过这条私有 seat，因此可以 **只捕获这一个应用、只往这一个应用的私有连接注入输入**；
- native 拿不到可读帧时，自动回退到 owner-authenticated XWayland 桥。

关键实现（详见 [docs/COMPATIBILITY.md](COMPATIBILITY.md) 与 [docs/CAPTURE.md](CAPTURE.md)）：

- 协议感知代理、双对象空间、参数与 FD 翻译、registry allowlist/denylist；
- `wl_shm` pool/buffer 生命周期与 ARGB/XRGB 解码；
- `zwp_linux_dmabuf_v1` 拦截（`add`/`create`/`create_immed`/`created`/`failed`）， mmap 与 GPU 导入（Vulkan / EGL+GBM）读取路径；
- `EXPORT_SYNC_FILE` + VkSemaphore + GPU copy 的无撕裂异步截图管线；
- 事件驱动的窗口几何跟踪（拦截 `xdg_surface` / `xdg_toplevel` 请求与事件）；
- 基于 `wl_surface.commit` / `wl_callback.done` 的渲染活跃度停滞检测。

---

## 3. 差异与优势

### 3.1 compositor 无关，无需 portal 授权

WCAS 只做协议代理，GNOME/Mutter、KDE/KWin、Sway、Hyprland 等都能用（GNOME/weston 已验证）。 MAA 的 Wlroots 路线只覆盖 wlroots 系；PipeWire 路线需要用户对录屏弹窗授权。 WCAS 没有这个交互和依赖。

### 3.2 无特权、无虚拟设备

MAA 的 UInput 需要 `/dev/uinput` 权限（root 或 uinput 组），Libei 需要 gamescope 的 EIS socket。 WCAS 使用 `SO_PEERCRED` 同 UID 认证 + owner-only socket，普通用户即可运行，不触碰内核输入子系统。

### 3.3 不打扰真实桌面与光标

MAA 的 wlroots/uinput/libei 注入会移动真实指针、抢占焦点。 WCAS 的输入只写进目标应用的私有连接；读取的是应用所见的设备坐标（`wl_pointer.enter` / `motion`），不操纵用户真实桌面。

### 3.4 应用/窗口边界捕获（隐私与正确性）

MAA 截的是屏幕，需要自行裁剪、定位窗口。 WCAS 直接截取应用 committed buffer，并做 `SurfaceState` pending/committed 分离、用 `xdg_toplevel` 识别 root surface，避免截到无关窗口。

### 3.5 覆盖 GPU 加速场景

WCAS 覆盖 `wl_shm` + DMA-BUF（mmap / Vulkan 导入解 tile / EGL+GBM 解压缩 surface）+ KMS 兜底 + XComposite/XGetImage，可处理只输出 dmabuf 的 Proton/DXVK/VKD3D 游戏。 MAA 的 wlroots screencopy 在部分 GPU 客户端上会受格式/权限限制。

### 3.6 自动 native → XWayland 回退

Java/XToolkit 等不暴露可读 native 帧的 toolkit，WCAS 自动切到 owner-authenticated XWayland 桥。MAA 没有这种应用级回退。

### 3.7 事件驱动的窗口几何跟踪

拦截 `xdg_surface.set_window_geometry`、`xdg_toplevel.set_title` / `move` / `resize` / `configure`，仅在窗口创建和拖动/缩放结束时查询一次合成器 IPC，替代 PID 轮询，拖动时也准确。

### 3.8 基于渲染活跃度的存活/停滞检测

统计 `wl_surface.commit` 与 `wl_callback.done`（presented frames）。即使游戏崩溃但 Wayland 连接不断开，也能判定为停滞。

### 3.9 无撕裂异步截图管线

`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` → VkSemaphore → `vkCmdCopyImageToBuffer`，三槽 latest-wins 调度，不阻塞 Wayland 消息转发。

### 3.10 框架中立

不绑定某个 agent 框架。接入 MAA 时相当于替换/新增一个 controller 后端。

---

## 4. 字段映射

| `MaaLinuxControllerCreate` 字段                                | WCAS 对应                                                       |
| -------------------------------------------------------------- | --------------------------------------------------------------- |
| `screencap_method`                                             | 无需配置；自动 shm → dmabuf → KMS/X11 回退                      |
| `input_method`                                                 | 无需配置；固定走私有 seat（pointer / keyboard / text-input-v3） |
| `wlr_socket_path`                                              | 不需要（不依赖 wlroots）                                        |
| `pw_socket_fd` / `pw_node_id`                                  | 不需要（不走 portal ScreenCast）                                |
| `eis_socket_path`                                              | 不需要                                                          |
| `uinput_path` / `uinput_screen_width` / `uinput_screen_height` | 不需要                                                          |
| `use_win32_vk_code`                                            | 输入发生在应用协议层，键位由 xkb keymap / keysym 决定           |

---

## 5. 局限性

- **必须由 WCAS 启动应用**，不能 attach 任意已在运行的窗口。
- **目标平台是 Wayland 会话**；纯 X11 会话不支持（XWayland 仅作会话内回退）。
- 回退路径需要 `Xwayland` + `xauth`。
- 当前仅支持单平面 RGB DMA-BUF；多平面 YUV 尚未实现。
- **KMS 抓取**依赖 libdrmtap 与 `CAP_SYS_ADMIN`；作为合成器回退时其裁剪按物理像素，输出缩放非 1:1 时可能偏移（此时 `grim -g` 优先，因为用的是布局坐标）。
