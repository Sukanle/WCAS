# WCAS 高级设置

本文件保存 README 之外的高级使用说明：图形后端、TOML profiles、CLI 参数、DMA-BUF 采集、IPC、已知问题、安全模型和项目结构。README 只保留项目介绍和快速开始。

## 启动与高级设置

### 图形后端怎么选

`graphics`（`--graphics-api`）只控制 Proton 为这条进程树准备的 D3D 转译层，**不影响游戏自己的 Vulkan**：

| 值       | 含义                                             | 典型用途                              |
| -------- | ------------------------------------------------ | ------------------------------------- |
| `dxvk`   | D3D9/10/11 → DXVK → Vulkan                       | 只支持 D3D11 的游戏，或规避启动器白屏 |
| `vulkan` | 不启用 DXVK，游戏原生 Vulkan 直接走 `winevulkan` | 支持 Vulkan 的游戏（Endfield）        |
| `opengl` | WineD3D / OpenGL                                 | QtWebEngine 启动器、老游戏            |

因为原生 Vulkan 不经过 D3D，启动器可以使用 `opengl`/WineD3D，而它拉起的游戏仍走原生 Vulkan；前提是游戏命令行**不带** `-force-d3d11`。

### 通过 Proton 包装脚本（推荐）

`proton-inject.sh` 会自动设置 `WINEPREFIX`、`WAYLAND_DISPLAY` 指向私有代理，并按目标程序推断匹配规则。常用示例：

明日方舟（手游移植，OpenGL / WineD3D）：

```sh
./proton-inject.sh /<Proton游戏容器根路径>/drive_c/Program\ Files/Hypergryph\ Launcher/games/Arknights/Arknights.exe --graphics-api opengl --cli
```

Lutris 用 `proton-inject-lutris.sh`。

### QtWebEngine 启动器（例如鹰角启动器）

QtWebEngine 启动器可以走 native Wayland，但需要使用 **WineD3D/OpenGL**；当前 DXVK/D3D11 内容经过 WCAS 私有代理会渲染成空白窗口：

```sh
# 推荐：启动器 native Wayland + WineD3D
./proton-inject.sh "/path/to/Launcher.exe" --transport wayland --graphics-api opengl

# 回退：启动器及子进程走 XWayland
./proton-inject.sh "/path/to/Launcher.exe" --transport xwayland

# 如果仍希望由启动器自己启动游戏，可尝试其他图形后端
./proton-inject.sh "/path/to/Launcher.exe" --transport wayland --graphics-api vk
./proton-inject.sh "/path/to/Launcher.exe" --transport wayland --graphics-api gl
```

如果想让启动器继续负责启动游戏，可以自行尝试 `vk`（游戏原生 Vulkan）或 `gl`（WineD3D）作为启动后端；**推荐优先尝试 `gl`**，因为大部分游戏厂商的启动器基于 QtWebEngine，而 DXVK/D3D11 内容经过 WCAS 私有代理时容易白屏。某一种后端出现白屏或黑屏时，切换到另一种再试；两者都不行再回退 `--transport xwayland`。

启动器使用 `opengl` 时，它拉起的游戏不会被强制成 WineD3D：D3D 转译层只影响 D3D API，游戏的原生 Vulkan 仍然通过 `winevulkan` 走宿主驱动。需要游戏走 DXVK 时，才需要下面的 per-exe 方案。

### 启动器 WineD3D + 游戏 DXVK（per-exe）

标准 Proton 在 `graphics = "dxvk"` 时会注入全局：

```text
WINEDLLOVERRIDES=...;d3d11=n;d3d10core=n;d3d9=n;dxgi=n;...
```

环境变量优先于 per-exe 的 `AppDefaults`，所以需要自定义 Proton wrapper 把这些全局 D3D 项去掉，同时保留前缀里的 DXVK DLL：

```sh
./scripts/wcas-proton-per-exe-wrapper.sh
```

然后 launcher profile 使用：

```toml
transport = "wayland"
graphics = "dxvk"
env = { PROTONPATH = '~/.cache/wcas/proton-per-exe', WCAS_PROTON_PER_EXE = '1' }
```

再设置 per-exe：

```sh
WINEPREFIX="$HOME/Games/hypergryph" \
  ./scripts/wcas-launcher-dll-overrides.sh apply

WINEPREFIX="$HOME/Games/hypergryph" \
  ./scripts/wcas-launcher-dll-overrides.sh apply-native Endfield.exe
```

效果：

```text
Launcher.exe / QtWebEngineProcess.exe / Games.exe
  → d3d9/d3d10/d3d10core/d3d11/dxgi = builtin（WineD3D，启动器正常）

Endfield.exe
  → 这些 DLL = native（前缀里的 DXVK）
```

`apply-native` 是必要的：wrapper 去掉全局 `d3d11=n` 后，Wine 对 system32 DLL 的默认加载顺序可能回退到 builtin，导致游戏也走 WineD3D。游戏 exe 需要显式 native。

恢复：

```sh
WINEPREFIX="$HOME/Games/hypergryph" \
  ./scripts/wcas-launcher-dll-overrides.sh remove

WINEPREFIX="$HOME/Games/hypergryph" \
  ./scripts/wcas-launcher-dll-overrides.sh remove-native Endfield.exe
```

启动后用以下命令确认：

```sh
pid=$(pgrep -f 'Arknights Endfield/Endfield.exe' | head -1)
grep -aiE 'd3d11|dxgi' /proc/$pid/maps
```

启动器进程应加载 Proton 的 builtin `d3d11.dll`/`dxgi.dll`，游戏进程应加载：

```text
$WINEPREFIX/drive_c/windows/system32/d3d11.dll
$WINEPREFIX/drive_c/windows/system32/dxgi.dll
```

注意：如果游戏命令行带有 `-force-d3d11`，游戏会走 **D3D11 → DXVK**，而不是游戏原生 Vulkan。per-exe override 只能决定 D3D11 走 WineD3D 还是 DXVK，不能把 `-force-d3d11` 变成原生 Vulkan；要原生 Vulkan，先按上一节关闭“强制 DX11”。

如果希望游戏本体走 native Wayland，不要依赖启动器拉起的子进程（它会继承启动器的环境），而是让 WCAS 直接启动游戏：Arknights 可用 `--profile arknights` 直启。启动器只负责登录/更新。

### TOML launch profiles

`examples/wcas-profiles.toml` 定义了启动器与 Arknights 的 profile。示例：

```sh
./proton-inject.sh \
  --config examples/wcas-profiles.toml \
  --profile arknights --serve
```

`dependencies` 指向需要的启动器 profile。

### 直接用 CLI

```sh
build/Release/proton-inject \
  --program /path/to/app --umu \
  --env WINEPREFIX=/path/to/prefix \
  --capture-backend dmabuf --graphics-api opengl \
  --native-timeout 25 --cli
```

主要启动参数：

| 参数                            | 说明                                                                                               |
| ------------------------------- | -------------------------------------------------------------------------------------------------- |
| `--program, -p`                 | 要启动的可执行文件                                                                                 |
| `--arg, -s` / `--env, -e`       | 传给程序的参数 / 环境变量（可重复）                                                                |
| `--proton` / `--wine` / `--umu` | 启动方式                                                                                           |
| `--graphics-api, -g`            | `dxvk`(默认) 或 `vulkan`(`vk`，游戏原生) 或 `opengl`(`gl`/`wined3d`)                               |
| `--transport`                   | `auto`(默认) / `wayland`(native) / `xwayland`(x11)                                                 |
| `--config FILE`                 | 读取 TOML launch profile                                                                           |
| `--profile ID`                  | 配合 `--config` 选择 profile；缺省用 `default`                                                     |
| `--list-profiles`               | 列出配置中的 profile 和依赖关系                                                                    |
| `--capture-backend, -b`         | `client`(默认) / `shm` / `dmabuf` / `kms`                                                          |
| `--bridge-width/-height, -x/-y` | XWayland 回退桥接分辨率                                                                            |
| `--match-regex, -r`             | 额外的子窗口进程匹配正则（默认取目标程序父目录）                                                   |
| `--output`                      | 启动后把窗口移到指定 Hyprland 显示器（如 `DP-2`）                                                  |
| `--key-delay`                   | 发送 key 前等待秒数（便于手动聚焦）                                                                |
| `--allow-activation`            | native Wayland 下放行选中窗口的激活请求，使其接收键盘（会夺取焦点）；XWayland 焦点在私有 bridge 内 |
| `--debug-all`                   | 打开全部调试类别（等价 `WCAS_DEBUG=1`）                                                            |
| `--debug-capture`               | 采集/截图调试（调度器丢弃、槽超时）                                                                |
| `--debug-dmabuf`                | DMA-BUF 导入/回读诊断（`[wcas:dmabuf]`）                                                           |
| `--debug-harness`               | 启动/native 等待/XWayland 回退（`[wcas:harness]`）                                                 |
| `--debug-proxy`                 | 连接生命周期与消息转发（`[wcas:proxy]`）                                                           |
| `--debug-input`                 | 注入的指针/键盘输入（`[wcas:input]`）                                                              |
| `--debug-xwayland`              | XWayland 桥（`[wcas:xwayland]`）                                                                   |
| `--debug-wire`                  | 逐条 Wayland 消息追踪（`[wcas:wire]`）                                                             |

调试类别可用各自的环境变量单独开启：`WCAS_DEBUG_HARNESS` / `WCAS_DEBUG_PROXY` / `WCAS_DEBUG_INPUT` / `WCAS_DEBUG_CAPTURE` / `WCAS_DEBUG_DMABUF` / `WCAS_DEBUG_XWAYLAND` / `WCAS_DEBUG_WIRE`；`WCAS_DEBUG=1` 打开全部。追踪行格式为 `[wcas:<类别> t=<单调毫秒>] ...`，时间戳与 `WINEDEBUG` 同源，便于对齐时序。

CLI 模式下的命令：

```
screenshot|ss [now]          截图（默认等待新帧；ss now 立即截图）
list|ls                      列出窗口，* 标记当前目标
sessions                     列出全部代理会话（含 kbd/ptr/keymap 诊断）
surfaces                     列出全部 surface（role/parent/toplevel/frame/seq）
target <index|app_id|title>  切换目标窗口
move <x> <y>                 移动到坐标
click [<x> <y>]              在坐标点击（省略坐标则点当前位置）
click <hold> <Top|Down|Left|Right|Center> [px]
                             长按滑动，如 `click 4s R 300`
drag <dx> <dy>               不按键移动视角/光标（旋转相机，不触发点击），如 `drag 300 -120`
up|down|left|right|center    相对移动 / 居中
key|press <combo> [hold]     组合键，如 `key ctrl+s`、`key Alt-C`、`key Enter 3s`
escape|esc / type <text>     单键与文本输入
cursor|pxy                   打印当前光标位置
monitor <DP-1|DP-2|...>      把选中窗口移到指定 Hyprland 显示器
activation on|off            放行/禁止选中窗口的激活请求
status                       进程、transport、commit/frame 计数、窗口尺寸
help|h / quit|exit|q
```

交互模式下命令行由 [cmdly](https://github.com/Sukanle/cmdly) 提供：支持行内编辑（←/→/Home/End 及退格）、↑/↓ 历史、Tab 补全命令名，Ctrl-C / Ctrl-D 退出。stdin 不是终端时（脚本管道）自动退回逐行读取，不输出提示符与回显。

常用工作流：

```
list                 # 看有哪些窗口
target 1             # 选中主窗口
ss                   # 截一张图（等待新帧）
click 4s R 300       # 长按 4 秒并向右滑 300px
drag 300 -120        # 向右上移动视角（旋转相机，不触发点击）
key Alt-C            # 发送 Alt+C
```

### DMA-BUF 采集后端

游戏窗口通常通过 DMA-BUF 提交 `wl_buffer`，其布局由 DRM modifier 描述（可能是厂商 tiled / block-linear，甚至带帧缓冲压缩）。WCAS 用环境变量选择解码后端：

| 变量                  | 取值                            | 说明                                                               |
| --------------------- | ------------------------------- | ------------------------------------------------------------------ |
| `WCAS_DMABUF_BACKEND` | `auto`(默认) / `vulkan` / `egl` | DMA-BUF 解码后端                                                   |
| `WCAS_CAPTURE_TIMING` | 设 `1` 打开                     | 每次采集打印 `[wcas] capture timing: backend=... <ms>`（默认关闭） |

三个后端：

- **`vulkan`** —— `VK_EXT_image_drm_format_modifier` 显式导入 + `vkCmdCopyImageToBuffer` → staging → CPU 解码；只在设备为“该格式 + modifier + `TRANSFER_SRC`”声明支持时可用。
- **`egl`** —— EGL/GBM（`EGL_EXT_image_dma_buf_import[_modifiers]`）导入 → GL 纹理 → FBO → `glReadPixels`。合成器同款路径，能解出 NVIDIA 帧缓冲压缩（modifier `c != 0`）的 surface。
- **`auto`** —— 按**能力查询**选择：线性或 Vulkan 声明支持 → Vulkan；否则若 EGL 广播了该格式+modifier → EGL；两者都不支持则**明确拒绝**，不会把驱动在不支持的布局上解出的花屏当成成功。

原则是“导入成功 ≠ 像素正确”：Vulkan 路径对未被声明支持的 modifier 会标 `modifier_supported=NO | pixels=unverified`，而不是谎称已验证。

当窗口自身的 buffer 两条路径都无法解码时，`auto` 会**窗口对齐**回退到合成器截图：先用 `swaymsg`/`hyprctl`/`xdotool` 求出窗口矩形，再用 `grim -g`（合成器布局坐标，缩放安全）或 KMS 裁剪读取合成结果，不会返回整屏或多显示器拼接。完整判定与能力查询见 [CAPTURE.md](CAPTURE.md)。

### IPC 会话（MAA 接管）

`proton-inject.sh` 默认以 `--serve` 启动一个常驻会话，并通过 Unix domain socket 暴露控制接口；这样用户从快捷方式启动游戏后，MAA 等外部程序可以 attach，而不需要自己重新启动游戏。

```sh
# 默认：serve 模式（不读 stdin，适合快捷方式）
./proton-inject.sh game.exe

# 交互调试；或显式禁用 serve
./proton-inject.sh game.exe --cli
./proton-inject.sh game.exe --no-serve

# 自定义 socket / 脱离终端
./proton-inject.sh game.exe --socket /run/user/1000/wcas/my.sock --detach
```

客户端：

```sh
wcasctl list                               # 列出活动 socket
wcasctl status                             # pid/frame/windows/transport
wcasctl windows                            # 窗口列表
wcasctl capture --out shot.png             # PNG 截图
wcasctl capture-sdm --out frame.bgr        # 共享内存 raw BGR（无 PNG/base64）
wcasctl frames-subscribe 30                # 后台连续写入 SDM ring（30fps）
wcasctl frames-status                      # 查看持续发布状态/seq
wcasctl frames-unsubscribe                 # 停止持续发布
wcasctl click 640 360                      # frame 坐标
wcasctl move 640 360
wcasctl relative 20 -10                    # 无按键相对移动（pointer-locked 视角）
wcasctl down 640 360 left                  # 按下按钮，配合 move/up 做拖拽
wcasctl up left
wcasctl drag 640 360 800 500 100           # 按下、移动、释放；最后一个参数是 hold_ms
wcasctl key ctrl+a
wcasctl key-down Control                   # 保持按下，配合 key-up 做 KeyDown/KeyUp
wcasctl key-up Control
wcasctl type hello
wcasctl stop                               # 结束受控游戏
```

socket 默认位于 `$XDG_RUNTIME_DIR/wcas/<pid>.sock`，`latest` 指向最近会话；连接使用 `SO_PEERCRED` 校验同 UID，目录 0700、socket 0600。协议为长度前缀 JSON。

除 `pointer.move` / `pointer.click` / `pointer.scroll` / `key` 外，IPC 还提供面向 MaaFramework 等控制器的**边沿输入**：`pointer.down`、`pointer.up`、`pointer.drag`、`pointer.relative`、`key.down`、`key.up`。`pointer.relative` 发送无按键的相对指针运动，适合 pointer-locked 游戏；目标启用 `zwp_relative_pointer_v1` 时走原生相对事件，未启用时回退到基于最后已知位置的绝对移动。`key.down`/`key.up` 会分别保留和释放对应的修饰键，因此可以同时存在多个独立的 KeyDown。

截图有两种数据面：

- `capture`：PNG + base64，适合低频 OCR；
- `capture-sdm`：返回共享内存 channel，客户端直接读取 raw BGR/BGRA，避免 PNG/base64 和 UDS 像素传输；帧带 `width/height/stride/format`，由客户端按 stride 解析。
- `frames-subscribe`：后台定时捕获并持续写入同一 ring；`frames-status` 返回 `running/attempts/writes/errors` 与最新 `seq`，MAA 可自行 `wait_for_frame`。

XWayland bridge 的合成器窗口不再固定显示为 `org.freedesktop.Xwayland` / `Xwayland on :N`。WCAS 会把 Xwayland 根 toplevel 的 app_id/title 改写为目标可执行文件名：

```text
class: wcas-<stem>        # 例如 wcas-launcher
title: <filename>         # 例如 Launcher.exe
```

因此可以在 Hyprland 中只对该启动器做规则，例如：

```conf
windowrulev2 = float, class:^(wcas-launcher)$
windowrulev2 = center, class:^(wcas-launcher)$
```

`umu-run` 会使用其第一个游戏参数作为名称来源；普通 `--program` 使用程序路径本身。

键盘注入需要目标窗口获得键盘焦点；若窗口不接收键盘，可在启动时加 `--allow-activation`，或先用 `move/click` 激活目标窗口。

## 已知问题

### 明日方舟（手游移植）按键无效

现象：主窗口（`Arknights.exe` / 明日方舟）**鼠标点击正常，但按键无效**（例如 Esc 不触发返回）。

排查结论：**不是 WCAS 注入层的问题。** 用自建 native 探针验证，注入完全正确：

- `key Enter 1s` → 客户端收到 `wl_keyboard.key key=28`（evdev Return）按下…释放
- `key Alt-C 500ms` → `Alt`(56)↓、`c`(46)↓↑、`Alt`↑

同一套注入在其它游戏、以及 Qt/GTK/Electron 等应用上均正常。明日方舟 PC 端是从手游移植，其键盘处理（很可能走 RawInput / 只认真实前台窗口）比原生 PC 端（如终末地）弱，对合成注入不响应。**这是游戏自身的行为，非 WCAS 缺陷。**

已知缓解手段（均未能让明日方舟响应合成按键，但值得记录）：

- `--allow-activation`：放行选中窗口的 `xdg_activation_v1.activate`，使其被合成器激活；
- `--key-delay N`：发送前等待，手动聚焦窗口；
- `activation on` + 点击窗口后再按键。

后续可能的方向：改走 X11/XWayland 注入（Wine 对 X11 输入处理更成熟），或在合成器层真正激活窗口后再注入。

### 终末地启动偶发退出（Exit 222）

现象：经 WCAS 启动《明日方舟：终末地》时，**偶尔**在加载初期（LOGO 之后）进程整体退出，退出码 **222**（腾讯 WeTest CrashSight 致命异常）。

排查结论：**根因是腾讯 ACE 反作弊驱动与 Wine/Proton 的 win32 内核接口不兼容，不是 WCAS 的问题。**

- 游戏日志可见 ACE 驱动调用 Wine 未实现的内核函数，Wine 直接 abort：
  ```
  wine: Call from ... to unimplemented function ntoskrnl.exe.InbvAcquireDisplayOwnership, aborting
  wine: Call from ... to unimplemented function ntoskrnl.exe.SeQueryInformationToken, aborting
  ```
- 这些 abort 在**不经 WCAS、直连 `umu-run`** 时同样出现；直连通常存活，经 WCAS（私有 Wayland 代理）时偶发致命。代理只改变了时序，且逐条消息对比确认**代理没有漏转发或改写消息**——根因不在 WCAS。
- 相关上游：DW-Proton issue #64 曾据此提交 `ntoskrnl` stub 补丁，但被维护者判定为启动参数误判并关闭（stock Proton 可正常启动）。

缓解：换用已验证的 Proton（GE / Experimental / CachyOS），或关注 Wine/DW-Proton 对上述 `ntoskrnl` 函数的支持。WCAS 侧无法修复 Wine 内部未实现的接口。

### 其它已知限制

- **多平面 / YUV DMA-BUF** 不支持，遇到会捕获失败。
- **压缩 surface 依赖 EGL 后端**：NVIDIA 帧缓冲压缩（modifier `c != 0`）在 Vulkan 上既不被列出也不支持 `TRANSFER_SRC`/`SAMPLED`，只能经 EGL/GBM 解出。若某设备两个后端都不广播该布局，WCAS 会明确拒绝而不是输出花屏。
- **KMS 抓取**依赖 libdrmtap 与 `CAP_SYS_ADMIN`；作为合成器回退时其裁剪按物理像素，输出缩放非 1:1 时可能出现偏移（此时 `grim -g` 优先，因为用的是布局坐标）。
- **X11 焦点**：XWayland 使用私有 bridge 的 `XSetInputFocus`，不会影响真实桌面焦点；不支持通过 `_NET_ACTIVE_WINDOW` 激活真实桌面上的 X11 窗口。
- 某些应用（如公告层、CEF）会**泄漏大量 surface** 且从不销毁；WCAS 用提交新近度区分活跃窗口，但历史 surface 仍会累积。
- **激活与不抢焦点天生冲突**：要让 Unity/Wine 收键通常需要窗口被激活，而激活会带走用户焦点。新窗口抢焦点主要是合成器（如 Hyprland 默认聚焦新 toplevel）行为，可用窗口规则缓解：

  ```conf
  misc {
      focus_on_activate = false
      initial_workspace_tracking = 0
  }
  windowrulev2 = noinitialfocus, class:^(arknights\.exe)$
  ```

## 安全模型

WCAS 代理显示协议并合成输入，属于**安全敏感基础设施**。涉及认证、协议过滤、进程归属或清理的改动需要显式评审。核心约束：

- 仅接受**同 UID** 的对端（内核 `SO_PEERCRED`）
- 私有 Wayland socket 权限 `0600`
- 永不向客户端暴露特权（跨客户端）Wayland 接口
- **不实现**环境级全桌面捕获
- 受控子进程设置 `PR_SET_PDEATHSIG` 并被显式回收

WCAS **不**为受控应用提供沙箱：被控应用仍拥有调用方用户的全部文件系统、网络与 D-Bus 权限，除非调用方另行隔离。WCAS 也**不是**用来隐藏窗口的——被控进程对桌面用户依然可见。

## 项目结构

```
include/SKL/               skl 公共头
  WCAS/                    WCAS 公共契约（WCAS / Controller / Capture / Frame / Window /
                           Transport / Types / Error / DRM / DMABUF）
  WCAS/Private/            内部头（Debug、Harness、Proxy、DMABUF、X11Capture、XWayland）
src/SKL/WCAS/
  Error.cpp                WCAS EPCD 静态错误域、CategoryRegistry 注册与错误描述
  DRM.cpp                  DRM/KMS 与窗口几何
  DMABUF.cpp               DMA-BUF 能力探测
  Private/Proxy/           Wayland 代理：Reactor、wire 翻译、Interfaces、surface 观察、
                           输入、截图槽位/调度、DMA-BUF 解码、Vulkan / EGL+GBM 导入
  Private/Harness/         ComputerUse、ControlledApp、ProcessHost、SessionContext、
                           Capture/Window/Focus/Health/Target/FrameBus/LaunchStrategy、
                           X11 XTest 输入
  Private/XWayland.cpp     XWayland bridge
  Private/X11Capture.cpp   X11/XComposite 抓取与窗口枚举
cmake/
  Dependencies.cmake       find_package / FetchContent 依赖引导（EPCD、ABIX、cmdly、libdrmtap）
  Findlibdrmtap.cmake      已安装 libdrmtap 的查找模块
dependencies/              CMake 自动克隆的依赖源码（按仓库名分目录，除 README.md 外 git 忽略）
codegen/                   由协议 XML 生成 wire 元数据（构建期执行）
scripts/                   维护脚本（Markdown reflow 等）
tests/                     单元测试与协议集成测试（fake Wayland 环境）
docs/                      项目文档（INTERFACE / STATUS / REFERENCE / CAPTURE / COMPATIBILITY /
                           CONTROLLER / FRAMES / MAA-COMPARISON / PROTON / ADVANCED / SECURITY）
main.cpp                   库使用示例
proton-inject.cpp          CLI
proton-inject*.sh          Proton / Lutris 启动包装
```
