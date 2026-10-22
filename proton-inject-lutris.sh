#!/usr/bin/env bash
# ============================================================================
# proton-inject.sh — Lutris/Steam 预启动脚本
# ============================================================================
# 将此脚本配置为 Lutris 的"预启动"脚本或 Steam 的"兼容性工具"包装器。
# 它会启动 wcas 测试控制器，后者会接管游戏进程并执行
# 自动化操作（捕获、点击、拖拽、键盘等）。
#
# 使用方法：
#   # 1) Lutris 配置 → 系统选项 → 预启动脚本:
#   #    /path/to/wcas/proton-inject.sh
#   #
#   # 2) 命令行直接使用:
#   #    ./proton-inject.sh /path/to/game.exe --dx11
#   #
#   # 3) 环境变量覆盖:
#   #    NATIVE_TIMEOUT=30 FALLBACK_TIMEOUT=120 ./proton-inject.sh game.exe
#
# 环境变量（全部可选）：
#   NATIVE_TIMEOUT    — Wayland 模式等待超时秒数（默认 30）
#   FALLBACK_TIMEOUT  — XWayland 回退超时秒数（默认 120）
#   BRIDGE_WIDTH      — XWayland 桥宽度（默认 1280）
#   BRIDGE_HEIGHT     — XWayland 桥高度（默认 800）
#   GPU_ACCELERATED   — 设为 "0" 强制软件渲染（默认 "1"）
#   WINEPREFIX        — Wine 前缀路径
#   STEAM_APP_ID      — Steam AppID（自动从 Steam 环境变量读取）
#   LOG_DIR           — 日志输出目录（默认 /tmp/agent-seat-logs）
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}"

# --------------- 日志设置 ---------------
LOG_DIR="${LOG_DIR:-/tmp/WCAS-logs}"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/proton-inject-lutris.log"
TIMESTAMP_FORMAT="+%H:%M:%S"

log() {
    echo "[$(date "${TIMESTAMP_FORMAT}")] $*" | tee -a "${LOG_FILE}" >&2
}

log "=== wcas Proton 注入脚本 ==="
log "脚本目录: ${SCRIPT_DIR}"
log "日志文件: ${LOG_FILE}"
log ""

# --------------- 收集参数 ---------------
# 如果通过 Lutris/Steam 调用，所有参数构成游戏启动命令
log "启动参数: $*"
log ""

# --------------- 环境检查 ---------------
check_environment() {
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

    # 检查 wcas 是否已编译
    if [[ -f "${PROJECT_DIR}/build/Release/proton-inject" ]]; then
        TEST_BINARY="${PROJECT_DIR}/build/Release/proton-inject"
        log "[OK] 找到 Release 测试二进制: ${TEST_BINARY}"
    elif [[ -f "${PROJECT_DIR}/build/Debug/proton-inject" ]]; then
        TEST_BINARY="${PROJECT_DIR}/build/Debug/proton-inject"
        log "[OK] 找到 Debug 测试二进制: ${TEST_BINARY}"
    else
        log "[INFO] 未找到测试二进制，尝试编译..."
        if command -v cmake &>/dev/null && command -v ninja &>/dev/null; then
            cd "${PROJECT_DIR}"
            cmake -B build/Release -DCMAKE_BUILD_TYPE=Release -S . -G "Ninja" -DDRMTAP_HELPER=OFF -DDRMTAP_EGL=AUTO
            cmake --build build/Release --parallel
            TEST_BINARY="${PROJECT_DIR}/build/Release/proton-inject"
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

    # 检查 XWayland（用于回退）
    if command -v Xwayland &>/dev/null; then
        log "[OK] Xwayland 可用"
    else
        log "[WARNING] Xwayland 未安装，回退模式不可用"
    fi

    log ""
}

# --------------- 启动测试 ---------------
# Lutris 已经正确配置了 Proton/Wine 环境，脚本只添加代理所需的最少配置。
# 不要覆盖 Lutris 的环境变量（如 PROTON_USE_WAYLAND、WINEESYNC 等），
# 否则会破坏 Lutris 原本正常的原生 Wayland 连接。
run_test() {
    local program="$1"
    shift

    log "--- 启动 wcas 测试控制器 ---"
    log "测试二进制: ${TEST_BINARY}"
    log ""

    # 构建命令数组（每个元素独立，空格路径安全）
    local -a cmd=(
        "${TEST_BINARY}"
    )

    # 直接传递原始命令，不使用 --umu 重构
    # Lutris 已经配置好了 umu-run 和所有 Proton 环境变量
    cmd+=("--program" "${program}")
    for arg in "$@"; do
        cmd+=("--arg" "${arg}")
    done
    log "模式: 透传 (程序: ${program} $*)"

    # 继承游戏输出（显示 umu-launcher、ProtonFixes 等日志）
    cmd+=("--inherit-output")

    # 超时设置
    local native_timeout="${NATIVE_TIMEOUT:-30}"
    local fallback_timeout="${FALLBACK_TIMEOUT:-120}"
    local content_timeout="${CONTENT_TIMEOUT:-120}"
    local stale_timeout="${STALE_TIMEOUT:-120}"
    log "超时配置: NativeWayland=${native_timeout}s, XWayland=${fallback_timeout}s, 内容=${content_timeout}s, 停滞=${stale_timeout}s"
    cmd+=(
        "--native-timeout" "${native_timeout}"
        "--fallback-timeout" "${fallback_timeout}"
        "--content-timeout" "${content_timeout}"
        "--stale-timeout" "${stale_timeout}"
        "--bridge-width" "${BRIDGE_WIDTH:-1280}"
        "--bridge-height" "${BRIDGE_HEIGHT:-800}"
    )

    # GPU 加速
    if [[ "${GPU_ACCELERATED:-1}" != "0" ]]; then
        cmd+=("--gpu-accelerated")
    fi

    # 图形 API 后端
    # Lutris 默认使用 Vulkan (DXVK)，可通过环境变量 GRAPHICS_API 或脚本参数切换
    local graphics_api="${GRAPHICS_API:-vulkan}"
    cmd+=("--graphics-api" "${graphics_api}")
    log "图形 API: ${graphics_api} $(case "${graphics_api}" in vulkan) echo '(DXVK/VKD3D)';; opengl) echo '(WineD3D)';; esac)"

    # 只传递 Lutris 已设置的关键环境变量
    # 这些变量用于 configure_native_wayland() 检测 Proton/Wine 模式
    # （决定是否保留 DISPLAY 环境变量）
    if [[ -n "${WINEPREFIX:-}" ]]; then
        cmd+=("--env" "WINEPREFIX=${WINEPREFIX}")
    fi

    # PROTON_ENABLE_WAYLAND 是代理模式必需的，确保 Wayland 后端可用
    # 注意：不设置 PROTON_USE_WAYLAND，Lutris 自己管理 Wayland 配置
    cmd+=("--env" "PROTON_ENABLE_WAYLAND=1")

    # 输出目录
    cmd+=("--output-dir" "${LOG_DIR}")

    # 打印命令（用 printf 保证空格路径正确显示）
    log "执行命令:"
    local idx=0
    for piece in "${cmd[@]}"; do
        if [[ "${piece}" == *" "* ]]; then
            printf '  [%d] "%s"\n' "${idx}" "${piece}" | tee -a "${LOG_FILE}"
        else
            printf '  [%d] %s\n' "${idx}" "${piece}" | tee -a "${LOG_FILE}"
        fi
        idx=$((idx + 1))
    done
    log ""

    # 执行测试（捕获退出码但不退出脚本）
    set +e
    "${cmd[@]}" 2>&1 | tee -a "${LOG_FILE}"
    local exit_code="${PIPESTATUS[0]}"
    set -e

    log ""
    if [[ "${exit_code}" -eq 0 ]]; then
        log "[OK] 测试完成，退出码=${exit_code}"
    else
        log "[ERROR] 测试失败，退出码=${exit_code}"
    fi

    return "${exit_code}"
}

# --------------- Wine/Proton 前缀自动探测 ---------------
# 从参数中查找 drive_c 目录并提取其父目录作为 WINEPREFIX。
# 例如：umu-run /home/.../hypergryph/drive_c/.../game.exe
#       → /home/.../hypergryph
# 能正确处理路径中带空格的情况（shell 拆散的参数会被重新拼接）。
detect_wine_prefix() {
    # 将所有参数（除第一个程序名外）用空格连接，在其中查找 drive_c
    local combined=""
    local first=true
    for arg in "$@"; do
        if $first; then
            first=false
            continue
        fi
        if [[ -z "${combined}" ]]; then
            combined="${arg}"
        else
            combined="${combined} ${arg}"
        fi
    done

    # 检查是否包含 drive_c 目录
    if [[ "${combined}" != *"/drive_c"* ]]; then
        log "[INFO] 未在参数中找到 drive_c 目录，跳过 Wine 前缀自动探测"
        return 0
    fi

    # 提取 drive_c 的父目录作为 Wine 前缀
    # /some/path/prefix/drive_c/... → /some/path/prefix
    local prefix="${combined%%/drive_c*}"

    # 验证前缀目录是否存在
    if [[ ! -d "${prefix}" ]]; then
        log "[INFO] 检测到可能的 Wine 前缀 '${prefix}' 但目录不存在，跳过"
        return 0
    fi

    log "[OK] 自动检测到 Wine 前缀: ${prefix}"
    export WINEPREFIX="${prefix}"
}

# --------------- 清理 ---------------
cleanup() {
    log "--- 清理 ---"
    log "日志已保存到: ${LOG_FILE}"
    log "截图文件: ${LOG_DIR}/*.png"
    log "=== 结束 ==="
}

# ============================================================================
# 主流程
# ============================================================================
trap cleanup EXIT

check_environment

# --------------- 脚本参数解析 ---------------
# Lutris 调用方式: proton-inject-lutris.sh [--inherit-output] [--stale-timeout SEC] <原始命令> [参数...]
# 脚本自己的标志必须放在游戏命令之前。
SCRIPT_INHERIT_OUTPUT=false
SCRIPT_STALE_TIMEOUT=""
SCRIPT_GRAPHICS_API=""
GAME_COMMAND=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --inherit-output)
            SCRIPT_INHERIT_OUTPUT=true
            shift
            ;;
        --stale-timeout)
            if [[ -n "${2:-}" ]]; then
                SCRIPT_STALE_TIMEOUT="$2"
                shift 2
            else
                log "[ERROR] --stale-timeout 需要参数"
                exit 1
            fi
            ;;
        --graphics-api)
            if [[ -n "${2:-}" ]]; then
                case "${2}" in
                    vulkan|vk|dxvk) SCRIPT_GRAPHICS_API="vulkan" ;;
                    opengl|gl|wined3d) SCRIPT_GRAPHICS_API="opengl" ;;
                    *)
                        log "[ERROR] 未知图形 API: ${2}，可选值: vulkan (vk/dxvk), opengl (gl/wined3d)"
                        exit 1
                        ;;
                esac
                shift 2
            else
                log "[ERROR] --graphics-api 需要参数"
                exit 1
            fi
            ;;
        --native-timeout)
            if [[ -n "${2:-}" ]]; then
                NATIVE_TIMEOUT="$2"
                shift 2
            else
                shift
            fi
            ;;
        --fallback-timeout)
            if [[ -n "${2:-}" ]]; then
                FALLBACK_TIMEOUT="$2"
                shift 2
            else
                shift
            fi
            ;;
        --content-timeout)
            if [[ -n "${2:-}" ]]; then
                CONTENT_TIMEOUT="$2"
                shift 2
            else
                shift
            fi
            ;;
        --help|-h)
            echo "用法: $0 [脚本选项] <游戏命令> [游戏参数...]"
            echo ""
            echo "脚本选项:"
            echo "  --inherit-output     继承游戏 stdout/stderr（默认已启用）"
            echo "  --stale-timeout SEC  停滞检测超时秒数（默认 120）"
            echo "  --graphics-api API   图形 API 后端: vulkan (默认) 或 opengl"
            echo "  --native-timeout SEC Wayland 超时秒数（默认 30）"
            echo "  --fallback-timeout SEC XWayland 超时秒数（默认 120）"
            echo "  --content-timeout SEC 内容超时秒数（默认 120）"
            echo ""
            echo "Lutris 会自动将原始游戏命令附加在脚本之后，例如："
            echo "  $0 /usr/bin/umu-run /path/to/game.exe"
            exit 0
            ;;
        --)
            shift
            GAME_COMMAND+=("$@")
            break
            ;;
        -*)
            # 未知脚本标志，可能是游戏参数，停止解析
            GAME_COMMAND+=("$@")
            break
            ;;
        *)
            # 第一个非标志参数 = 游戏命令的开始
            GAME_COMMAND+=("$@")
            break
            ;;
    esac
done

if [[ ${#GAME_COMMAND[@]} -eq 0 ]]; then
    log "[ERROR] 未指定游戏命令"
    log ""
    log "用法: $0 [脚本选项] <游戏命令> [游戏参数...]"
    log ""
    log "Lutris 配置: 将此脚本设为'预启动脚本'或'可执行文件覆盖'"
    log "Lutris 会自动将游戏命令附加在脚本之后"
    exit 1
fi

log "游戏命令: ${GAME_COMMAND[*]}"

# 覆盖停滞超时（如果脚本参数指定了）
if [[ -n "${SCRIPT_STALE_TIMEOUT}" ]]; then
    STALE_TIMEOUT="${SCRIPT_STALE_TIMEOUT}"
fi

# 覆盖图形 API（如果脚本参数指定了）
if [[ -n "${SCRIPT_GRAPHICS_API}" ]]; then
    GRAPHICS_API="${SCRIPT_GRAPHICS_API}"
fi

# 自动探测 Wine 前缀（仅当用户未主动设置时）
if [[ -z "${WINEPREFIX:-}" ]]; then
    detect_wine_prefix "${GAME_COMMAND[@]}"
fi

run_test "${GAME_COMMAND[@]}"
