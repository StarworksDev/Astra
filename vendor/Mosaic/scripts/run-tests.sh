#!/usr/bin/env bash
# Run the Mosaic test suite built by scripts/build.sh.
#   scripts/run-tests.sh <Debug|Release|Dist> [--rng-seed N]
# Catch2 runs in random order with the given seed (default 1). Writes
# test-results/junit-<Config>-seed<N>.xml.
set -euo pipefail

config="${1:-}"
case "$config" in
    Debug|Release|Dist) ;;
    *) echo "usage: $0 <Debug|Release|Dist> [--rng-seed N]" >&2; exit 2 ;;
esac
shift
seed=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rng-seed) seed="${2:?--rng-seed needs a value}"; shift 2 ;;
        *) echo "run-tests.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[[ "$seed" =~ ^[0-9]+$ ]] || { echo "run-tests.sh: seed must be a non-negative integer" >&2; exit 2; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

shopt -s nullglob
exes=(bin/"$config"-*/MosaicTests/MosaicTests)
if [[ ${#exes[@]} -ne 1 ]]; then
    echo "run-tests.sh: expected exactly one MosaicTests under bin/$config-*, found ${#exes[@]} (run scripts/build.sh $config)" >&2
    exit 1
fi

mkdir -p test-results
junit="test-results/junit-$config-seed$seed.xml"
echo "== ${exes[0]} --order rand --rng-seed $seed"
"${exes[0]}" --order rand --rng-seed "$seed" --reporter console --reporter "JUnit::out=$junit"
