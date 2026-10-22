# 参考实现对照（Rust agent-seat-linux）

WCAS 的 C++ 实现以 Rust 版 `agent-seat-linux` 为行为参照。本文提炼其中与协议、采集和启动引导相关的关键决策，便于排查“为什么这样实现”。源仓库位于 `/home/sukanle/Code/agent-seat-linux`，其源码路径在原记录中以 `src/...` 形式给出。

## 1. 启动引导

### 代理模式悬挂

- **现象**：游戏崩溃后代理模式不退出。Wine/winewayland 崩溃后 Wayland 连接可能延迟关闭，客户端 `wl_display.disconnect` 未及时到达，代理一直认为连接存活。
- **对策**：三层存活检测
    1. Wayland 代理连接状态；
    2. 子进程存活（`/proc/<pid>` 检查）；
    3. 渲染停滞检测 —— 用 `wl_surface.commit` 计数判断“进程活着但不再出帧”。
- **对应实现**：`harness/session.cpp`、`harness/computer_use.cpp` 的 native 等待循环与 `total_commit_count()` / `presented_frames()`。

### Lutris 包装

- Lutris 自己管理 Wayland 配置，脚本**不能**覆盖它的关键环境变量。
- 只保留 `WINEPREFIX` 等必要变量，并显式设置 `PROTON_ENABLE_WAYLAND=1`。
- 脚本参数与游戏命令必须分离：`--inherit-output`、`--stale-timeout` 属于脚本，其余原样透传给游戏。

### `PROTON_USE_WAYLAND` 不是标准变量

- 标准变量是 `PROTON_ENABLE_WAYLAND=1`；设置 `PROTON_USE_WAYLAND` 会误导 MESA-EGL，导致 `d3d11` 等路径回退 XWayland。
- 正确做法：只设置 `PROTON_ENABLE_WAYLAND`，并保留 `PROTON_USE_WAYLAND` 的**检测** 用途（用于识别 Proton/Wine 模式）。

### GraphicsApi 选择

不同游戏对 DXVK / 原生 Vulkan / WineD3D 的兼容性不同，因此图形后端做成显式选项：

| 选项     | 含义                                                          |
| -------- | ------------------------------------------------------------- |
| `dxvk`   | 默认，Proton 的 D3D→Vulkan 翻译                               |
| `vulkan` | 游戏自带 Vulkan 渲染器（需关闭 DXVK 的 d3d9/d3d11/dxgi 覆盖） |
| `opengl` | WineD3D                                                       |

## 2. 代理层

### 帧提交计数与停滞检测

- 维护每个 surface 的 commit 计数与全局计数；
- 计数在 `wl_surface.commit` 时递增；
- 停滞判定：超过阈值没有新 commit → 判定应用已崩溃（进程可能还活着）。

### 事件驱动的窗口几何

- 不轮询 compositor：拦截 `xdg_surface.set_window_geometry`、`xdg_toplevel.set_title` / `move` / `resize` / `configure`；
- 只在窗口创建和拖动/缩放结束时查询一次合成器 IPC，避免拖动过程中频繁查询。

### `zwp_linux_dmabuf_v1` 必须放行

- 该接口是 GPU 加速应用提交缓冲区的唯一途径；
- 过滤策略应为**白名单优先**：除明确拒绝的接口外全部放行；
- 只拒绝真正危险的接口（如跨客户端全局接口）。

### DMA-BUF 拦截流程

1. 拦截 `zwp_linux_buffer_params_v1.create`（异步，暂存参数，不携带 `new_id`）；
2. 拦截 `create_buffer` / `destroy` / `attach` / `commit`；
3. 在 `created` 事件中登记 buffer 与 params 的对应关系（否则 `capturable_surface` 永远为假）；
4. 保留 plane FD 快照，使 buffer/pool 销毁后仍可捕获已提交帧。

### 乱序协议与生命周期

- 协议消息可能乱序到达，需要容忍 `create` 与 `created` 之间的其它消息；
- buffer/pool 销毁后，已提交帧通过**独立 FD 快照**保留，避免悬空。

## 3. 采集管线

### 后端优先级

```text
capture_frame
  ├─ DRM/KMS（libdrmtap）  —— 绕过用户态显示栈，最终屏幕输出
  ├─ wl_shm                  —— 软件渲染
  └─ dmabuf                  —— 硬件加速（Vulkan/EGL 导入）
```

KMS 需要 `CAP_SYS_ADMIN` 或外部 helper，失败时按顺序回退。裁剪时注意：KMS 裁剪按物理像素，输出缩放非 1:1 时会偏移，因此优先使用合成器工具的布局坐标（`grim -g`）。

### 异步三阶段管线

```text
Stage 1（Wayland 代理线程）：wl_surface.commit → CaptureJob 入队 → 立即返回
Stage 2（CaptureWorker）：EXPORT_SYNC_FILE → VkSemaphore → 导入 Vulkan → copy → 等待
Stage 3（CPU）：staging buffer → 解码 → RGB
```

- `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` 导出的是 **T0 时刻的 fence 快照**，不保证 T0~T2 之间没有新 fence 进入；只读拷贝场景下这一语义可以接受。
- 队列采用 **latest-wins**：同一 surface 只保留最新任务，worker 来不及时丢弃旧帧。
- 超时保护：worker 等待 fence 有 500ms 上限，GPU 提交也有上限，避免永久挂起。
- 原则：**CPU 不直接理解厂商 tiled 布局**，一律交给 Vulkan 解码。

### DMA-BUF ioctl ABI 教训

- `DMA_BUF_IOCTL_SYNC` / `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` 的结构体字段宽度直接参与 `_IOC` 编码；
- 用 `u64` 而不是 `u32/i32` 会让内核算出不同的命令号并返回 `ENOTTY`；
- 探针程序一直成功、同步程序一直失败，通常就是 ABI 宽度不一致。

### Vulkan 导入排查清单

- `vkCmdCopyImageToBuffer` 之后的 staging buffer 是**紧密排列**的，解码时按 `width * 4` 而非源 stride；
- `SubresourceLayout::size` 必须是 `width * height * 4`，否则读出条带状损坏；
- DMA-BUF modifier 是 **64 位**，高低位都参与解析；只取低 32 位会造成画面撕裂；
- “导入成功”不等于“像素正确”：驱动可能接受未声明支持的 modifier。

## 4. 文件拆分与窗口身份链

Rust 参考实现把大文件拆成目录模块（`proxy/`、`capture/`、`vulkan_dmabuf/`、`harness/`），拆分过程中暴露了 4 个窗口截图身份链缺陷，WCAS 移植时同样需要避免：

1. `xdg_surface` → `wl_surface` 映射错误（最关键）；
2. 缺少 `surface → toplevel` 反向映射；
3. 用启发式挑 primary surface，而不是由目标驱动；
4. 诊断日志不完整，无法定位归属。

WCAS 对应实现见 `src/skl/wcas/proxy/conn.cpp` 的 role 映射与 `src/skl/wcas/proxy/window_source.cpp`。
