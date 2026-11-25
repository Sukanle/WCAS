#!/usr/bin/env bash
# Create a Proton wrapper that keeps the DXVK prefix DLLs but removes Proton's
# global d3d* / dxgi entries from WINEDLLOVERRIDES.
#
# This is the missing piece for the per-exe launcher setup:
#   Launcher.exe / QtWebEngineProcess.exe -> builtin WineD3D (AppDefaults)
#   game exe                             -> default DXVK (prefix DLLs)
#
# Proton normally appends WINEDLLOVERRIDES=d3d11=n,dxgi=n,... for DXVK, and
# environment overrides win over per-exe AppDefaults. The generated wrapper
# filters only those D3D entries when WCAS_PROTON_PER_EXE=1 is set.
#
# Usage:
#   ./scripts/wcas-proton-per-exe-wrapper.sh
#   PROTONPATH="$HOME/.cache/wcas/proton-per-exe" \
#   WCAS_PROTON_PER_EXE=1 ./proton-inject.sh ...
#
# Set ORIG_PROTONPATH if the source Proton is not DW-Proton.
set -euo pipefail

source_proton="${ORIG_PROTONPATH:-$HOME/.local/share/Steam/compatibilitytools.d/DW-Proton Latest}"
wrapper="${WCAS_PROTON_WRAPPER:-$HOME/.cache/wcas/proton-per-exe}"

if [[ ! -x "${source_proton}/proton" ]]; then
    echo "error: source Proton not found: ${source_proton}/proton" >&2
    echo "hint: set ORIG_PROTONPATH to the original Proton/DW-Proton directory" >&2
    exit 2
fi

mkdir -p "${wrapper}"

# Mirror every Proton asset except the main script; files/ and protonfixes/ are
# large, so symlink them instead of copying.
shopt -s dotglob nullglob
for entry in "${source_proton}"/*; do
    name="$(basename "${entry}")"
    case "${name}" in
        proton) continue ;;
    esac
    ln -sfn "${entry}" "${wrapper}/${name}"
done
shopt -u dotglob nullglob

cp "${source_proton}/proton" "${wrapper}/proton"
chmod +x "${wrapper}/proton"

python3 - "${wrapper}/proton" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
marker = '        append_to_env_str(self.env, "WINEDLLOVERRIDES", s, ";")'
needle = "WCAS_PROTON_PER_EXE"
if needle not in text:
    if marker not in text:
        raise SystemExit("error: could not find WINEDLLOVERRIDES marker in Proton script")
    patch = marker + '''
        if os.environ.get("WCAS_PROTON_PER_EXE"):
            # WCAS: keep the DXVK prefix DLLs, but remove Proton's global
            # D3D native overrides so per-exe AppDefaults can select builtin
            # WineD3D for the launcher while the game keeps DXVK.
            _wcas_drop = {"d3d9", "d3d10", "d3d10core", "d3d11", "d3d11core", "d3d12", "d3d12core", "dxgi"}
            _wcas_keep = []
            for _wcas_entry in self.env.get("WINEDLLOVERRIDES", "").split(";"):
                _wcas_name = _wcas_entry.split("=", 1)[0].strip().lower()
                if _wcas_name and _wcas_name not in _wcas_drop:
                    _wcas_keep.append(_wcas_entry)
            self.env["WINEDLLOVERRIDES"] = ";".join(_wcas_keep)'''
    text = text.replace(marker, patch, 1)
    path.write_text(text)
PY

cat <<EOF
Proton per-exe wrapper: ${wrapper}
Source: ${source_proton}

Use it for the launcher run:
  PROTONPATH="${wrapper}" \\
  WCAS_PROTON_PER_EXE=1 \\
  ./proton-inject.sh ... --transport wayland --graphics-api dxvk

The wrapper strips global d3d9/d3d10/d3d10core/d3d11/d3d12/dxgi overrides
only when WCAS_PROTON_PER_EXE=1. Keep the AppDefaults builtin overrides for
Launcher.exe / QtWebEngineProcess.exe / Games.exe.
EOF
