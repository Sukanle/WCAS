#!/usr/bin/env bash
# Build the optional Windows parent-process helper used by the Endfield
# parent-spoof profile. Requires Zig (it supplies the MinGW headers/libraries).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output="${1:-${root}/build/endfield-parent-spoof.exe}"

if ! command -v zig >/dev/null 2>&1; then
    echo "error: zig is required to cross-build the Windows helper" >&2
    exit 1
fi

mkdir -p "$(dirname "${output}")"
zig c++ -target x86_64-windows-gnu -O2 -municode -Wl,--subsystem,windows \
    -o "${output}" "${root}/tools/endfield-parent-spoof.cpp" -lshell32
echo "built ${output}"
