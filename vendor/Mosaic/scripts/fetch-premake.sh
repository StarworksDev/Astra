#!/usr/bin/env bash
# Fetch the pinned premake5 for Linux / macOS into .tools/premake5/ and verify
# its SHA-256. Windows uses the vendored vendor/premake5/premake5.exe, which is
# byte-identical to the upstream v5.0.0-beta8 windows zip (same pin).
# Idempotent: a verified binary already in place is reused.
# Prints the path to the premake5 binary on stdout.
set -euo pipefail

PREMAKE_VERSION="5.0.0-beta8"
SHA256_LINUX="63edd3e7461eebdd45b500a3c7e8ad4e7a67d68f230010f9a97cbb71b4ec59c8"  # premake-5.0.0-beta8-linux.tar.gz  (x86_64)
SHA256_MACOSX="fa73a46f093fa6f17494a3d063421aa6cae3ea825a61c62dd59fc2f07a256d03" # premake-5.0.0-beta8-macosx.tar.gz (arm64)

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/.tools/premake5/$PREMAKE_VERSION"
bin="$dest/premake5"

case "$(uname -s)" in
    Linux)  asset="premake-$PREMAKE_VERSION-linux.tar.gz";  sha="$SHA256_LINUX" ;;
    Darwin) asset="premake-$PREMAKE_VERSION-macosx.tar.gz"; sha="$SHA256_MACOSX" ;;
    *) echo "fetch-premake.sh: unsupported OS $(uname -s) (Windows uses the vendored premake5.exe)" >&2; exit 1 ;;
esac

if [[ -x "$bin" && -f "$dest/.sha256" && "$(cat "$dest/.sha256")" == "$sha" ]]; then
    echo "$bin"
    exit 0
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

rm -rf "$dest"
mkdir -p "$dest"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
url="https://github.com/premake/premake-core/releases/download/v$PREMAKE_VERSION/$asset"
curl -fsSL --retry 4 --retry-delay 2 -o "$tmp/$asset" "$url"
actual="$(sha256_of "$tmp/$asset")"
if [[ "$actual" != "$sha" ]]; then
    echo "fetch-premake.sh: SHA-256 mismatch for $asset" >&2
    echo "  expected $sha" >&2
    echo "  actual   $actual" >&2
    exit 1
fi
tar -xzf "$tmp/$asset" -C "$dest" premake5
chmod +x "$bin"
echo "$sha" > "$dest/.sha256"
echo "$bin"
