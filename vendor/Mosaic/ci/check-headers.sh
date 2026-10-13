#!/usr/bin/env bash
# Compiles every public Mosaic header on its own (-fsyntax-only), so a header
# that only builds because something else happened to include its
# dependencies first fails here instead of in a consumer (Astra, Manifold2D,
# Arcane). Same check as Astra's ci/check-headers.sh.
#
#   ci/check-headers.sh <c++ compiler> <debug|release>
#
# Flags mirror the Mosaic project's gcc/clang settings (premake5.lua).
set -euo pipefail

CXX="${1:?usage: ci/check-headers.sh <c++ compiler> <debug|release>}"
CONFIG="${2:?usage: ci/check-headers.sh <c++ compiler> <debug|release>}"

case "$CONFIG" in
    debug)   DEFINES="-DMOSAIC_DEBUG" ;;
    release) DEFINES="-DMOSAIC_RELEASE -DNDEBUG" ;;
    *) echo "unknown config: $CONFIG" >&2; exit 2 ;;
esac

# -mavx2 mirrors premake5.lua on x86-64; arm64 has no AVX and takes the
# portable/NEON paths.
case "$(uname -m)" in
    x86_64|amd64) ARCHFLAGS="-mavx2" ;;
    *)            ARCHFLAGS="" ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/include"

check_one() {
    local header="$1"
    if ! output="$(printf '#include <%s>\n' "$header" | "$CXX" -std=c++23 -fsyntax-only $ARCHFLAGS \
            -ffp-contract=off -fno-fast-math -Wall -Wextra -Wpedantic -Werror \
            -Wno-unknown-warning-option \
            $DEFINES -I. -x c++ - 2>&1)"; then
        printf 'FAIL %s\n%s\n' "$header" "$output"
        return 1
    fi
}
export -f check_one
export CXX DEFINES ROOT ARCHFLAGS

headers=$(find Mosaic -name '*.hpp' | sort)
count=$(printf '%s\n' "$headers" | wc -l)
if printf '%s\n' "$headers" | xargs -P "$(getconf _NPROCESSORS_ONLN)" -I{} bash -c 'check_one "$@"' _ {}; then
    echo "check-headers: all $count headers compile standalone ($CXX, $CONFIG)"
else
    echo "check-headers: standalone header failures ($CXX, $CONFIG)" >&2
    exit 1
fi
