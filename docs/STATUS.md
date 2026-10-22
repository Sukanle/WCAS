# 迁移状态与验证记录

本文记录 WCAS 的能力清单、阶段计划、测试矩阵与历史关键修复，作为开发时的对照材料。内容来自 C++ 迁移过程中的工程记录，已按当前代码状态整理。

## 1. 当前状态

| 状态 | 项目                        | 说明                                                                                                             |
| ---- | --------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| [x]  | 协议感知代理基础            | 已脱离生产路径中的原始字节透明 relay。                                                                           |
| [x]  | typed protocol/object model | 覆盖 core Wayland、`wl_shm`、seat/input 对象创建和常用 xdg-shell 请求。                                          |
| [x]  | 消息 framing                | 支持部分读取、合并消息和基础格式校验；空字符串参数不再被误判为畸形。                                             |
| [x]  | 双对象空间                  | app-facing 与 upstream-facing ID 独立分配并维护双向映射。                                                        |
| [x]  | 参数与 FD 翻译              | 支持标量、对象、`new_id`、字符串、数组和 `SCM_RIGHTS` FD 转发。                                                  |
| [x]  | registry 过滤               | 支持 global allowlist/denylist。                                                                                 |
| [x]  | 核心对象观察                | 跟踪 `wl_surface`、`wl_buffer`、`wl_shm_pool`、`xdg_surface`、`xdg_toplevel`、`wl_callback`。                    |
| [x]  | `wl_shm` 元数据与捕获       | pool/buffer 生命周期、FD 保留、mmap 以及 ARGB/XRGB 解码。                                                        |
| [x]  | surface commit 语义         | attach、commit、空 attach 清帧和 commit 计数。                                                                   |
| [x]  | retained frame 生命周期     | buffer/pool 销毁后，已提交帧仍可通过独立 FD 快照捕获。                                                           |
| [x]  | xdg 目标跟踪                | title、app_id、surface/toplevel 关系及 `CaptureTarget` 选择。                                                    |
| [x]  | native readiness            | 由连接、registry 握手、可捕获 surface 和稳定性检查驱动。                                                         |
| [x]  | 进程组清理                  | `SIGINT`→`SIGTERM`→`SIGKILL` 顺序清理与 launcher 回收；`PR_SET_PDEATHSIG`。                                      |
| [x]  | 诊断输出门控                | 代理不再无条件打印；`WCAS_DEBUG` 显式开启，按类别门控。                                                          |
| [x]  | 第三阶段输入                | keymap FD、xkbcommon 布局解析、modifier、Unicode、text-input-v3、pointer/scroll/drag。                           |
| [x]  | DMA-BUF 与 GPU 捕获         | Vulkan / EGL+GBM 导入、线性 mmap 回退、合成器回退、三槽调度、窗口几何。                                          |
| [x]  | XWayland 回退               | 独立 Xwayland bridge、native 失败回退、XComposite 捕获、XTest 输入。                                             |
| [x]  | 能力 Interface 化           | Controller、Capture、DmaBufDecoder、FrameSink、Window、Transport 等按能力拆分，见 [INTERFACE.md](INTERFACE.md)。 |
| [/]  | KMS 直接 ioctl              | 当前走 libdrmtap，真实捕获依赖外部 helper/权限，暂缓。                                                           |
| [ ]  | WinAPI / Cocoa 后端         | 未开始。                                                                                                         |
| [ ]  | 多平面 / YUV DMA-BUF        | 目前只支持单平面 RGB。                                                                                           |

## 2. 阶段计划

| 状态 | 阶段                                    | 通过条件                                                                                           |
| ---- | --------------------------------------- | -------------------------------------------------------------------------------------------------- |
| [x]  | 第一阶段：AgentSeat 与原生 Wayland 代理 | fake compositor/client 覆盖 registry、对象、`new_id`、FD、销毁、surface commit、断开和超时。       |
| [x]  | 第二阶段：`wl_shm` 捕获基础             | pool/buffer 元数据、FD 所有权、pending/committed buffer、mmap 解码、目标选择、销毁后保留已提交帧。 |
| [x]  | 第二阶段补全：wire 协议元数据           | 由协议 XML/生成头文件产出完整 wire 表，替代手工分支与不可用的客户端接口表。                        |
| [x]  | 第三阶段：输入                          | 键盘 keymap FD、xkbcommon 布局、modifier、Unicode、text-input-v3、指针/滚轮/拖拽。                 |
| [x]  | 第四阶段：DMA-BUF 与 GPU 捕获           | 格式、offset/stride、回退、Vulkan 导入、三槽生命周期与 FD 泄漏测试通过。                           |
| [/]  | 第四阶段补全：KMS                       | 改为直接 DRM/KMS ioctl，去除外部 helper 依赖；无 `CAP_SYS_ADMIN` 时优雅降级。                      |
| [/]  | 第五阶段：XWayland                      | 桥接启停、回退状态机、XComposite 捕获与 XTest 输入已落地；X11-only 客户端端到端仍需真实环境验证。  |
| [/]  | 第六阶段：CLI 与脚本 parity             | 参数、窗口管理、长按/滑动、显示器、诊断已支持；Proton/UMU 端到端脚本仍缺。                         |
| [x]  | 第七阶段：能力 Interface 化             | 契约、provider、registry、绑定期解析与显式失败语义齐备，见 [INTERFACE.md](INTERFACE.md)。          |

## 3. 测试矩阵

当前 CTest 共 28 项（`ctest --test-dir build/Debug`）：

| 类别             | 测试                                                                                                                                                                                                                     |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 协议与对象       | `wcas-proxy-integration`、`wcas-unit-tests`                                                                                                                                                                              |
| 输入             | `wcas-input-injection`                                                                                                                                                                                                   |
| DMA-BUF / 采集   | `wcas-dmabuf-capture`、`wcas-dmabuf-probe`、`wcas-capture-scheduler`、`wcas-capture-pipeline`、`wcas-kms-capture`、`wcas-window-geometry`                                                                                |
| X11 / XWayland   | `wcas-x11-capture`、`wcas-xwayland-bridge`、`wcas-xwayland-fallback`                                                                                                                                                     |
| Interface 契约   | `wcas-controller-interface`、`wcas-capture-interface`、`wcas-window-interface`、`wcas-dmabuf-decoder-interface`、`wcas-transport-interface`                                                                              |
| Interface 运行时 | `wcas-frame-bus`、`wcas-frame-pump`、`wcas-capture-runtime`、`wcas-runtime-bindings`、`wcas-process-host`、`wcas-focus-backend`、`wcas-health-monitor`、`wcas-target-selector`、`wcas-launch-strategy`、`wcas-x11-input` |
| CLI              | `wcas-cli-smoke`                                                                                                                                                                                                         |

仍缺：未知 opcode / 版本越界 / 畸形签名的**负向**协议测试。

## 4. 生命周期与通用要求

| 状态 | 要求                                                                                                                                                               |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| [x]  | launcher 使用独立 process group，并按 `SIGINT` → 有限等待 → `SIGTERM` → `SIGKILL` 清理后代。                                                                       |
| [x]  | 子进程设置 `PR_SET_PDEATHSIG`；代理连接、PID 注册和 socket 状态在销毁路径上清理。                                                                                  |
| [/]  | socket / FD / mmap / 对象映射 / 子进程均使用完整 RAII 生命周期。已知待收尾：输入注入由调用方线程直接写 `client_fd`，与 `run()` 线程共用同一 socket，约定为单写者。 |
| [/]  | `ConnState` 的映射表由 `data_mutex` 保护，但 `run()` 与注入路径的边界仍需进一步收紧。                                                                              |
| [x]  | 单元测试与桌面会话无关，协议集成测试使用 fake Wayland 环境。                                                                                                       |
| [ ]  | 每个阶段完成后同步更新本状态文档。                                                                                                                                 |

## 5. 关键修复（历史坑）

以下问题只有真实 compositor / 真实客户端才会暴露，测试环境容易漏掉：

1. **wire 元数据来源错误（根因级）**
    - libwayland 的 `wl_interface` 是**客户端接口表**，`release`/`destroy` 与相邻 opcode 合并，之后所有 opcode 偏移；
    - 系统 `wayland.xml` 也不是完整 wire 描述（`wl_registry.bind` 实际是 `bind(name:uint, interface:string, version:uint, id:new_id) = "usun"`）；
    - 现行方案：构建期由 `codegen/generate_core_wire_meta.py`（`wayland.xml` + `wayland-server-protocol.h`）与 `generate_wire_meta.py`（扩展协议 XML）生成权威表；缺失输入直接 `FATAL_ERROR`，不回退到易漂移的副本。

2. **upstream 对象 ID 必须连续**
    - 真实 Wayland server 用 `wl_map_insert_at` 校验客户端 `new_id` 落在其分配器水位之下；
    - 高位基址或独立计数器都会漂移被拒；因此 upstream 侧使用**恒等映射**。

3. **event 的 `new_id` 目标接口**
    - `wl_data_device.data_offer` 等事件的 `new_id` 目标不在 request 表中；
    - 需从 XML 的 `new_id` 属性解析，否则角色丢失，native 连接被拆。

4. **destroy 启发式**
    - 旧实现把 opcode 0 一律当销毁，而 `wl_pointer.set_cursor` 就是 0；
    - 现要求方法名恰好是 `destroy` 且不是 `new_id` 方法。

5. **DMA-BUF ioctl ABI**
    - `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` 的结构体必须用 `u32`/`i32`；
    - 用 `u64` 会让 `_IOC` 编码变化，内核返回 `ENOTTY` 而非真实“不支持”。

6. **坐标空间**
    - 捕获得到 frame 像素，`wl_pointer` 使用 surface-local 逻辑像素；
    - 需跟踪 `wp_viewport.set_destination`、`wl_surface.set_buffer_scale`、`wp_fractional_scale_v1` 后换算。

7. **实时性**
    - 以 `wl_surface.frame` → `wl_callback.done`（“新帧已上屏”）作为屏障，而不是只看 commit 计数。

8. **窗口身份链**
    - `xdg_surface` → `wl_surface` 映射错误、缺少 `surface → toplevel` 反向映射、启发式选择 primary surface，会让主窗口不可见或错选子窗口。

9. **退出崩溃**
    - detached 的连接线程在进程退出时访问已释放静态状态 → 段错误；
    - 现所有连接线程可 join，退出路径显式回收。

10. **DMA-BUF 重复导入的 FD 归属**
    - 同一 DMA-BUF 被多次导入时 fd 归属必须唯一，否则导入到错误的 Vulkan 内存。

## 6. 验证方式

```sh
cmake -B build/Debug -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug --parallel
ctest --test-dir build/Debug --output-on-failure
```

> 始终使用**独立构建目录**。历史上曾因混用旧的构建产物误判“协议元数据损坏”，也曾因陈旧产物误判“回归”。验证一律从零构建。

**通过标准**：一个阶段不能仅因能编译或能启动进程而通过。必须同时具备与参考实现一致的行为、针对关键协议与生命周期的集成测试、明确的错误处理，以及经阶段测试验证的资源清理。未勾选的项目不得在状态文档或脚本输出中宣称已完成。

## 7. 已知崩溃（与 WCAS 无关）

《明日方舟：终末地》经 WCAS 启动时偶发 **Exit 222**（腾讯 WeTest CrashSight 致命异常）：

```text
wine: Call from ... to unimplemented function ntoskrnl.exe.InbvAcquireDisplayOwnership, aborting
```

不经 WCAS、直连 `umu-run` 同样出现，根因是腾讯 ACE 反作弊驱动与 Wine/Proton 的 win32 内核接口不兼容。WCAS 侧无法修复，缓解方式是换用已验证的 Proton （GE / Experimental / CachyOS）。详见 [README](../README.md#82-终末地启动偶发退出exit-222)。
