#!/usr/bin/env bash
# Apply/remove per-executable Wine DLL overrides for the QtWebEngine launcher.
#
# Goal:
#   Launcher.exe / QtWebEngineProcess.exe / Games.exe -> WineD3D (builtin)
#   Endfield.exe (or the game exe)                   -> no override, default DXVK
#
# This only works when the launcher profile runs with graphics = "dxvk" (no
# global PROTON_USE_WINED3D=1); otherwise the whole prefix is already WineD3D.
#
# Usage:
#   WINEPREFIX=~/Games/hypergryph ./scripts/wcas-launcher-dll-overrides.sh apply
#   WINEPREFIX=~/Games/hypergryph ./scripts/wcas-launcher-dll-overrides.sh remove
#
# Optional:
#   PROTONPATH=/path/to/DW-Proton ./scripts/...
#   ... apply 'MyLauncher.exe' 'AnotherHelper.exe'
#
# Stop the launcher/game before changing the prefix, then restart it.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: WINEPREFIX=/path/to/prefix [PROTONPATH=/path/to/proton] \
       wcas-launcher-dll-overrides.sh <mode> [exe ...]

Modes:
  apply [exe ...]          force launcher helpers to builtin WineD3D
                           (default exes: Launcher.exe QtWebEngineProcess.exe Games.exe)
  remove [exe ...]         remove the builtin overrides
  apply-native <game.exe>  force the game exe to native DXVK
  remove-native <game.exe> remove the native overrides

The builtin overrides are for the QtWebEngine launcher. The native overrides
are required for the game exe when the Proton wrapper removes Proton's global
d3d11/dxgi=n entries: without them Wine's default system-DLL order would pick
builtin WineD3D for the game too.

Defaults:
  d3d9 d3d10 d3d10core d3d11 dxgi
EOF
}

mode="${1:-}"
case "${mode}" in
    apply|remove|apply-native|remove-native) ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
shift

proton="${PROTONPATH:-$HOME/.local/share/Steam/compatibilitytools.d/DW-Proton Latest}"
prefix="${WINEPREFIX:-}"
wine="${proton}/files/bin/wine"

if [[ -z "${prefix}" ]]; then
    echo "error: WINEPREFIX is not set" >&2
    exit 2
fi
if [[ ! -d "${prefix}" ]]; then
    echo "error: WINEPREFIX does not exist: ${prefix}" >&2
    exit 2
fi
if [[ ! -x "${wine}" ]]; then
    echo "error: Proton wine not found: ${wine}" >&2
    echo "hint: set PROTONPATH to the Proton/DW-Proton installation" >&2
    exit 2
fi

exes=("$@")
case "${mode}" in
    apply|remove)
        if [[ ${#exes[@]} -eq 0 ]]; then
            exes=("Launcher.exe" "QtWebEngineProcess.exe" "Games.exe")
        fi
        value="builtin"
        ;;
    apply-native|remove-native)
        if [[ ${#exes[@]} -eq 0 ]]; then
            echo "error: ${mode} requires at least one game exe (e.g. Endfield.exe)" >&2
            exit 2
        fi
        value="native"
        ;;
esac
dlls=(d3d9 d3d10 d3d10core d3d11 dxgi)

export WINEPREFIX="${prefix}"
export WINEDLLPATH="${proton}/files/lib/wine"
export LD_LIBRARY_PATH="${proton}/files/lib:${proton}/files/lib/wine/x86_64-unix"
export WINEDEBUG=-all

for exe in "${exes[@]}"; do
    key="HKCU\\Software\\Wine\\AppDefaults\\${exe}\\DllOverrides"
    for dll in "${dlls[@]}"; do
        if [[ "${mode}" == "apply" || "${mode}" == "apply-native" ]]; then
            if ! "${wine}" reg add "${key}" /v "${dll}" /d "${value}" /f >/dev/null 2>&1; then
                echo "error: failed to set ${exe}:${dll}=${value}" >&2
                exit 1
            fi
        else
            # Ignore missing values; removing an already-clean key is fine.
            "${wine}" reg delete "${key}" /v "${dll}" /f >/dev/null 2>&1 || true
        fi
    done
    echo "${mode}: ${exe}"
done

case "${mode}" in
    apply)
        cat <<EOF
Applied WineD3D (builtin) overrides for: ${exes[*]}
Use the per-exe Proton wrapper:
  ./scripts/wcas-proton-per-exe-wrapper.sh
  PROTONPATH="$HOME/.cache/wcas/proton-per-exe" WCAS_PROTON_PER_EXE=1 ...
and a launcher profile with transport="wayland", graphics="dxvk".
Also mark the game exe native so it keeps DXVK:
  ./scripts/wcas-launcher-dll-overrides.sh apply-native Endfield.exe
EOF
        ;;
    apply-native)
        cat <<EOF
Applied native DXVK overrides for: ${exes[*]}
Run the launcher through scripts/wcas-proton-per-exe-wrapper.sh so Proton's
global d3d11/dxgi=n entries do not override these per-exe settings.
EOF
        ;;
    remove-native)
        echo "Removed native overrides for: ${exes[*]}"
        ;;
    remove)
        echo "Removed WineD3D (builtin) overrides for: ${exes[*]}"
        ;;
esac
