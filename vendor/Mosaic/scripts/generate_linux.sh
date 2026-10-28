#!/usr/bin/env bash
# Generate the gmake makefiles only (scripts/build.sh generates AND builds).
# Uses the pinned premake from scripts/fetch-premake.sh.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
"$("$root/scripts/fetch-premake.sh")" gmake "$@"
