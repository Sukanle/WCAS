#!/bin/sh
# CLI smoke test for wcas-test-7-functions. Mirrors the Rust example's argument
# contract: unknown/missing arguments fail, and `--test-dmabuf` runs the probe.
set -eu

BIN="$1"

fail() {
    echo "cli smoke: $1" >&2
    exit 1
}

# Unknown option must fail.
if "$BIN" --definitely-not-an-option >/dev/null 2>&1; then
    fail "unknown option was accepted"
fi

# Missing --program must fail.
if "$BIN" --cli >/dev/null 2>&1; then
    fail "missing --program was accepted"
fi

# Invalid --graphics-api / --capture-backend values must fail.
if "$BIN" --program /bin/true --graphics-api bogus >/dev/null 2>&1; then
    fail "unknown --graphics-api was accepted"
fi
if "$BIN" --program /bin/true --capture-backend bogus >/dev/null 2>&1; then
    fail "unknown --capture-backend was accepted"
fi

# A non-positive timeout must fail.
if "$BIN" --program /bin/true --native-timeout 0 >/dev/null 2>&1; then
    fail "non-positive --native-timeout was accepted"
fi

# --test-dmabuf runs the sync probe and reports without launching an app.
OUT="$("$BIN" --test-dmabuf 0 2>&1)" || fail "--test-dmabuf failed unexpectedly"
echo "$OUT" | grep -q "DMA-BUF 同步能力测试报告" || fail "--test-dmabuf produced no report"

# All previously-rejected options must now be accepted by the parser: this
# invocation reaches launch and fails only because the program does not exist.
if "$BIN" --program /nonexistent-wcas-smoke --native-timeout 1 --fallback-timeout 1 \
        --content-timeout 1 --stale-timeout 1 --capture-backend dmabuf --debug-all \
        --probe-dmabuf --output-dir /tmp/wcas-smoke --bridge-width 640 --bridge-height 480 \
        >/dev/null 2>&1; then
    fail "launch of a nonexistent program unexpectedly succeeded"
fi

echo "cli smoke: ok"
