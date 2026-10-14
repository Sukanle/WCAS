#!/usr/bin/env bash
# ============================================================================
# proton-inject.sh — 独立 Proton/Wine 游戏启动脚本
# ============================================================================
# 通过 umu-run 启动 Windows 游戏，由 wcas 接管进程并执行
# 自动化操作（捕获、点击、拖拽、键盘等）。
#
# 不依赖 Lutris 或 Steam 前缀，可直接在命令行使用。
#
# 使用方法：
#   # 最简用法（.exe 自动启用 umu 模式，自动检测 WINEPREFIX 和 PROTONPATH）
#   ./proton-inject.sh /path/to/game.exe
#
#   # 指定 Wine 前缀和 Proton 路径
#   ./proton-inject.sh game.exe --wine-prefix ~/Games/prefix --proton-path ~/Proton
#
#   # 快速选择 Proton 变体（默认 DW-Proton）
#   ./proton-inject.sh game.exe --proton ge
#
#   # 非 .exe 程序（原生 Linux）
#   ./proton-inject.sh /usr/bin/gnome-calculator
#
#   # 显式禁用 umu 模式
#   ./proton-inject.sh game.exe --no-umu
#
# 选项：
#   --umu                  使用 umu-run 启动（.exe 自动启用）
#   --no-umu               禁用 umu-run
#   --proton VARIANT       Proton 变体: dw (默认), ge, cachyos, office (=Steam 官方), auto
#                          （指定变体未安装时自动回退到 auto）
#   --proton-path PATH     Proton 兼容工具路径（PROTONPATH，优先于 --proton）
#   --game-id ID           umu GAMEID（默认：umu-default）
#   --proton-verb VERB     Proton 启动动词（默认：waitforexitandrun）
#   --wine-prefix PATH     Wine 前缀路径（WINEPREFIX，自动从 drive_c 探测）
#   --env KEY=VALUE        额外环境变量（可重复）
#   --game-arg ARG         传递给游戏的参数（可重复）
#   --cli                  启用命令行交互模式
#   --debug-all            启用全部调试类别
#   --debug-capture        截图/采集调试日志
#   --debug-dmabuf         DMA-BUF 导入/回读诊断
#   --debug-harness        启动/native 等待/XWayland 回退
#   --debug-proxy          连接生命周期与消息转发
#   --debug-input          输入注入
#   --debug-xwayland       XWayland 桥
#   --debug-wire           逐条 Wayland 消息追踪
#   --native-timeout SEC   Wayland 超时秒数（默认 30）
#   --fallback-timeout SEC XWayland 回退超时秒数（默认 120）
#   --content-timeout SEC  内容渲染超时秒数（默认 120）
#   --bridge-width W       XWayland 桥宽度（默认 1280）
#   --bridge-height H      XWayland 桥高度（默认 800）
#   --no-gpu               禁用 GPU 加速
#   --graphics-api API     图形 API 后端: dxvk (默认) | vulkan (原生) | opengl
#   --capture-backend BE   截图后端: dmabuf (默认), client, kms, shm
#   --output-dir DIR       日志输出目录（默认 /tmp/agent-seat-logs）
#   --inherit-output       继承游戏 stdout/stderr（调试用）
#
# 环境变量（全部可选，命令行选项优先）：
#   WINEPREFIX            Wine 前缀路径
#   PROTONPATH            Proton 兼容工具路径
#   PROTON_VARIANT        Proton 变体: dw (默认), ge, cachyos, office (=Steam 官方), auto
#   GAMEID                umu 游戏 ID
#   NATIVE_TIMEOUT        Wayland 超时秒数
#   FALLBACK_TIMEOUT      XWayland 回退超时秒数
#   CONTENT_TIMEOUT       内容渲染超时秒数
#   BRIDGE_WIDTH          XWayland 桥宽度
#   BRIDGE_HEIGHT         XWayland 桥高度
#   GPU_ACCELERATED       设为 "0" 强制软件渲染
#   GRAPHICS_API          图形 API 后端: dxvk (默认) | vulkan (原生) | opengl
#   CAPTURE_BACKEND       截图后端: dmabuf (默认), client, kms, shm
#   LOG_DIR               日志输出目录
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}"

# --------------- 日志设置 ---------------
LOG_DIR="${LOG_DIR:-/tmp/WCAS-logs}"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/proton-inject.log"
TIMESTAMP_FORMAT="+%H:%M:%S"

log() {
    echo "[$(date "${TIMESTAMP_FORMAT}")] $*" | tee -a "${LOG_FILE}" >&2
}

log "=== wcas 独立启动脚本 ==="
log "脚本目录: ${SCRIPT_DIR}"
log "日志文件: ${LOG_FILE}"

# --------------- 参数解析 ---------------
PROGRAM=""
UMU_MODE=""  # "" = auto, "yes", "no"
PROTON_PATH=""
PROTON_VARIANT="${PROTON_VARIANT:-dw}"
GAME_ID=""
PROTON_VERB=""
WINE_PREFIX=""
EXTRA_ENV=()
GAME_ARGS=()
CLI_MODE=false
DEBUG_MODE=0
debug_args=(
    [0]="--debug-capture"    # 位0，值 0x01
    [1]="--debug-dmabuf"     # 位1，值 0x02
    [2]="--debug-rawdump"    # 位2，值 0x04
    [3]="--debug-harness"    # 位3，值 0x08
    [4]="--debug-proxy"      # 位4，值 0x10
    [5]="--debug-input"      # 位5，值 0x20
    [6]="--debug-xwayland"   # 位6，值 0x40
    [7]="--debug-wire"       # 位7，值 0x80
)
NATIVE_TIMEOUT="${NATIVE_TIMEOUT:-30}"
FALLBACK_TIMEOUT="${FALLBACK_TIMEOUT:-120}"
CONTENT_TIMEOUT="${CONTENT_TIMEOUT:-120}"
BRIDGE_WIDTH="${BRIDGE_WIDTH:-1280}"
BRIDGE_HEIGHT="${BRIDGE_HEIGHT:-720}"
GPU_ACCELERATED="${GPU_ACCELERATED:-1}"
GRAPHICS_API="${GRAPHICS_API:-dxvk}"
CAPTURE_BACKEND="${CAPTURE_BACKEND:-dmabuf}"
OUTPUT_DIR="${LOG_DIR}"
INHERIT_OUTPUT=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --umu)
            UMU_MODE="yes"
            shift
            ;;
        --no-umu)
            UMU_MODE="no"
            shift
            ;;
        --proton|-p)
            if [[ -z "${2:-}" ]]; then
                log "[ERROR] --proton 需要参数 (dw|ge|cachyos|office|auto)"
                exit 1
            fi
            case "${2}" in
                dw|ge|cachyos|cachy|office|official|steam|umu|auto) PROTON_VARIANT="${2}" ;;
                *)
                    log "[ERROR] 未知 Proton 变体: ${2}，可选值: dw (默认), ge, cachyos, office, auto"
                    exit 1
                    ;;
            esac
            shift 2
            ;;
        --proton-path)
            PROTON_PATH="$2"
            shift 2
            ;;
        --game-id)
            GAME_ID="$2"
            shift 2
            ;;
        --proton-verb)
            PROTON_VERB="$2"
            shift 2
            ;;
        --wine-prefix)
            WINE_PREFIX="$2"
            shift 2
            ;;
        --env)
            EXTRA_ENV+=("$2")
            shift 2
            ;;
        --game-arg)
            GAME_ARGS+=("$2")
            shift 2
            ;;
        --cli)
            CLI_MODE=true
            shift
            ;;
        --debug-all)
            ((DEBUG_MODE|=0XFF))
            shift
            ;;
        --debug-capture)
            ((DEBUG_MODE|=0X01))
            shift
            ;;
        --debug-dmabuf)
            ((DEBUG_MODE|=0X02))
            shift
            ;;
        --debug-rawdump)
            ((DEBUG_MODE|=0X04))
            shift
            ;;
        --debug-harness)
            ((DEBUG_MODE|=0X08))
            shift
            ;;
        --debug-proxy)
            ((DEBUG_MODE|=0X10))
            shift
            ;;
        --debug-input)
            ((DEBUG_MODE|=0X20))
            shift
            ;;
        --debug-xwayland)
            ((DEBUG_MODE|=0X40))
            shift
            ;;
        --debug-wire)
            ((DEBUG_MODE|=0X80))
            shift
            ;;
        --native-timeout)
            NATIVE_TIMEOUT="$2"
            shift 2
            ;;
        --fallback-timeout)
            FALLBACK_TIMEOUT="$2"
            shift 2
            ;;
        --content-timeout)
            CONTENT_TIMEOUT="$2"
            shift 2
            ;;
        --bridge-width)
            BRIDGE_WIDTH="$2"
            shift 2
            ;;
        --bridge-height)
            BRIDGE_HEIGHT="$2"
            shift 2
            ;;
        --no-gpu)
            GPU_ACCELERATED="0"
            shift
            ;;
        --graphics-api)
            if [[ -n "${2:-}" ]]; then
                case "${2}" in
                    dxvk) GRAPHICS_API="dxvk" ;;
                    vulkan|vk) GRAPHICS_API="vulkan" ;;
                    opengl|gl|wined3d) GRAPHICS_API="opengl" ;;
                    *)
                        log "[ERROR] 未知图形 API: ${2}，可选值: dxvk, vulkan (vk, 原生), opengl (gl/wined3d)"
                        exit 1
                        ;;
                esac
                shift 2
            else
                log "[ERROR] --graphics-api 需要参数"
                exit 1
            fi
            ;;
        --capture-backend)
            if [[ -n "${2:-}" ]]; then
                case "${2}" in
                    client|client-buffer|cache) CAPTURE_BACKEND="client" ;;
                    kms|drm) CAPTURE_BACKEND="kms" ;;
                    shm|wl-shm) CAPTURE_BACKEND="shm" ;;
                    dmabuf|dma-buf) CAPTURE_BACKEND="dmabuf" ;;
                    *)
                        log "[ERROR] 未知截图后端: ${2}，可选值: dmabuf (默认), client, kms, shm"
                        exit 1
                        ;;
                esac
                shift 2
            else
                log "[ERROR] --capture-backend 需要参数"
                exit 1
            fi
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --inherit-output)
            INHERIT_OUTPUT=true
            shift
            ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        -*)
            log "[ERROR] 未知选项: $1"
            log "使用 --help 查看帮助"
            exit 1
            ;;
        *)
            if [[ -z "${PROGRAM}" ]]; then
                PROGRAM="$1"
            else
                log "[ERROR] 多个程序路径: ${PROGRAM} 和 $1"
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "${PROGRAM}" ]]; then
    log "[ERROR] 未指定程序路径"
    log ""
    log "用法: $0 <程序路径> [选项]"
    log "示例: $0 /path/to/game.exe"
    log "      $0 game.exe --wine-prefix ~/Games/prefix --proton-path ~/Proton"
    log ""
    log "使用 --help 查看完整帮助"
    exit 1
fi

log "目标程序: ${PROGRAM}"

# --------------- 自动检测 umu 模式 ---------------
if [[ -z "${UMU_MODE}" ]]; then
    if [[ "${PROGRAM}" == *.exe ]]; then
        UMU_MODE="yes"
        log "[自动] 检测到 .exe 程序，启用 umu 模式"
    else
        UMU_MODE="no"
        log "[自动] 非 .exe 程序，使用普通模式"
    fi
fi

# --------------- 环境检查 ---------------
log ""
log "--- 环境检查 ---"

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
    log "[WARNING] WAYLAND_DISPLAY 未设置，可能不在 Wayland 会话中"
else
    log "[OK] WAYLAND_DISPLAY=${WAYLAND_DISPLAY}"
fi

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
    log "[ERROR] XDG_RUNTIME_DIR 未设置，Wayland 无法工作"
    exit 1
else
    log "[OK] XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR}"
fi

if [[ "${UMU_MODE}" == "yes" ]]; then
    if command -v umu-run &>/dev/null; then
        log "[OK] umu-run: $(command -v umu-run)"
    else
        log "[ERROR] umu-run 未找到。请安装 umu-launcher: https://github.com/Open-Wine-Components/umu-launcher"
        exit 1
    fi
fi

if [[ -f "${PROJECT_DIR}/build/Release/wcas-test-7-functions" ]]; then
    TEST_BINARY="${PROJECT_DIR}/build/Release/wcas-test-7-functions"
    log "[OK] 找到 Release 测试二进制: ${TEST_BINARY}"
elif [[ -f "${PROJECT_DIR}/build/Debug/wcas-test-7-functions" ]]; then
    TEST_BINARY="${PROJECT_DIR}/build/Debug/wcas-test-7-functions"
    log "[OK] 找到 Debug 测试二进制: ${TEST_BINARY}"
else
    log "[INFO] 未找到测试二进制，尝试编译..."
    if command -v cmake &>/dev/null && command -v ninja &>/dev/null; then
        cd "${PROJECT_DIR}"
        cmake -B build/Release -DCMAKE_BUILD_TYPE=Release -S . -G "Ninja" -DDRMTAP_HELPER=OFF -DDRMTAP_EGL=AUTO
        cmake --build build/Release --parallel
        TEST_BINARY="${PROJECT_DIR}/build/Release/wcas-test-7-functions"
        if [[ ! -f "${TEST_BINARY}" ]]; then
            log "[ERROR] 编译失败"
            exit 1
        fi
        log "[OK] 编译成功: ${TEST_BINARY}"
    else
        log "[ERROR] 未找到 cmake 和 ninja，无法编译测试二进制"
        exit 1
    fi
fi

if command -v Xwayland &>/dev/null; then
    log "[OK] Xwayland 可用"
else
    log "[WARNING] Xwayland 未安装，回退模式不可用"
fi

log ""

# --------------- Wine 前缀自动探测 ---------------
detect_wine_prefix() {
    local path="$1"

    if [[ "${path}" != *"/drive_c"* ]]; then
        log "[INFO] 路径中未找到 drive_c 目录，跳过 Wine 前缀自动探测"
        return 1
    fi

    local prefix="${path%%/drive_c*}"

    if [[ ! -d "${prefix}" ]]; then
        log "[INFO] 检测到可能的 Wine 前缀 '${prefix}' 但目录不存在，跳过"
        return 1
    fi

    log "[OK] 自动检测到 Wine 前缀: ${prefix}"
    echo "${prefix}"
    return 0
}

# --------------- Proton 变体解析 ---------------
# 自定义兼容工具位于 compatibilitytools.d，Steam 官方 Proton 位于 steamapps/common。
PROTON_TOOL_DIRS=(
    "${HOME}/.local/share/Steam/compatibilitytools.d"
    "${HOME}/.steam/steam/compatibilitytools.d"
    "${HOME}/.steam/root/compatibilitytools.d"
    "${HOME}/.var/app/com.valvesoftware.Steam/.local/share/Steam/compatibilitytools.d"
)
STEAM_COMMON_DIRS=(
    "${HOME}/.local/share/Steam/steamapps/common"
    "${HOME}/.steam/steam/steamapps/common"
    "${HOME}/.steam/root/steamapps/common"
    "${HOME}/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common"
)

# office 对应 Steam 官方 Proton，因此只搜索 steamapps/common。
proton_variant_search_dirs() {
    case "$1" in
        office|official|steam) printf '%s\n' "${STEAM_COMMON_DIRS[@]}" ;;
        *)                     printf '%s\n' "${PROTON_TOOL_DIRS[@]}" ;;
    esac
}

# 变体别名 -> 目录名匹配模式（按优先级）
proton_variant_patterns() {
    case "$1" in
        dw)            printf '%s\n' 'DW-Proton*' 'dwproton*' ;;
        ge)            printf '%s\n' 'Proton-GE*' 'GE-Proton*' ;;
        cachyos|cachy) printf '%s\n' 'Proton-CachyOS*' 'cachyos*' ;;
        office|official|steam)
                       printf '%s\n' 'Proton - Experimental' 'Proton - Hotfix' 'Proton [0-9]*' 'Proton*' ;;
        umu)           printf '%s\n' 'UMU-Proton*' 'Proton-UMU*' ;;
        *)             return 1 ;;
    esac
}

# 将 --proton 变体解析为 Proton 根目录（内含 proton 可执行文件）
resolve_proton_variant() {
    local variant="$1"
    local pattern dir candidate
    local -a matches
    while read -r pattern; do
        while read -r dir; do
            [[ -d "${dir}" ]] || continue
            matches=()
            for candidate in "${dir}"/${pattern}; do
                [[ -d "${candidate}" && -f "${candidate}/proton" ]] || continue
                matches+=("${candidate}")
            done
            if (( ${#matches[@]} > 0 )); then
                # 多个匹配时取最近修改的
                ls -1dt "${matches[@]}" 2>/dev/null | head -n1
                return 0
            fi
        done < <(proton_variant_search_dirs "${variant}" 2>/dev/null)
    done < <(proton_variant_patterns "${variant}" 2>/dev/null)
    return 1
}

# --------------- Proton 路径自动探测 ---------------
detect_proton_path() {
    local steam_compat="${HOME}/.local/share/Steam/compatibilitytools.d"
    local steam_proton="${HOME}/.local/share/Steam/steamapps/common"

    # 1. Steam 兼容性工具目录（GE-Proton、CachyOS 等）
    if [[ -d "${steam_compat}" ]]; then
        local found
        found="$(ls -1td "${steam_compat}"/*/ 2>/dev/null | head -n1)"
        if [[ -n "${found}" ]]; then
            found="${found%/}"
            if [[ -f "${found}/proton" ]]; then
                log "[OK] 自动检测到 Proton: ${found}"
                echo "${found}"
                return 0
            fi
        fi
    fi

    # 2. Steam 官方 Proton
    if [[ -d "${steam_proton}" ]]; then
        local found
        found="$(ls -1td "${steam_proton}"/Proton*/ 2>/dev/null | head -n1)"
        if [[ -n "${found}" ]]; then
            found="${found%/}"
            if [[ -f "${found}/proton" ]]; then
                log "[OK] 自动检测到 Steam Proton: ${found}"
                echo "${found}"
                return 0
            fi
        fi
    fi

    log "[WARNING] 未找到 Proton 安装，请用 --proton-path 指定"
    return 1
}

# --------------- umu 环境变量配置 ---------------
setup_umu_env() {
    # WINEPREFIX
    if [[ -n "${WINE_PREFIX}" ]]; then
        EXTRA_ENV+=("WINEPREFIX=${WINE_PREFIX}")
    elif [[ -n "${WINEPREFIX:-}" ]]; then
        EXTRA_ENV+=("WINEPREFIX=${WINEPREFIX}")
    else
        local detected
        if detected="$(detect_wine_prefix "${PROGRAM}")"; then
            EXTRA_ENV+=("WINEPREFIX=${detected}")
        else
            log "[WARNING] WINEPREFIX 未设置且无法自动检测，umu-run 可能无法正常工作"
        fi
    fi

    # PROTONPATH：显式路径 > 环境变量 > --proton 变体 > 自动探测
    if [[ -n "${PROTON_PATH}" ]]; then
        EXTRA_ENV+=("PROTONPATH=${PROTON_PATH}")
        log "[OK] 使用指定的 Proton 路径: ${PROTON_PATH}"
    elif [[ -n "${PROTONPATH:-}" ]]; then
        EXTRA_ENV+=("PROTONPATH=${PROTONPATH}")
        log "[OK] 使用环境变量 PROTONPATH: ${PROTONPATH}"
    elif [[ "${PROTON_VARIANT}" != "auto" ]]; then
        local resolved
        if resolved="$(resolve_proton_variant "${PROTON_VARIANT}")"; then
            EXTRA_ENV+=("PROTONPATH=${resolved}")
            log "[OK] Proton 变体 '${PROTON_VARIANT}': ${resolved}"
        else
            log "[WARNING] 未找到 Proton 变体 '${PROTON_VARIANT}'，回退到自动探测"
            local detected
            if detected="$(detect_proton_path)"; then
                EXTRA_ENV+=("PROTONPATH=${detected}")
            fi
        fi
    else
        local detected
        if detected="$(detect_proton_path)"; then
            EXTRA_ENV+=("PROTONPATH=${detected}")
        fi
    fi

    # GAMEID
    if [[ -n "${GAME_ID}" ]]; then
        EXTRA_ENV+=("GAMEID=${GAME_ID}")
    elif [[ -n "${GAMEID:-}" ]]; then
        EXTRA_ENV+=("GAMEID=${GAMEID}")
    fi

    # PROTON_VERB
    if [[ -n "${PROTON_VERB}" ]]; then
        EXTRA_ENV+=("PROTON_VERB=${PROTON_VERB}")
    fi

    # Wine/Proton 性能优化（通用）
    local -A defaults=(
        [WINEARCH]="win64"
        [WINEESYNC]="1"
        [WINEFSYNC]="1"
        [WINE_LARGE_ADDRESS_AWARE]="1"
        [WINEDEBUG]="-all"
        [PROTON_ENABLE_WAYLAND]="1"
    )

    for key in "${!defaults[@]}"; do
        local already_set=false
        for env_str in "${EXTRA_ENV[@]}"; do
            if [[ "${env_str}" == "${key}="* ]]; then
                already_set=true
                break
            fi
        done
        if ! ${already_set}; then
            EXTRA_ENV+=("${key}=${defaults[$key]}")
        fi
    done

    # 图形 API 后端配置
    case "${GRAPHICS_API}" in
        vulkan)
            # DXVK/VKD3D (Vulkan) — 默认高性能后端
            local -A vk_defaults=(
                [DXVK_NVAPIHACK]="0"
                [DXVK_ENABLE_NVAPI]="1"
                [PROTON_DXVK_D3D8]="1"
                [DXVK_LOG_LEVEL]="error"
            )
            for key in "${!vk_defaults[@]}"; do
                local already_set=false
                for env_str in "${EXTRA_ENV[@]}"; do
                    if [[ "${env_str}" == "${key}="* ]]; then
                        already_set=true
                        break
                    fi
                done
                if ! ${already_set}; then
                    EXTRA_ENV+=("${key}=${vk_defaults[$key]}")
                fi
            done
            ;;
        opengl)
            # WineD3D (OpenGL) — 兼容性回退后端
            EXTRA_ENV+=("PROTON_USE_WINED3D=1")
            ;;
    esac
}

# --------------- 构建测试命令 ---------------
build_test_command() {
    local -a cmd=(
        "${TEST_BINARY}"
    )

    if [[ "${UMU_MODE}" == "yes" ]]; then
        # umu 模式：program 传游戏 .exe 路径，--umu 让 Rust 端用 umu-run 包装
        cmd+=("--program" "${PROGRAM}")
        cmd+=("--umu")
    else
        cmd+=("--program" "${PROGRAM}")
    fi

    # 游戏参数
    for arg in "${GAME_ARGS[@]+"${GAME_ARGS[@]}"}"; do
        cmd+=("--arg" "${arg}")
    done

    # 环境变量
    for env_str in "${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"}"; do
        cmd+=("--env" "${env_str}")
    done

    # 超时
    cmd+=(
        "--native-timeout" "${NATIVE_TIMEOUT}"
        "--fallback-timeout" "${FALLBACK_TIMEOUT}"
        "--content-timeout" "${CONTENT_TIMEOUT}"
        "--bridge-width" "${BRIDGE_WIDTH}"
        "--bridge-height" "${BRIDGE_HEIGHT}"
    )

    # GPU 加速
    if [[ "${GPU_ACCELERATED}" != "0" ]]; then
        cmd+=("--gpu-accelerated")
    fi

    # 图形 API 后端
    if [[ "${UMU_MODE}" == "yes" ]]; then
        cmd+=("--graphics-api" "${GRAPHICS_API}")
    fi

    # 截图后端（适用于所有模式）
    cmd+=("--capture-backend" "${CAPTURE_BACKEND}")

    # CLI 模式
    if ${CLI_MODE}; then
        cmd+=("--cli")
    fi

    # Debug 模式
    if [[ "${DEBUG_MODE}" == "all" ]]; then
        cmd+=("--debug-all")
    else
        # 位操作将 Debug 模式转换为参数
        for ((i=0; i<${#debug_args[@]}; i++)); do
            if (( (DEBUG_MODE & (1 << i)) != 0 )); then
                cmd+=("${debug_args[i]}")
                if [[ "${debug_args[i]}" == "--debug-dmabuf" ]]; then
                    cmd+=("--probe-dmabuf")
                fi
            fi
        done
    fi


    # 输出目录
    cmd+=("--output-dir" "${OUTPUT_DIR}")

    # cmd+=("--key-delay" "3")

    # 继承输出
    if ${INHERIT_OUTPUT}; then
        cmd+=("--inherit-output")
    fi

    printf '%s\0' "${cmd[@]}"
}

# --------------- 主流程 ---------------
log "--- 启动配置 ---"

if [[ "${UMU_MODE}" == "yes" ]]; then
    log "模式: umu-run (Proton/Wine)"
    log "Proton 变体: ${PROTON_VARIANT}"
    setup_umu_env
else
    log "模式: 原生 Linux"
fi

log "超时: NativeWayland=${NATIVE_TIMEOUT}s, XWayland=${FALLBACK_TIMEOUT}s, 内容=${CONTENT_TIMEOUT}s"
log "桥尺寸: ${BRIDGE_WIDTH}x${BRIDGE_HEIGHT}"
log "GPU 加速: $(if [[ "${GPU_ACCELERATED}" != "0" ]]; then echo 启用; else echo 禁用; fi)"
log "图形 API: ${GRAPHICS_API} $(case "${GRAPHICS_API}" in dxvk) echo '(DXVK/VKD3D)';; vulkan) echo '(游戏原生 Vulkan)';; opengl) echo '(WineD3D)';; esac)"
log "截图后端: ${CAPTURE_BACKEND} $(case "${CAPTURE_BACKEND}" in client) echo '(客户端缓冲区缓存)';; kms) echo '(DRM/KMS 内核帧缓冲区)';; shm) echo '(wl_shm 共享内存)';; dmabuf) echo '(DMA-BUF GPU 缓冲区)';; esac)"
# if ${DEBUG_MODE}; then log "调试模式: 启用 (${DEBUG_MODE})"; fi
if [[ "${DEBUG_MODE}" == "all" ]]; then
    log "调试模式: 启用 (all)"
elif [[ "${DEBUG_MODE}" != "0" ]]; then
    for ((i=0; i<${#debug_args[@]}; i++)); do
        if (( (DEBUG_MODE & (1 << i)) != 0 )); then
            log "调试模式: 启用 (${debug_args[i]})"
        fi
    done
else
    log "调试模式: 禁用"
fi

# 构建命令
mapfile -d '' CMD_ARRAY < <(build_test_command)

log "--- 启动 wcas ---"
log "测试二进制: ${TEST_BINARY}"
log ""
log "执行命令:"
local_idx=0
for piece in "${CMD_ARRAY[@]}"; do
    if [[ "${piece}" == *" "* || "${piece}" == *"	"* ]]; then
        printf '  [%d] "%s"\n' "${local_idx}" "${piece}" | tee -a "${LOG_FILE}"
    else
        printf '  [%d] %s\n' "${local_idx}" "${piece}" | tee -a "${LOG_FILE}"
    fi
    local_idx=$((local_idx + 1))
done
log ""

# 设置 Wayland 支持
export PROTON_ENABLE_WAYLAND=1

# 执行测试
set +e
"${CMD_ARRAY[@]}" 2>&1 | tee -a "${LOG_FILE}"
EXIT_CODE="${PIPESTATUS[0]}"
set -e

log ""
if [[ "${EXIT_CODE}" -eq 0 ]]; then
    log "[OK] 测试完成，退出码=${EXIT_CODE}"
else
    log "[ERROR] 测试失败，退出码=${EXIT_CODE}"
fi

# --------------- 清理 ---------------
log "--- 清理 ---"
log "日志已保存到: ${LOG_FILE}"
log "截图文件: ${OUTPUT_DIR}/*.png"
log "=== 结束 ==="

exit "${EXIT_CODE}"
