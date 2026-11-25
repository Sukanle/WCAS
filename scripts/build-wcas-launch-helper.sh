#!/usr/bin/env bash
# Build the optional Windows launch helper. The C++ source is stored
# encrypted; this script decrypts it to a temporary directory, cross-builds
# with Zig, then removes the plaintext.
#
# Requires Zig (it supplies the MinGW headers/libraries) and age.
#
# Environment:
#   WCAS_AGE_KEY   age identity used to decrypt the source
#                  (default: <repo>/tmp/age/wcas-age.key)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output="${1:-${root}/build/wcas-launch-helper.exe}"
archive="${root}/tools/wcas-launch-helper.cpp.age"
key="${WCAS_AGE_KEY:-${root}/tmp/age/wcas-age.key}"

if ! command -v zig >/dev/null 2>&1; then
    echo "error: zig is required to cross-build the Windows helper" >&2
    exit 1
fi
if ! command -v age >/dev/null 2>&1; then
    echo "error: age is required to decrypt the helper source" >&2
    exit 1
fi
if [[ ! -f "${archive}" ]]; then
    echo "error: helper source not found: ${archive}" >&2
    exit 1
fi
if [[ ! -f "${key}" ]]; then
    echo "error: age identity not found: ${key} (set WCAS_AGE_KEY to override)" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

age --decrypt --identity "${key}" "${archive}" >"${work}/helper.cpp"

mkdir -p "$(dirname "${output}")"
zig c++ -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows \
    -o "${output}" "${work}/helper.cpp" -lshell32
echo "built ${output}"
