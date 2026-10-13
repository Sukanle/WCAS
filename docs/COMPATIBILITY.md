# 兼容性矩阵

`WCAS` 针对一个小矩阵持续验证，并尽力覆盖更广的矩阵。本文件是“支持”含义的唯一权威说明，以及如何扩展该矩阵。

## 支持层级

| 层级 | 含义 |
|---|---|
| **已验证（Validated）** | 在 CI 或维护者的真实硬件上跑过；出问题视为发布阻塞项。 |
| **预期可用（Expected）** | 机制上应当可用（只依赖 `wl_shm` + 我们的私有代理）；尚未实测 —— 欢迎反馈结果。 |
| **不支持（Unsupported）** | 已知不可用；以 issue 跟踪。 |

## 合成器 / 会话

| 合成器 | 会话 | 截图 | 输入 | 层级 | 备注 |
|---|---|---|---|---|---|
| GNOME / Mutter | Wayland | ✅ | ✅ | 已验证 | 主要开发目标。 |
| weston | Wayland | ✅ | ✅ | 已验证 | 用于无头 / 开发测试。 |
| KDE Plasma / KWin | Wayland | ✅ | ✅ | 预期可用 | 请通过 issue 反馈。 |
| Sway / wlroots | Wayland | ✅ | ✅ | 预期可用 | 请通过 issue 反馈。 |
| Hyprland | Wayland | ✅ | ✅ | 预期可用 | 请通过 issue 反馈。 |
| 任意纯 X11 会话 | X11 | — | — | 不支持 | 本项目面向 Wayland；XWayland 桥仅作会话内回退。 |

## 应用 toolkit

启动器会按 toolkit 注入软件渲染 / 环境提示，以产生可读的 `wl_shm` 帧；若某个原生 toolkit 不产生可读帧，高层 API 会改走经过 owner 认证的 XWayland 桥重试。

| Toolkit | 原生 Wayland | XWayland 回退 | 层级 |
|---|---|---|---|
| GTK 3/4 | ✅ | ✅ | 已验证 |
| Qt 5/6（wayland） | ✅ | ✅ | 已验证 |
| Electron / Chromium | ✅ | ✅ | 已验证 |
| SDL2 | ✅ | ✅ | 预期可用 |
| Java / XToolkit | — | ✅ | 预期可用 |
| 仅 GPU / 仅 DMA-BUF 的客户端 | ✅ | ✅ | 预期可用 | 走 DMA-BUF（Vulkan / EGL+GBM 导入，线性 mmap 回退）；无法解码时窗口对齐回退到合成器截图。 |

## 要求回顾

- 处于活动 Wayland 会话的 Linux（`XDG_RUNTIME_DIR`、`WAYLAND_DISPLAY`）。
- 兼容回退需要 `Xwayland` + `xauth`。
- 应用必须通过本库启动（不能 attach 任意窗口）。
- 截图支持 `wl_shm` 与 DMA-BUF（Vulkan / EGL+GBM 导入，线性 mmap 回退）；合成器回退用 grim / KMS。

## 验证一个新目标

1. 对代表性应用运行示例或 CLI：
   `build/Release/wcas-example <app>`（截一张 `wcas-capture.png`），
   或 `build/Release/wcas-test-7-functions --program <app> --cli` 后在 CLI 内用 `ss` / `list`。
2. 确认 `ControlledApp::transport()`（CLI 的 `status`）报告预期的 transport（native Wayland 或 XWayland）。
3. 确认 `capture()`（CLI 的 `ss`）返回非空白帧，且 click/key/scroll/text 输入能到达应用。
4. 提交 PR，在上表中新增一行并把层级改为 **已验证**，注明发行版 + 合成器 / toolkit 版本。
