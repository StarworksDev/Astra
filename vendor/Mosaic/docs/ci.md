# CI

Mosaic follows the shared Starworks CI standard (Arcane, Astra, Manifold2D). This file uses the standard's section order: Overview, Matrix, Entry points, Toolchains, Seeds and test results, Supply chain, Reproducing locally, Deviations.

## Overview

One workflow, `.github/workflows/ci.yml`, triggered on pushes to `main`, on every pull request, and on `workflow_dispatch`. Its jobs:

- `build-test`: the OS x compiler x configuration matrix. Every leg runs the same entry points: build, a standalone-header check (gcc/clang legs), then the tests with two seeds. Each leg uploads `test-results/` as `test-results-<leg>-<Config>`.
- `sanitize`: Linux Clang 19 Debug under ASan+UBSan, and under TSan. The work-scheduler tests drive a real `std::thread` pool.
- `notify`: Discord, never for pull requests.

The workflow sets `permissions: contents: read`, allows one run per ref (a newer push cancels the older run), and gives every job a timeout.

## Matrix

| Leg | Image | Compiler | Configs |
|---|---|---|---|
| `windows-msvc` | `windows-2025` | MSVC from the newest VS on the image (`vs2022`/`vs2026`, picked by `build.ps1`) | Debug, Release |
| `linux-gcc-14` | `ubuntu-24.04` | `g++-14` | Debug, Release |
| `linux-clang-19` | `ubuntu-24.04` | `clang++-19` | Debug, Release |
| `macos-appleclang` | `macos-15` (arm64) | Apple Clang, Xcode 16.4 | Debug, Release |
| `sanitize-asan-ubsan`, `sanitize-tsan` | `ubuntu-24.04` | `clang++-19` + `libclang-rt-19-dev` | Debug |

## Entry points

```
scripts/build.sh  <Debug|Release|Dist> [premake options]   # Linux, macOS (e.g. --sanitize=address|thread)
scripts/build.ps1 <Debug|Release|Dist>                     # Windows
scripts/run-tests.sh  <Config> [--rng-seed N]
scripts/run-tests.ps1 <Config> [--rng-seed N]
ci/check-headers.sh <c++ compiler> <debug|release>          # every public header compiles on its own
```

**Determinism guard.** Both build scripts refuse fast-math and FP contraction in the generated projects, and require the strict model on `Mosaic` and `MosaicTests`.

## Toolchains

- **Windows:** the vendored `vendor/premake5/premake5.exe` (5.0.0-beta8, SHA-256 checked by `build.ps1`), with MSBuild located by vswhere.
- **Linux/macOS:** `scripts/fetch-premake.sh` fetches the same premake version with SHA-256 pins into `.tools/` (gitignored). `CC`/`CXX` select the compiler.
- **macOS:** builds native arm64, without the x86 `-mavx2`.

## Seeds and test results

Each leg runs the suite in random order twice:
- seed `1`, fixed, so it reproduces exactly from run to run;
- `SEED_RUN = run_number % 98999 + 1000`, which moves with the run so order coupling keeps being probed.

Both seeds appear in the banner and in the JUnit file name (`test-results/junit-<Config>-seed<N>.xml`). A failure reproduces with `--rng-seed N`.

## Supply chain

- Every action is pinned by full commit SHA, with the release in a comment.
- Runner images are named, never `*-latest`.
- Checkout does not persist the token.
- premake is pinned and SHA-256 checked on every OS.

## Reproducing locally

```bash
CC=gcc-14 CXX=g++-14 scripts/build.sh Debug && scripts/run-tests.sh Debug --rng-seed 1
ci/check-headers.sh g++-14 debug
CC=clang-19 CXX=clang++-19 scripts/build.sh Debug --sanitize=thread && scripts/run-tests.sh Debug
```

```powershell
scripts\build.ps1 Debug; scripts\run-tests.ps1 Debug --rng-seed 1
```

## Deviations

| Deviation | Reason |
|---|---|
| No `Dist` leg | Dist differs from Release only by `symbols "off"`; nothing in Mosaic is configuration-dependent beyond `NDEBUG`, which Release covers. |
| No determinism-dump comparison | Mosaic has no simulation output to compare. The SIMD determinism contract (`-ffp-contract=off`, `/fp:strict`) is enforced at build time, and exercised end to end by Manifold2D's cross-platform determinism job, which vendors Mosaic. |
