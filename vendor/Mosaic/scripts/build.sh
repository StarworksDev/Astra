#!/usr/bin/env bash
# Build Mosaic + MosaicTests on Linux (macOS once the x86-only flags are
# guarded -- docs/ci.md, Deviations).
#   scripts/build.sh <Debug|Release|Dist> [premake options...]
#     e.g. scripts/build.sh Debug --sanitize=address   (the CI sanitizer lanes)
# Toolchain comes from CC / CXX (default gcc/g++). Generator: premake5 gmake,
# pinned + SHA-256 checked by scripts/fetch-premake.sh.
set -euo pipefail

config="${1:-}"
case "$config" in
    Debug|Release|Dist) ;;
    *) echo "usage: $0 <Debug|Release|Dist> [premake options...]" >&2; exit 2 ;;
esac
shift

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

premake="$("$root/scripts/fetch-premake.sh")"

if [[ "$(uname -s)" == "Darwin" ]]; then
    export CC="${CC:-clang}" CXX="${CXX:-clang++}"
    jobs="$(sysctl -n hw.ncpu)"
else
    export CC="${CC:-gcc}" CXX="${CXX:-g++}"
    jobs="$(nproc)"
fi

# Tell premake which flag dialect to emit (clang rejects some gcc-only flags).
if "$CXX" --version 2>/dev/null | grep -qi clang; then cc_family=clang; else cc_family=gcc; fi

echo "== premake5 $("$premake" --version | awk '{print $NF}') gmake (--cc=$cc_family) $*"
echo "== $("$CXX" --version | head -n1)"
"$premake" --cc="$cc_family" "$@" gmake

# Determinism guard (Mosaic's SIMD/Wide paths): no fast-math or FP contraction
# may reach any generated makefile.
shopt -s nullglob
makefiles=(Makefile ide/*.make)
if grep -nE -- '-ffast-math|-Ofast|-ffp-contract=(fast|on)|-funsafe-math-optimizations' "${makefiles[@]}"; then
    echo "build.sh: fast-math / FP-contraction flag in generated makefiles (banned, see docs/ci.md)" >&2
    exit 1
fi
if ! grep -q -- '-ffp-contract=off' ide/Mosaic.make; then
    echo "build.sh: -ffp-contract=off missing from ide/Mosaic.make" >&2
    exit 1
fi

lower="$(echo "$config" | tr '[:upper:]' '[:lower:]')"
make -j"$jobs" config="$lower"
