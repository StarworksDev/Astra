# Astra vs EnTT vs flecs — same-machine head-to-head (2026-07-21, updated 2026-07-23 post-Phase-C)

**Setup:** one machine, identical hand-rolled harness (`bench_common.hpp`), identical component layout, 1,000,000 entities, MSVC `/std:c++20 /O2 /DNDEBUG /EHsc`, view/query/group created once (pure-iteration), median of runs. EnTT master, flecs v4.1.6, Astra @ `perf/phase-c-chunk-storage` (post-W5+W7+W4+W3 chunk storage modernization — see the Phase C section below). Each library uses its idiomatic fast path. Numbers are **ns per entity/op** (lower = better); M/s in parens.

| Operation | Astra | EnTT | flecs | Rank |
|---|---|---|---|---|
| iterate 1 comp | 0.451 (2218) | 0.512 (1955) | **0.389 (2574)** | flecs > **Astra** > EnTT |
| iterate 2 comp | 1.069 (936) | 2.303 view / 1.046 group | **0.773 (1294)** | flecs > EnTT-group ≈ **Astra** > EnTT-view |
| iterate 3 comp | 1.176 (850) | 3.249 view (308) | **0.980 (1020)** | flecs > **Astra** > EnTT-view |
| create (2 comp) | **49.2 (20.3)** | 38.2 (26.2) | 94.6 (10.6) | EnTT > **Astra** > flecs |
| add component | 55.6 (18.0) | **11.3 (88.2)** | 53.6 (18.6) | EnTT > **Astra** ≈ flecs |
| remove component | 40.8 (24.5) | **16.0 (62.4)** | 32.8 (30.5) | EnTT > flecs > **Astra** |
| random get | 58.4 (17.1) | **19.0 (52.6)** | 54.2 (18.5) | EnTT > flecs > **Astra** |

Astra numbers are the median of 8 interleaved same-session runs against a pre-Phase-C worktree build (see
"Phase C" below for the full A/B methodology). EnTT/flecs are the median of 4 fresh same-session runs
(unmodified code — re-measured, not reused from 2026-07-22, to keep the table internally consistent; this
machine's absolute numbers drift a few percent session-to-session from background load, same caveat as the
W6+W2 session). **create and add both changed rank this update** — Astra now beats flecs on create and is
within measurement noise of flecs on add (see "Residual gap to flecs" under Phase C for the honest
before/after and the caveat that create/add rank swaps rest on cross-library comparisons across sessions,
not the tightly-controlled pre/post-C A/B, which only compares Astra against itself).

## W1 (unified paged entity record) — landed 2026-07-22

W1 replaced `ArchetypeManager`'s `std::unordered_map<Entity,EntityRecord>` with a paged, id-indexed
`EntityRecord{archetype, location, version}` table shared with the liveness-version storage (`Entity/EntityTable.hpp`).
Validate (version check) and locate (archetype+location) now read the **same cache line** with **no hashing**
and **no per-create heap allocation**. Public `Registry` API is unchanged.

| Operation | Before (d4f8042) | After (W1) | Delta |
|---|---|---|---|
| create (2 comp) | 366.8 ns | 129.0 ns | **2.84× faster** |
| add component | 278.6 ns | 150.5 ns | **1.85× faster** |
| remove component | 220.4 ns | 105.6 ns | **2.09× faster** |
| random get | 147.0 ns | 62.2 ns | **2.36× faster** (beat the ~80 ns estimate) |
| iterate 1/2/3 comp | 0.456 / 1.538 / 1.457 ns | 0.436 / 1.173 / 1.142 ns | flat, within run-to-run noise (record table is off the iteration hot path, as expected — no regression) |

`GetComponent`/`HasComponent` were verified to route through the new `m_records->GetRecord(id)` path
(`ArchetypeManager.hpp:512-529`) — both the version check and the location read come off the single fetched
`EntityRecord`, confirming the win is real (not a leftover/mixed path).

## W6+W2 (avalanche hash mix + array-indexed archetype edges) — landed 2026-07-22

W6 replaced `SplitHash`'s raw `std::hash` forwarding with an `fmix64`-style avalanche mix before splitting
H1/H2, so pointer-keyed (and other low-entropy) `FlatMap` keys no longer degenerate the SIMD H2 filter
(H2 was constant for pointer keys pre-mix). W2 replaced the two-level `FlatMap<Archetype*, FlatMap<ComponentID,
Archetype*>>` in `ArchetypeGraph` with a lazily-allocated, per-archetype `Archetype* addEdges[MAX_COMPONENTS]`
/ `removeEdges[MAX_COMPONENTS]` array embedded directly on `Archetype` (`Archetype.hpp:1061-1087`); warm add/remove
is now a direct array index (`from->GetAddEdge(id)` / `from->GetRemoveEdge(id)`), not a hashed double-probe.
`ArchetypeGraph.hpp` was deleted. Verified `GetArchetypeWithAdded`/`GetArchetypeWithRemoved`
(`ArchetypeManager.hpp:1092-1106`) route through `GetAddEdge`/`SetAddEdge`/`GetRemoveEdge`/`SetRemoveEdge` —
not a leftover path.

### Measurement methodology

The machine was materially noisier during this benchmarking session than the 2026-07-21/W1 session — a
sanity check running the **unmodified** `bench_entt`/`bench_flecs` binaries showed their numbers drifting
20-100% from the recorded baseline run-to-run (e.g. EnTT random_get swung 18.6 → 36-37 ns on early runs before
settling back to ~22 ns), which is impossible if their code hadn't changed. So absolute ns numbers are **not**
directly comparable run-to-run against the 2026-07-21 baseline on this session alone.

To get an honest, noise-controlled delta, a second `bench_astra.exe` was built from the pre-W6+W2 commit
(`13ee47b`, the last commit before W6/W2) using the identical harness and flags, and the two binaries were
run **interleaved, back-to-back**, 10 rounds total. The first 4 rounds landed during a noisy window (machine
settling); the last 6 rounds were internally consistent and closely reproduced the original W1-session
absolute numbers (Astra-old create/add/remove/random_get ≈ 130/152/106/65 ns vs the recorded W1 baseline of
129/150.5/105.6/62.2 ns), confirming those 6 are a valid apples-to-apples comparison. The table below uses
the median of that settled 6-round window for both binaries.

| Operation | Before (pre-W2, `13ee47b`, this session) | After (W6+W2) | Delta |
|---|---|---|---|
| add component | 152.5 ns | 131.5 ns | **13.7% faster** |
| remove component | 106.0 ns | 86.4 ns | **18.4% faster** |
| create (2 comp) | 130.0 ns | 133.2 ns | flat (±2%, within noise — create doesn't hit the edge cache on the hot path, as expected) |
| random get | 65.3 ns | 63.8 ns | flat (±2%, within noise — unaffected, as expected) |
| iterate 1/2/3 comp | 0.510 / 1.147 / 1.280 ns | 0.432 / 1.222 / 1.192 ns | flat within run-to-run noise, no regression |

The add/remove win is real and consistent, not a fluke: across all 6 settled rounds, every single "after" add
sample (130-136 ns) was below every single "before" sample (150-155 ns), and likewise remove (85-88 ns after
vs 105-107 ns before) — the two distributions don't overlap.

### Honest sanity-check against the plan's prediction

The perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`, Phase B) originally estimated add
279→~90 ns and remove 220→~60 ns against the **pre-W1** baseline that was current when that line was written.
Rebased onto the actual pre-W2 (i.e. post-W1) starting point — the baseline this session's A/B actually
measured from — the predicted target was **add ~150→~100-110 ns and remove ~106→~70 ns** (i.e. ≈27-34%
reductions: add ~27-33%, remove ~34%), still on the theory that the edge lookup was the dominant remaining
cost. The measured reduction is smaller than that rebased prediction — **13.7%/18.4%**, below the ≈27-34%
range. The wiring was independently verified correct in source (above), so this is not a leftover-path bug;
it means the archetype-edge `FlatMap` double-probe was a real but smaller share of add/remove's cost than
the plan estimated. The remaining cost is dominated by the transition **move** itself (per-element
`MoveConstruct`/`Destruct` via function pointer over the chunk's present components, `Archetype.hpp:1433` /
`ArchetypeChunkPool.hpp:365-396`) — unchanged by W2, and exactly what **W3** (trivial memcpy move) targets next.

Residual gap to flecs, post-W6+W2: add 131.5 vs flecs 54.6 (2.41×, down from 2.79× pre-W2: 152.5/54.6, this
session's own fresh pre-W2/flecs medians), remove 86.4 vs flecs 32.7 (2.64×, down from 3.24× pre-W2:
106.0/32.7). Both gaps shrank but flecs is still meaningfully ahead — consistent with the move-cost diagnosis
above (W3/W4/W5, Phase C, is where that residual gap should close further).

## Phase C (chunk storage modernization: W5+W7+W4+W3) — landed 2026-07-23

Phase C bundled four work items sharing one layout change:
- **W5 — metadata once per archetype.** `ArchetypeColumnMeta` now holds a shared `const ComponentDescriptor*`
  per column instead of a 176 B by-value copy, and the chunk's present-component storage is a packed
  `columns[0..columnCount)` array (ascending id) instead of a `MAX_COMPONENTS`(128)-slot absolute-id-indexed
  array — killing the ~23 KB/chunk metadata bloat and letting every hot loop iterate `0..N` instead of `0..128`.
- **W7 — compact `idToColumn` get.** Record→component resolution now indexes a small per-archetype
  `idToColumn[id]` array instead of walking the old by-value `ComponentArrayInfo` array; this necessarily
  rides on W5's layout (the old 128-slot direct array is gone).
- **W4 — fast-append.** Confirmed a **genuine no-op**: git archaeology (commit `3b77b30`) shows
  `AddEntityWithComponents` already avoided double-constructing caller-supplied components *before* Phase C
  touched anything (it predates the W5 metadata scaffold). The commit has **zero production-code diff** — it
  only adds a regression-guard test (`FastAppendPreservesAllProvidedValues`) pinning the property. `AddEntity`
  (no-value-provided path) is deliberately left doing a full `DefaultConstruct` per column, since chunk slots
  reused after swap-and-pop hold stale bytes, not zeros — skipping it would leak stale data.
- **W3 — trivial memcpy transition move**, gated on `is_trivially_copyable` as a **correctness gate, not an
  optimization** (a blanket memcpy would skip a move-only/lifetime-counting type's move ctor and corrupt it).
  Verified in source on **both** transition directions: the remove path `MoveEntityFrom`
  (`Archetype.hpp:439,462,482`) and the add path `MoveAndAdd`/`MoveAndAddByID`
  (`ArchetypeManager.hpp:1178,1208-1213,1433,1491-1496`) — both merge-join over the two archetypes' packed
  `idToColumn`-indexed column lists (no per-id `isValid` scan) and `memcpy` any trivially-copyable column,
  falling back to `desc.MoveConstruct` otherwise.

### Measurement methodology

Following the W6+W2 session's finding that this machine can be noisy, a second `bench_astra.exe` was built
from the pre-Phase-C commit (`a8712d6`, the last commit before the W5 metadata scaffold) in a temporary git
worktree (`git worktree add <path> a8712d6`). The bench harness files (`bench_astra.cpp`, `bench_common.hpp`,
`build_one.bat`) aren't tracked by git, so they were copied by hand into the worktree's `bench-compare/`
(with `build_one.bat`'s `cd` path adjusted to the worktree) and built with the identical flags. The two
binaries were then run **interleaved, back-to-back, 8 rounds total** (pre-C, post-C, pre-C, post-C, ...).
Unlike the W6+W2 session, all 8 rounds were internally consistent — no noisy leading window needed to be
discarded. The worktree was removed after data collection (`git worktree remove`).

| Operation | Before (pre-C, `a8712d6`, this session, n=8) | After (Phase C, n=8) | Delta |
|---|---|---|---|
| create (2 comp) | 129.4 ns | 49.2 ns | **62.0% faster (2.63×)** |
| add component | 131.2 ns | 55.6 ns | **57.6% faster (2.36×)** |
| remove component | 85.6 ns | 40.8 ns | **52.4% faster (2.10×)** |
| random get | 58.8 ns | 58.4 ns | flat (±1%, within noise — W7's headroom was small post-W1, as expected) |
| iterate 1/2/3 comp | 0.435 / 1.074 / 1.188 ns | 0.451 / 1.069 / 1.176 ns | flat within run-to-run noise, no regression |

The win is real and consistent, not a fluke: across all 8 paired rounds, every post-C sample was strictly
below every pre-C sample for all three structural ops — create pre-C range [128.6, 130.6] vs post-C
[48.5, 50.6]; add pre-C [128.7, 134.5] vs post-C [54.9, 57.8]; remove pre-C [85.0, 90.5] vs post-C
[39.4, 42.5] — non-overlapping distributions.

### Honest attribution — the create win is NOT from W4

W4 was independently confirmed a no-op (no source diff — see above), exactly as this task's brief predicted.
Yet create moved 129.4 → 49.2 ns (2.63×), a bigger win than add or remove got. The actual mechanism is **W5**:
pre-Phase-C, `AddEntity`/`AddEntityWithComponents` scanned the full `0..MAX_COMPONENTS` (128) slot range with
an `isValid`-gated `DefaultConstruct` per slot (root causes R4/R5 in the plan); post-W5, those same functions
iterate the packed `m_meta->columnCount` present-column array (`ArchetypeChunkPool.hpp:137,144,190,199`) — for
the benchmark's 2-component archetype, that's a 2-iteration loop, not a 128-iteration scan with 126 wasted
`isValid` checks. The plan's fix-matrix rated W5's create impact as "●" (merely contributory) and gave W4 the
dominant "●●●" — in practice, since W4's own construct-avoidance logic had already shipped before Phase C,
killing the 0..128 scan (which is what W5 actually did) turned out to be the dominant create lever. The win
is genuine and verified in source; it was just attributed to the wrong work item in the original matrix.

### Add/remove: bigger than predicted, too

The brief's directional guidance suggested add moving toward ~90-100 ns and remove toward ~55-65 ns from the
pre-C baseline (~131/86). Measured: add 55.6 ns, remove 40.8 ns — both **better than the optimistic end of
that range**. W3's memcpy gate (verified correct and present on both transition directions — see above)
compounds with W5: the merge-join over present columns now walks a packed `0..N` array of shared descriptor
pointers (cache-friendly) instead of scanning 128 by-value 176 B `ComponentDescriptor`s, so both the
"which columns move" bookkeeping and the per-column move itself got cheaper together.

### Residual gap to flecs, post-Phase-C

- **create — Astra is now AHEAD of flecs:** 49.2 vs 94.6 ns (Astra **1.92× faster**). This is a genuine
  result: flecs's own numbers are stable and its code is unmodified, and Astra's A/B against its own pre-C
  build is a clean, non-overlapping 2.63× win. The plan's original 3.5× flecs-behind gap on create is now
  inverted.
- **add — near parity:** 55.6 vs 53.6 ns (1.04×). This comparison is cross-library and cross-session (not
  the tightly-controlled pre/post-C worktree A/B), so treat "near parity" rather than a precise ranking as
  the honest takeaway; the plan's Phase-B residual of 2.41× is closed to essentially zero either way.
- **remove — gap much smaller, still real:** 40.8 vs 32.8 ns (1.24×, down from 2.64× post-W6+W2 and 6.4×
  pre-W1).
- **random_get — unchanged, near parity:** 58.4 vs 54.2 ns (1.08×), as expected — get was never a Phase C
  target and W7's remaining headroom post-W1 was small.
- **iteration — flecs still ahead, unchanged:** iterate1 1.16×, iterate2 1.38×, iterate3 1.20× flecs-ahead.
  W5's cache-density argument (per-chunk metadata shrink) was expected to possibly help iteration, but this
  session's data does not show a measurable win there — reported honestly as flat/unchanged rather than
  reading a win into session-level noise. Chunk-size/prefetch tuning remains open for a future pass.

## Honest verdict (updated post-W1)

- My original "Astra **can't out-perform** EnTT/flecs" was **wrong**: Astra beats EnTT's common `view` idiom on 2- and 3-component iteration (1.9×–2.8×) and beats EnTT on single-component.
- My mid-stream "Astra **dominates** iteration" (from the EnTT-only slice) was **also wrong**: **flecs — the direct archetype competitor — is still faster than Astra on all three iteration cases** (iterate2: flecs 0.82 vs Astra 1.17 = **1.4× faster**, narrower than pre-W1's 1.7× but iteration wasn't W1's target — this delta is noise, not a real gain).
- **W1 closed a large chunk of the structural-churn gap.** Pre-W1 Astra was a distant #3 on create/add/remove/get, 2.3×–6.4× behind flecs. Post-W1: create is now 1.4× behind flecs (was 3.5×), remove 3.3× behind (was 6.4×), random_get 1.2× behind (was 2.3×) — random_get in particular is now nearly at flecs parity. Add is still the biggest remaining gap (150.5 vs 53.5 ns, 2.8× — W1 wasn't targeted at add's dominant cost, the edge-lookup double-hash in `ArchetypeGraph`; that's W2's job).

**The real picture:** Astra is still #2 on iteration (ahead of EnTT-view, behind flecs, unchanged by W1 as expected) and now a much closer #3 on structural churn — no longer "distant." EnTT's sparse-set numbers remain out of reach for an archetype model (see plan §4, do-not-chase), but flecs parity (the actual target) is now within ~1.2×–3.3× depending on the op, down from ~2.3×–6.4×.

## Why this is good news, not bad

flecs uses the **same storage model** as Astra. Pre-W1 it was 1.7× faster on iteration and 3.5×–6.4× faster on structural ops — meaning Astra's deficits were **optimization gaps, not architectural limits**. W1 (the first of those fixes) already halved-to-thirded most of the structural gap, confirming the diagnosis. Remaining causes, per the perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`):
- **Iteration gap vs flecs:** W6 (hash mixing) landed but iteration was never W6's target and shows no change, as predicted; the remaining cause is per-chunk 23 KB `ComponentDescriptor`-by-value metadata bloat (cache pressure — W5) and possibly chunk-size/prefetch tuning — both Phase C.
- **Remaining structural gap (add especially):** W2 replaced the two pointer-keyed `FlatMap` probes with array-indexed edges and measurably helped (add 2.79×→2.41× behind flecs, remove 3.24×→2.64×), but the transition **move** cost (per-element ctor/dtor via fn-ptr, not memcpy) now dominates what's left — that's **W3**'s target.
- **random_get** is close to flecs (63.8 vs 57.4 ns, 1.11×) — W1 alone nearly closed this gap, exactly as the plan predicted (W1 "dominates random_get"); W2 doesn't touch this path, as expected (flat).

**Takeaway for the roadmap (as of the W6+W2 landing):** W1 validated the "optimization gaps, not architectural limits" diagnosis — a single surgical change (paged record, no API change) closed most of random_get and a meaningful slice of create/add/remove. **W6+W2 landed 2026-07-22** (avalanche hash mix + array-indexed archetype edges): a real but smaller-than-predicted add/remove win (13.7%/18.4%, below the rebased ≈27-34% prediction — see the W6+W2 section above), confirming the edge lookup was a genuine but not dominant cost. Next up per the plan's execution order: **Phase C** (chunk storage modernization: W5 ⇒ W4 ⇒ W3 ⇒ W7), beginning with W5 — the shared metadata-layout change W4/W3/W7 build on; W3 (trivial memcpy move) is the clearest lever on add/remove once that groundwork lands.

## Honest verdict (updated post-Phase C, 2026-07-23)

**Phase C landed 2026-07-22/23** (W5 metadata-once + packed columns, W7 compact `idToColumn` get, W4
confirmed no-op, W3 memcpy move on both transition directions) and **exceeded the plan's Phase C
expectations on every structural op**, not just met them:

- **create: 129.4 → 49.2 ns (2.63×), Astra now beats flecs (94.6 ns) outright** — the single biggest
  surprise of this round. The plan attributed create's dominant lever to W4; W4 turned out to be a no-op,
  and the real driver was W5 killing the `0..128` scan in `AddEntity`/`AddEntityWithComponents` (see "Honest
  attribution" above). The plan's original 3.5×-behind-flecs create gap is now inverted to Astra being ahead.
- **add: 131.2 → 55.6 ns (2.36×), essentially at flecs parity (53.6 ns, 1.04×)** — beyond the brief's
  optimistic ~90-100 ns guidance, and closing what had been the single biggest remaining structural gap
  (2.8× behind flecs pre-W2, 2.41× post-W6+W2) to near-zero.
- **remove: 85.6 → 40.8 ns (2.10×)**, gap to flecs now 1.24× (down from 6.4× pre-W1, 2.64× post-W6+W2) —
  smaller than add's closure but still a large, real win.
- **random_get and iteration are honestly flat**, as predicted (W7's headroom was small post-W1; iteration
  was never Phase C's primary target and this session's data shows no measurable movement there, reported
  as such rather than reading a win into noise).

**Updated overall picture:** what started as "Astra is a distant #3 behind flecs on every structural op"
(pre-W1: 2.3×-6.4× behind) is now "Astra leads flecs on create, ties it on add, and trails by only ~1.1×-1.2×
on remove/random_get" — with iteration (flecs's own strength, ~1.2×-1.4× ahead of Astra) the only place flecs
still holds a clear edge. This confirms the plan's central diagnosis end-to-end: the gaps were optimization
gaps in Astra's implementation of the same archetype model flecs uses, not model-inherent limits — three
surgical phases (W1, then W6+W2, then W5+W7+W4+W3) closed nearly all of them.

**Next per the plan's execution order (§3/§7): Phase D** — wiring `CommandBuffer::Execute` through the
existing `GroupEntitiesByArchetype`/batch-move machinery, and per-chunk change versions for change detection.
Both are **roadmap features, not benchmark movers** (the immediate single-entity API this benchmark exercises
is already close to fully optimized); Phase D's value is real-world batched-workload throughput and the
change-detection feature itself, not this microbenchmark's numbers.

## Phase 2 — dynamic chunk sizing (2026-07-23, branch `perf/phase-2-dynamic-chunk-sizing` @ `ae40c9b`)

Phase 2 replaced the fixed 16KB chunk with a **TLSF (Two-Level Segregated Fit) allocator** +
**grow-as-populate sizing** (`chunkBytes = clamp(archetypeBytes / growDivisor, minChunkBytes,
maxChunkBytes)`, shipped NSDMIs `growDivisor=2`, `minChunkBytes=4KB`, `maxChunkBytes=512KB`) +
**per-chunk-capacity addressing** (dropped the uniform pow2 `m_entitiesPerChunk` shift/mask —
`EntityLocation`/`EntityRecord`/on-disk format unchanged) + **compaction-on-defrag** (rebuild-style
repack of sparse archetypes into fresh target-size chunks, freed chunks TLSF-coalesced). Full design:
`docs/superpowers/specs/2026-07-23-phase-2-dynamic-chunk-sizing-design.md`.

### 3-config test suite (Step 1)

| Config | Total | Passed | Notes |
|---|---|---|---|
| Debug | 722 | 722 | clean |
| Release | 720 | 719 direct / 720 after retry | `CompressionTest.PerformanceBenchmark` failed once (9.71 MB/s vs the >10.0 MB/s throughput gate), passed on 3 isolated reruns (11.99, 10.92, 11.12 MB/s) — the known pre-existing timing-threshold flake, not a regression |
| Dist | 720 | 719 direct / 720 after retry | same flake, isolated reruns 2/2 passed (11.47, 11.36 MB/s) |

No other failures in any config. This matches the expected post-Task-6 counts (722/720/720).

### Machine state — RESOLVED, quiet-machine re-run performed (2026-07-23, later same day)

The first pass at Steps 2/3 (below, in the "Superseded" subsections) ran under **sustained ~70–84%
background CPU load** from the user's own foreground applications (a running game process, Discord, a
browser, Steam/Roblox helpers). The user then closed the game and heavy apps. Before trusting the
machine, load was re-sampled (`Get-CimInstance Win32_Processor LoadPercentage`, repeated windows):
a brief transient spike (avg ~27–29%, one process — `clangd`, the IDE's background indexer — visibly
churning) settled within ~15–20s to a clean **avg 10.7% and 12.6%** across two consecutive windows,
confirmed well under the 15% bar with a per-process CPU-delta snapshot showing only small, ordinary
desktop background consumers (browser tabs, Steam helper) — no heavy foreground app. Steps 2 and 3 were
then re-run in full. **The results in this section are the authoritative, quiet-machine numbers**; the
original noisy-session tables are retained further down, clearly marked **superseded**, for transparency
about what changed and why.

### 3-way benchmark (Step 2) — quiet machine, 7 interleaved rounds (astra→flecs→entt ×7)

`bench_astra.cpp` needed a harness fix before it would even build against this branch's headers (not a
library defect, and unchanged from the first pass): its three `View::ForEach` lambdas were missing the
mandatory leading `Astra::Entity` parameter (every other call site in the repo, e.g.
`benchmark/Benchmark.cpp`, passes `(Astra::Entity, Components&...)`) — fixed in the untracked scratch
file, not a library change. HEAD was confirmed unchanged (`ae40c9b` under the results commit) before
reusing the existing `bench_astra.exe`; no rebuild was needed.

Median of 7 same-session interleaved rounds, N=1,000,000, ns/op (lower = better), with [min, max] spread:

| Operation | Astra | flecs | EnTT |
|---|---|---|---|
| create | **48.56** [46.81, 51.46] | 97.92 [96.73, 103.96] | 41.29 [39.59, 46.77] |
| add | **52.29** [51.40, 52.79] | 55.82 [54.75, 58.16] | 11.73 [11.46, 12.34] |
| remove | 38.25 [37.82, 39.73] | **34.18** [33.83, 36.06] | 16.46 [16.29, 17.30] |
| random_get | 69.70 [67.76, 75.41] | **62.76** [60.03, 83.81]¹ | 32.45 [28.82, 47.52] |
| iterate1 | 0.451 [0.392, 0.652] | **0.396** [0.387, 0.557] | 0.641 [0.565, 0.794] |
| iterate2 | 1.237 [1.049, 1.873] | **1.107** [0.886, 1.464] | 2.420 view / 1.330 group |
| iterate3 | 1.359 [0.975, 1.888] | **1.123** [1.042, 1.652] | 3.277 view |

¹ flecs random_get had one anomalous round (83.807 ns, round 6 of 7) — Astra's and EnTT's own round-6
numbers were unremarkable that round, so the spike looks isolated to flecs, not shared background
noise; excluding it, flecs's clean-round range is [60.03, 65.45].

For context, the historical clean-machine baseline (dev `cb68bf3`, pre-Phase-2, a different session —
included for trend, not as a same-session comparison):

| Operation | Astra (historical) | flecs (historical) | EnTT (historical) |
|---|---|---|---|
| create | 51.4 | 95.9 | 40.6 |
| add | 57.1 | 54.4 | 12.1 |
| remove | 40.6 | 34.1 | 16.1 |
| random_get | 65.4 | 57.0 | 25.3 |
| iterate1 | 0.509 | 0.390 | 0.492 |
| iterate2 | 0.993 | 0.812 | 1.04 (group) |
| iterate3 | 1.050 | 0.889 | 3.21 (view) |

This quiet-machine session's absolute numbers track the historical baseline closely (create/add/remove
within a few percent; flecs create 97.9 vs 95.9 historical, astra create 48.6 vs 51.4 historical) —
consistent with a genuinely quiet machine, unlike the first pass's 1.2×–1.8× inflation.

**Per-round paired comparison** (same round, both libraries) — win counts out of 7, spreads now mostly
non-overlapping (tight and trustworthy):

| Operation | Astra-faster-than-flecs, rounds | Spread overlap? | Read |
|---|---|---|---|
| create | **7/7** | none — Astra max (51.46) < flecs min (96.73) | **robust, decisive** |
| add | **7/7** | none — Astra max (52.79) < flecs min (54.75) | **robust — a clean win, not just "no regression"** |
| remove | 0/7 (flecs faster 7/7) | none — Astra min (37.82) > flecs max (36.06) | robust; Astra ~11.9% slower, close to (slightly better than) the historical ~19% gap — no regression |
| iterate2 | 1/7 (flecs faster 6/7) | full | flecs robustly faster but the median gap shrank a lot (~11.8% vs ~22% historical) |
| iterate3 | 2/7 (flecs faster 5/7) | full | flecs faster; gap (~21%) essentially matches historical (~18%) — no regression, not the hoped "~parity" either |
| iterate1 | 2.5/7 (flecs faster 4/7, one near-tie) | full | close to a coin-flip, but flecs's median edge (~14%) is real; a big improvement over the historical ~31% gap |
| random_get | 1/7 (flecs faster 6/7, excl. the flecs outlier round) | none excluding the outlier | robust; Astra ~11.1% slower — essentially the *same* gap as historical (~15%), i.e. **no improvement materialized here** |

**Verdict against the Step 2 success criteria (quiet-machine, authoritative):**
- **create stays ahead of flecs:** ✅ **confirmed, decisively** — non-overlapping, 7/7 rounds, and the
  gap is even wider than the historical baseline (2.02× vs 1.87× historically).
- **add/remove no regression beyond noise:** ✅ **confirmed, and add is better than required** — add is
  a clean, non-overlapping win over flecs this session (not just parity); remove's gap is essentially
  unchanged from (slightly better than) history.
- **iterate1, iterate2, random_get at-or-ahead of flecs:** ❌ **not met, now confirmed with confidence
  (not noise).** flecs remains ahead on all three. The honest, more nuanced picture: **iterate1 and
  iterate2 both improved substantially** — their gaps to flecs roughly halved from the historical
  pre-Phase-2 baseline (iterate1 ~31%→~14%, iterate2 ~22%→~12%) — real, partial progress toward the
  study's prediction, just not enough to flip the ranking. **random_get did not improve at all** — its
  gap to flecs (~11%) is essentially the same as the pre-Phase-2 historical gap (~15%), contradicting
  the study's predicted +16% Astra-side swing for this specific op. This is now a clean, decisive,
  non-noise finding, not an open question.

### Grow-divisor / max-chunk-bytes mini-sweep (Step 3) — quiet machine re-run

Same `bench-compare/bench_sweep2.cpp` harness, re-run on the quiet machine. Numbers are much tighter
and internally consistent than the first pass (e.g. all six cells' create times now cluster in
47.2–49.0 ns at N=1e6, vs the noisy pass's 67.9–87.6 ns spread).

N=1,000,000, ns/op (shipped NSDMIs `growDivisor=2, maxChunkBytes=512KB` marked `*`):

| growDivisor, cap | create | add | remove | random_get | iterate1 | iterate2 | iterate3 |
|---|---|---|---|---|---|---|---|
| 1, 256KB | 47.34 | 52.25 | 39.04 | 66.78 | 0.391 | 0.907 | 1.338 |
| 1, 512KB | 49.00 | 51.98 | 39.13 | 63.04 | 0.390 | 0.910 | 0.894 |
| 2, 256KB | 48.17 | 52.74 | 38.51 | 64.59 | 0.343 | 0.843 | 1.029 |
| **2, 512KB\*** | **47.17** | 53.80 | 39.57 | 71.21 | 0.400 | 1.029 | **0.849** |
| 4, 256KB | 48.25 | 54.33 | 38.93 | 69.32 | 0.409 | 0.895 | 1.426 |
| 4, 512KB | 48.09 | 51.19 | 37.33 | 64.97 | 0.385 | 0.956 | 0.889 |

(N=100,000 and N=1,000 tables in the untracked `sweep2_results.csv` show the same qualitative picture.)

**Reading:** the shipped `(2, 512KB)` cell wins outright on **create** (47.17, the best of the six) and
**iterate3** (0.849, the best of the six), and is mid-pack on **remove** (39.57) and **iterate1**
(0.400). It is the *worst* of the six on **add** (53.80, ~5% above the best) and **random_get** (71.21,
~11% above the best, `1,512KB`) and **iterate2** (1.029, ~22% above the best, `2,256KB`). No single
alternative cell dominates across all primary ops either: `4, 512KB` is modestly better than shipped on
add/remove/random_get/iterate2 (roughly 2–10%) at N=1e6, and similarly at N=1e5/1e3, but is not
uniformly ahead (create is a rounding error apart, iterate3 favors shipped by ~4.5%) — and each cell was
measured as a single MIN-of-3 sample, not a repeated-trial confidence interval, so a ~5–10% edge on 2 of
7 ops isn't yet strong enough to call a real, reproducible win.

**Verdict: the shipped NSDMIs hold, but not by a landslide.** `growDivisor=2, maxChunkBytes=512KB`
remains a reasonable, defensible default — it wins the two ops the design most cares about
(create, and iterate3 is a bonus) and is never the *worst* choice by a wide margin. `growDivisor=4,
maxChunkBytes=512KB` shows a modest, worth-a-look edge on add/remove/random_get/iterate2 (~2–10%,
fairly consistent across N) that a future pass could investigate with repeated trials, but this single
sweep pass is not a data-backed case to change the NSDMIs now, per the brief's own bar (>5%,
*consistently*, on the primary ops — met on some ops, not all). **NSDMIs left unchanged.**

### Superseded — first pass (noisy machine, ~70–84% background CPU load), kept for transparency

<details>
<summary>Original noisy-session Step 2/3 tables and verdict (click to expand) — superseded by the
quiet-machine re-run above; retained only so the before/after and the reasoning for the re-run are
auditable.</summary>

Median of 7 same-session interleaved rounds under heavy background load, N=1,000,000, ns/op, [min, max]:

| Operation | Astra (noisy session) | flecs (noisy session) | EnTT (noisy session) |
|---|---|---|---|
| create | 73.76 [59.15, 85.40] | 162.63 [125.25, 197.29] | 59.62 [50.30, 75.10] |
| add | 67.73 [64.51, 88.47] | 94.73 [68.52, 104.51] | 15.42 [14.20, 20.61] |
| remove | 59.57 [42.91, 76.73] | 47.74 [43.01, 59.61] | 18.49 [18.01, 23.06] |
| random_get | 194.79 [128.27, 226.27] | 174.28 [113.65, 208.20] | 83.20 [55.53, 146.40] |
| iterate1 | 1.193 [0.639, 2.851] | 1.651 [0.787, 2.544] | 1.941 [1.042, 3.004] |
| iterate2 | 2.474 [2.044, 4.403] | 2.793 [2.128, 5.076] | 4.233 view / 2.785 group |
| iterate3 | 2.651 [2.338, 5.382] | 3.355 [2.216, 5.382] | 4.803 view |

Per-round paired win counts (of 7): create 7/7 Astra, add 7/7 Astra, remove 1/7 Astra (flecs 6/7),
iterate3 5/7 Astra, iterate2 4/7 Astra, iterate1 4/7 Astra, random_get 1/7 Astra (flecs 6/7). Verdict at
the time: create/add/remove robust, iterate1/iterate2/random_get inconclusive (spreads fully
overlapped) — recommended a clean-machine re-run, which is the quiet-machine section above. In
hindsight, the paired win-rate direction for every op (including the inconclusive ones) matches the
quiet-machine result — the noise widened the spreads enough to erase confidence, but didn't flip the
underlying signal.

Grow-divisor sweep, noisy session, N=1,000,000, ns/op:

| growDivisor, cap | create | add | remove | random_get | iterate1 | iterate2 | iterate3 |
|---|---|---|---|---|---|---|---|
| 1, 256KB | 67.86 | 84.60 | 66.77 | 231.90 | 1.536 | 3.440 | 3.904 |
| 1, 512KB | 78.23 | 85.33 | 57.26 | 209.32 | 1.901 | 3.854 | 2.928 |
| 2, 256KB | 77.01 | 85.36 | 75.43 | 263.19 | 0.356 | 2.135 | 4.176 |
| 2, 512KB* | 87.60 | 74.76 | 64.44 | 235.43 | 1.084 | 2.820 | 3.416 |
| 4, 256KB | 87.58 | 85.96 | 61.83 | 212.19 | 0.833 | 2.746 | 3.448 |
| 4, 512KB | 81.06 | 86.78 | 71.29 | 136.18 | 1.943 | 3.229 | 4.031 |

Verdict at the time: inconclusive (non-monotonic, metric-flipping pattern under noise); the quiet-machine
re-sweep above is the authoritative one.

</details>

### Memory-waste check (Step 4)

`ArchetypeTest.SmallArchetypeStaysAtMinimumChunk` (added in Task 5, `tests/Registry/ArchetypeTest.cpp:1382`)
already pins the exact invariant and passes in isolation: 100 `Position` entities (100 × 12B ≈ 1.2KB)
stay in exactly one chunk, and that chunk is exactly 4096 bytes.

A scratch probe (`Registry::GetArchetypeMemoryUsage`/`GetArchetypeCount`, isolated via baseline
subtraction — a fresh `Registry` already carries an empty root archetype with its own fixed
bookkeeping overhead and a legacy-fixed-size chunk, so a second `Registry` with the 100 entities was
diffed against it to isolate the `{Position}` archetype's own contribution) corroborates this at the
public-API level:

```
baseline (root archetype only): archetypeCount=1, ArchetypeMemoryUsage=20992 bytes
with 100 Position entities:     archetypeCount=2, ArchetypeMemoryUsage=29696 bytes (29.00 KB)
{Position} archetype's OWN contribution = 8704 bytes = 4096 (its one 4KB data chunk)
                                           + 4608 (fixed per-archetype bookkeeping, unrelated to Phase 2)
```

4096 bytes for the data chunk itself, exactly the 4KB `minChunkBytes` floor, vs the pre-Phase-2 fixed
`ArchetypeChunkPool::DEFAULT_CHUNK_SIZE = 16384` (16KB) every archetype's first chunk used to cost
regardless of how little data it held — a genuine 4× reduction for small archetypes, confirming Phase
2's memory-waste goal. No new test was added; Task 5's existing test already covers this exactly.

### Overall Phase-2 verdict (quiet-machine, authoritative)

- **Robust, decisive wins:** create (2.02× ahead of flecs, 7/7 non-overlapping rounds — even better than
  the historical 1.87×), add (a clean, non-overlapping win over flecs, 7/7 rounds — better than the
  "no regression" bar), and the memory-waste invariant (4× smaller footprint for small archetypes,
  directly measured).
- **No regression:** remove (~11.9% behind flecs, essentially the historical gap, slightly improved).
- **Real, partial progress — gap roughly halved but ranking unchanged:** iterate1 (~31%→~14% behind
  flecs) and iterate2 (~22%→~12% behind flecs) both improved substantially versus the historical
  pre-Phase-2 baseline, confirming Phase 2 helped iteration meaningfully — just not enough to reach
  "at-or-ahead of flecs."
- **Did not improve:** random_get's gap to flecs (~11%) is essentially unchanged from the historical
  pre-Phase-2 gap (~15%) — the study's predicted +16% swing for this specific op did not materialize.
  This is now a clean, decisive, non-noise finding (non-overlapping spreads, 6/7 paired rounds), not an
  open question.
- **Divisor/cap sweep:** the shipped `growDivisor=2, maxChunkBytes=512KB` NSDMIs hold — best on
  create and iterate3, mid-pack on remove/iterate1, worst (by 5–22%) on add/random_get/iterate2 versus
  the best alternative cell in each case, with no single alternative dominating across all primary ops.
  `growDivisor=4` (either cap) is a modest, worth-a-future-look candidate on add/remove/random_get/
  iterate2, but this single-sample-per-cell sweep isn't a strong enough basis to change the defaults now.
- **No library defect found.** All test-suite failures were the pre-existing `CompressionTest` timing
  flake, confirmed via isolated reruns. This task changed no library code (only the untracked benchmark
  scratch harness).
- **Net read for the merge decision:** Phase 2 delivers on its memory-waste goal outright, delivers a
  decisive structural-op win (create, add), holds steady on remove, and makes real but incomplete
  progress on iteration (iterate1/iterate2) — while random_get's predicted improvement did not
  materialize on this machine. None of this is a regression; the shortfall is against the study's
  optimistic prediction, not against Astra's own pre-Phase-2 baseline.

## Lever 1 — random_get record→direct-chunk-pointer (2026-07-24, branch `perf/random-get-chunk-pointer` @ `f5ea6c3`)

Design: `docs/superpowers/specs/2026-07-24-random-get-chunk-pointer-design.md`. `EntityRecord` now caches
the `ArchetypeChunk*` for its location (behind a write funnel — all 16 write sites routed through
`EntityTable::SetRecord`/`ArchetypeManager`'s `SetRecordLocation`/`ClearRecordLocation`), so
`Archetype::GetComponent` and `Registry`'s ByID/hash `GetComponent` paths read `rec->chunk` directly
instead of hopping through `archetype->GetChunks()[location.GetChunkIndex()]` + a mask test — shortening
the dependent-load chain from 8 hops to ~5. The trade-off, called out in the spec up front: `EntityRecord`
grew from 24B to a hard `alignas(32)` 32B (guaranteeing no record ever straddles a cache line — 1/3 of the
old 24B records did) at the cost of a fatter per-entity record, i.e. lower record density per cache line
for the random-order lookups this lever targets.

### 3-config test suite (Step 1)

| Config | Total | Passed | Notes |
|---|---|---|---|
| Debug | 732 | 732 | clean, no flake |
| Release | 730 | 730 | clean, no flake |
| Dist | 730 | 730 | clean, no flake |

Matches the expected post-branch counts (732/730/730 — the Release/Dist delta from Debug is the 2
`EXPECT_DEATH`-only tests, same spread pattern as dev's 724/722/722). `CompressionTest.PerformanceBenchmark`
did not flake this run; no reruns were needed. `bench_astra.exe` was rebuilt clean against this branch's
headers (Step 2) with no source changes needed this time (the `View::ForEach` leading-`Entity`-param fix
from the Phase-2 session was already present in the untracked harness).

### Machine load (Step 3 pre-check)

`typeperf "\Processor(_Total)\% Processor Time"`, two windows of 6–8 samples each, taken immediately
before benching: **avg ~9.9% and ~11.7%** (samples 8–15%, one 14.8%/14.5% pair, otherwise high-single-digits)
— comfortably under the 15–20% quiet-machine bar (`Get-Process` showed the usual desktop background set —
Discord, Steam helpers, a Sublime/editor process, browsers, no heavy foreground app — consistent with
ordinary idle-desktop load, not a spike). Machine judged quiet; results below are **not** noise-flagged.

### 6 interleaved rounds (astra→flecs→entt ×6), N=1,000,000, ns/op, median [min, max]

| Operation | Astra (this branch) | flecs | EnTT |
|---|---|---|---|
| create | 50.45 [49.59, 61.73]¹ | 100.93 [98.17, 104.28] | 41.83 [40.00, 48.16] |
| add | 54.14 [52.04, 56.19] | 57.34 [56.52, 62.04] | 11.90 [11.59, 12.51] |
| remove | 37.65 [37.22, 38.69] | 34.58 [33.80, 35.34] | 16.47 [16.29, 16.87] |
| **random_get** | **74.55 [72.42, 88.32]¹** | 67.26 [61.99, 71.45] | 37.51 [32.07, 41.62] |
| iterate1 | 0.468 [0.401, 0.539] | 0.476 [0.393, 0.553] | 0.754 [0.690, 1.361] |
| iterate2 | 1.107 [0.958, 1.285] | 1.122 [0.937, 1.511] | 2.608 view / 1.524 group |
| iterate3 | 1.471 [1.091, 2.087]¹ | 1.507 [1.256, 2.742] | 3.516 view |

¹ Round 3 had a latency blip localized to Astra's turn (create 61.73, random_get 88.32, iterate3 2.09 —
all well above the other 5 rounds), while flecs's and EnTT's own round-3 numbers were unremarkable that
round — looks like a scheduler/cache hiccup isolated to Astra's process, not shared background noise.
Excluding round 3: create median 50.05 [49.59, 51.70], random_get median 72.71 [72.42, 76.46], iterate3
median 1.40 [1.09, 1.67] — the random_get exclusion narrows the number but does not change the verdict
below (still well above the ≤63ns target, still robustly slower than flecs every round).

### Before/after vs the 2026-07-23 pre-lever baseline

| Operation | Astra before (dev, `ec1d5ce`) | Astra after (this branch) | Δ% | Target |
|---|---|---|---|---|
| create | 48.56 | 50.45 | +3.9% | within noise |
| add | 52.29 | 54.14 | +3.5% | within noise |
| remove | 38.25 | 37.65 | -1.6% | within noise |
| **random_get** | **69.70** | **74.55** | **+7.0%** | **≤63 (missed)** |
| iterate1 | 0.451 | 0.468 | +3.8% | within noise |
| iterate2 | 1.237 | 1.107 | -10.5% | within noise |

**Per-round paired comparison, random_get, Astra vs flecs:** flecs faster in **6/6** rounds, no spread
overlap (Astra min 72.42 > flecs max 71.45) — identical direction and a near-identical relative gap to the
pre-lever baseline (this session: (74.55−67.26)/67.26 = **10.8%** Astra-slower; pre-lever baseline session:
(69.70−62.76)/62.76 = **11.1%** Astra-slower). create/add/remove keep their pre-lever win/loss pattern too
(create 6/6 Astra-faster, add 6/6 Astra-faster, remove 6/6 flecs-faster — all matching the historical 7/7
patterns), confirming the lever changed nothing structural about any op except the one it targeted.

**Interpretation.** The lever missed its target: random_get's median (74.55ns) is not just short of the
≤63ns goal, it's numerically *worse* than the 69.70ns pre-lever baseline. Two pieces of evidence say this
is not primarily a regression introduced by the branch, though: (1) flecs's and EnTT's own random_get
numbers inflated by similarly large amounts in this same session versus their 2026-07-23 numbers (flecs
62.76→67.26, +7.2%; EnTT 32.45→37.51, +15.6%) — a session-to-session shift affecting all three libraries in
the same direction, most plausibly machine/thermal/turbo variance rather than anything in Astra's code path;
and (2) the *same-session, paired* Astra-vs-flecs gap (10.8%) is essentially unchanged from the pre-lever
historical gap (11.1%) — the fair, noise-controlled comparison says the gap didn't move, not that it grew.
Taking both together, the honest read is **no measurable improvement**, not a proven regression: the
predicted win (dependent-load chain 8→~5) did not show up in the gap to flecs, and the most likely
structural explanation is the trade-off the spec called out up front — growing `EntityRecord` from 24B to a
hard-aligned 32B lowers record density for exactly the random-order access pattern this lever targets, and
that cache-pressure cost plausibly ate the shortened-chain benefit. All other primary ops (create, add,
remove, iterate1, iterate2) stayed within session-level noise of their pre-lever baselines (−10.5%..+3.9%)
and kept the same win/loss pattern against flecs — **no regression found anywhere except the missed
random_get target.** This is a real, decisive, non-noise finding (not an open question) and should inform
the merge decision as such: the lever is safe to merge (no regressions), but should not be described as
having closed the random_get gap.

### Full-opt flag re-run — NEW BENCH FLAG BASELINE (2026-07-24, dev @ `5590ca8`, post-Lever-1 merge)

**Why.** Every bench run above built with minimal flags (`/O2 /DNDEBUG /EHsc`). The solution's own Dist
config compiles with substantially more: `/arch:AVX`, manual `__SSE2__`/`__SSE4_2__` defines (MSVC never
auto-defines these — without them Mosaic's `Platform.hpp:172-191` compiles OUT the hardware-CRC32 and AVX
SIMD tiers, so every prior bench ran Astra at its SSE2 floor), `/fp:fast`, `/GL`+`/LTCG`.
(`ASTRA_BUILD_DIST` was checked and is cosmetic — a build-type string; `NDEBUG` governs asserts.)
All THREE benches were rebuilt with the identical full flag set for fairness:
`/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /link /LTCG`
(+ `/DASTRA_BUILD_DIST` for Astra; flecs.c compiled `/c /O2 /GL /W0 /DNDEBUG /arch:AVX /fp:fast`).
**These flags are the bench recipe from now on** — they match what a real optimized consumer ships with.

**Setup.** Quiet machine (typeperf ~3-11% before the run). 6 interleaved rounds (astra→flecs→entt), N=1M,
medians [min,max]. Raw: `ab2_rounds.csv` (untracked).

| Operation | Astra | flecs | EnTT | Astra vs flecs |
|---|---|---|---|---|
| create | 49.90 [48.69, 51.17] | 89.54 [88.35, 92.52] | 38.91 [37.29, 44.71] | **1.79× ahead** |
| add | 48.63 [47.34, 50.11] | 50.99 [49.18, 52.84] | 11.65 [11.21, 12.93] | **~5% ahead** |
| remove | 37.36 [36.95, 38.24] | 33.84 [31.51, 35.98] | 16.59 [16.44, 17.05] | ~10% behind |
| random_get | 60.03 [54.09, 70.05] | 56.21 [54.77, 67.93] | 24.40 [22.26, 45.98] | ~4% behind (paired median; **2/6 rounds ahead**) |
| iterate1 | 0.484 [0.385, 0.602] | 0.434 [0.382, 0.741] | 0.576 [0.515, 0.732] | ~11% behind (spreads overlap heavily) |
| iterate2 | 1.046 [0.836, 1.148] | 0.885 [0.846, 1.036] | 2.80 view / 1.34 group | ~18% behind — now the biggest gap |
| iterate3 | 0.997 [0.915, 1.216] | 1.010 [0.892, 1.129] | 3.23 view | **~parity (slightly ahead)** |

Per-round paired random_get gaps (Astra vs flecs): −1.1%, +4.4%, −1.3%, +17.8%, +3.1%, +8.1%.

**Interpretation.** Under Dist-parity flags the random_get gap to flecs narrows from ~11% (both prior
sessions) to **~4% median, with Astra winning 2 of 6 rounds — effectively near-parity**. IMPORTANT CAVEAT:
this run changes BOTH the flags and (vs the 07-23 baseline) includes the Lever-1 merge, and no pre-lever
build was measured under the new flags — so the narrowing CANNOT be attributed to the lever vs the SIMD
tiers (hardware CRC32, AVX) vs LTCG; they are confounded. What can be said cleanly: **on the flags a real
user would ship with, current dev is 1.79× ahead of flecs on create, ahead on add, near-parity on
random_get and iterate3, ~10% behind on remove, and ~18% behind on iterate2** — iterate2 replaces
random_get as the biggest flecs gap (flecs's 2-component loop benefits strongly from AVX+`/fp:fast`
auto-vectorization: its iterate2 dropped 1.11→0.885 across the two same-day sessions while Astra's dropped
only 1.107→1.046). EnTT's sparse-set random_get also leaps ahead under full opts (37.5→24.4) — model-inherent,
not a target. Net for the lever program: lever 2 (per-chunk iteration-setup flattening, iterate1/2) is now
clearly the highest-value target; remove (~10%) second; random_get is no longer the headline gap.

## Lever 2 — remove-path validate-once seam + move-path micro-work (2026-07-24, branch `perf/lever2-remove-path` @ `dc9cae9`)

Design: `docs/superpowers/specs/2026-07-24-lever2-remove-path-design.md`. Plan:
`docs/superpowers/plans/2026-07-24-lever2-remove-path.md`. Goal: close the remove-path gap to flecs
(37.36 vs 33.84 ns, ~10% behind — the 2026-07-24 full-opt baseline above) by cutting redundant
validation at the `Registry` seam and wasted work in the transition-move internals.

**Branch built on dev @ `4d98942`. Commits, in order:**
- `996bfd0` — signal-contract characterization tests (pins add/remove emission + return-value
  behavior ahead of the seam rewrite; 3 tests, all pass pre-rewrite as required for a
  behavior-preserving refactor).
- `fead7a5` — **Phase A: validate-once seam.** Dropped redundant `IsValid`/`GetComponent`
  pre-checks at 5 `Registry` sites (`AddComponent<T>`, `EmplaceComponent<T>`, `RemoveComponent<T>`,
  `AddComponentByID`, `RemoveComponentByID`), hoisting signal-payload work behind
  `IsSignalEnabled` so the common (signals-disabled) path does one call instead of three. Rides
  along a Lever-1 rider: typed `GetComponent` gains a `rec->chunk` null-check.
- `ab917e7` — **B1:** `ComponentDescriptor::Destruct` gains the triviality gate that
  `MoveConstruct`/`DefaultConstruct` already had — was an unconditional indirect function-pointer
  call, firing twice per column on every swap-remove backfill even for trivially-destructible types.
- `dc9cae9` — **B3 + B4:** B3 is ordinal-direct column addressing in the transition merge-join
  (new `ArchetypeChunk::GetColumnPointer(column, index)`, killing 4 `idToColumn` re-resolutions per
  move in `MoveEntityFrom`); B4 is `AllocateEntitySlot` switched from resize+index-write to
  `push_back`, killing a value-init-then-overwrite on every slot append.

**B2 (fused move-out + backfill) was SKIPPED at the plan's Task-4 controller gate, by user
decision.** Per the plan (`docs/superpowers/plans/2026-07-24-lever2-remove-path.md` Task 4), the
gate rule is: if Astra remove is still behind flecs after checkpoint 2, proceed to B2 (Task 5);
if Astra remove is at-or-ahead of flecs, stop and ask the user whether B2 still lands. Checkpoint 2
(below) showed Astra remove **~23% ahead** of flecs, 6/6 rounds — the target was decisively met
without the riskier fused-move rewrite, so the user chose to stop at Task 6. B2 remains documented
as a future lever in the spec (§4 B2). **There is no checkpoint 3.**

### Authoritative 3-config

The last code commit on the branch is `dc9cae9` (B3+B4); confirmed via `git log` that HEAD is
still `dc9cae9` and `git status` shows no tracked-file changes (only the pre-existing pile of
untracked `bench-compare/` scratch files and untracked review docs, unrelated to this branch) —
no code has changed since that commit's own 3-config run (Task 3 Step 5), so it stands as
authoritative without a re-run:

| Config | Total | Passed | Notes |
|---|---|---|---|
| Debug | 736 | 736 | clean, no flake |
| Release | 734 | 734 | clean, no flake |
| Dist | 734 | 734 | clean, no flake |

Zero failures, zero `EntityRecord chunk/location desync` assert aborts (the Lever-1 desync guard
sweeps every structural test in the suite), zero `Tracked::s_live` lifetime-imbalance in any
config. 736/734/734 matches 735 (post-Phase-A) + 1 (B1's added triviality-gate flag test); B3/B4
added no new tests (pure internal rewiring, covered by the existing structural-transition suite).

### Checkpoint table — baseline → ckpt1 (Phase A) → ckpt2 (B1+B3+B4), same-session paired medians vs flecs

N=1,000,000, ns/op, median of 6 interleaved rounds each session. ckpt1 ran under **heavy background
load (~22-30%, labeled noisy — provisional)**; ckpt2 ran under **mild load (~16%, labeled mildly
noisy)** — neither is a fully quiet-machine read, but both are internally paired (same-session
Astra-vs-flecs), which cancels most common-mode noise. Full per-round data:
`.superpowers/sdd/task-1-report.md` (ckpt1), `.superpowers/sdd/task-3-report.md` (ckpt2).

| Operation | Baseline (390fb10) Astra / flecs | ckpt1 (post-Phase-A, noisy) Astra / flecs | ckpt2 (post-B1+B3+B4, mildly noisy) Astra / flecs |
|---|---|---|---|
| create | 49.90 / 89.54 (1.79× ahead) | 51.34 / 95.80 (1.87× ahead) | 51.127 / 90.903 (**1.78× ahead**) |
| add | 48.63 / 50.99 (~5% ahead) | 49.80 / 51.80 (~3.9% ahead) | 46.672 / 50.894 (**~8.3% ahead**) |
| remove | 37.36 / 33.84 (~10% **behind**) | 32.24 / 34.32 (~6.0% ahead) | 25.748 / 33.441 (**~23.0% ahead**) |
| random_get | 60.03 / 56.21 (~6.8% behind) | 74.73 / 68.33 (~9.4% behind) | 70.242 / 63.586 (~10.5% behind) |
| iterate1 | 0.484 / 0.434 | 0.456 / 0.501 | 0.504 / 0.499 |
| iterate2 | 1.046 / 0.885 | 1.385 / 1.439 | 1.091 / 1.134 |
| iterate3 | 0.997 / 1.010 | 1.416 / 1.347 | 1.260 / 1.330 |

Per-round paired Astra-vs-flecs deltas at ckpt2 (same session, cancels common noise; 6/6 = Astra
faster all 6 rounds):
```
create             -43.5%, -46.2%, -42.4%, -44.4%, -42.5%, -43.4%   (astra faster, 6/6)
add_component      -8.2%, -6.6%, -7.3%, -9.9%, -6.9%, -10.0%         (astra faster, 6/6)
remove_component   -21.0%, -23.2%, -26.5%, -24.3%, -24.0%, -18.3%    (astra faster, 6/6 -- wider and tighter than ckpt1's -2.2%..-25.0%)
random_get         +11.6%, +10.7%, +11.0%, +11.9%, +13.5%, +7.3%     (astra slower, 6/6; untouched path, matches ckpt1's shape)
```

### Per-phase attribution (approximate — ckpt1 ran noisy)

- **Phase A alone (ckpt1, noisy session):** flipped remove from ~10% behind flecs to ~6% ahead
  (6/6 rounds Astra-faster, a directional flip from the historical 6/6-flecs-faster pattern) —
  consistent with the algorithmic story: the hot arm (signals disabled) collapsed from three calls
  (`IsValid` + `GetComponent` + `RemoveComponent`) to one.
- **+B1+B3+B4 (ckpt2, mildly noisy session):** remove widened further to ~23% ahead, with a
  tighter, decisively-shifted per-round band (−18.3%..−26.5% vs ckpt1's −0.9%..−25.0%) — every
  ckpt2 round beats ckpt1's best round. add also widened its lead further (~3.9%→~8.3% ahead),
  since `AddComponent`'s transition runs the same `MoveEntityFrom`+`AllocateEntitySlot` internals
  B3/B4 touched.
- **Attribution honesty:** because ckpt1 ran under materially heavier load (~22-30%) than ckpt2
  (~16%), the exact ckpt1→ckpt2 delta is not a clean isolated measurement of B1+B3+B4's
  contribution alone — some of the apparent widening could be ckpt2 simply being the less-noisy
  session. What is robust across both sessions regardless of load: remove's *directional* flip
  (Phase A) and *further* widening (the safe tranche) are each independently 6/6-round-consistent,
  which is a real signal, not noise. create stayed flat throughout (untouched code path, as
  expected — a stability check that nothing else broke).
- **random_get** ran session-elevated in both checkpoints (74.73 and 70.242 vs the fully-quiet
  baseline's 60.03) — environmental, not a regression: it is an untouched code path (aside from the
  Lever-1 rider's `if (!rec->chunk)` null-check, a correctly-predicted not-taken branch), and the
  *paired* gap to flecs stayed essentially stable across both noisy sessions (~9.4% at ckpt1,
  ~10.5% at ckpt2) rather than tracking the absolute-value inflation — evidence the null-check adds
  no measurable cost and the swings are machine load, not the branch's code.

### Verdict vs the ≤~34ns target

The plan's target was **remove at or within noise of flecs (~34 ns)**. Checkpoint 2 measured Astra
remove at **25.748 ns vs flecs 33.441 ns — ~23% AHEAD, 6/6 rounds, non-overlapping-band**. The
target was not merely met, it was smashed, decisively enough (a directional flip sustained and
widened across two independent sessions under different load conditions) that the plan's own gate
rule triggered a stop-and-ask before the riskier B2 rewrite — see the gate decision above. Net
program effect of Lever 2: remove flipped from Astra's single biggest structural-op deficit
(~10% behind flecs) to one of its clearest wins (~23% ahead), and add's existing lead widened
(~5%→~8.3% ahead) as a side effect of touching the same shared transition internals. create,
random_get, and iterate1/2/3 — all untouched by this lever — held within session-noise bands of
their pre-lever values, with random_get's absolute inflation attributed to machine load rather
than the Lever-1 rider (see above).

`ckpt1.csv`/`ckpt2.csv` are untracked scratch (not committed — consistent with the pile of prior
scratch csvs already `git status`-untracked in `bench-compare/`); the per-round raw data lives in
`.superpowers/sdd/task-1-report.md` and `.superpowers/sdd/task-3-report.md`.

### growDivisor=4 re-look — VERDICT: KEEP 2 (2026-07-24, dev @ `24fa402`, post-Lever-2)

The Phase 2 divisor sweep left "growDivisor=4 worth a future look (2-10% on some ops)" open; that data
was noisy, pre-full-opt-flags, and pre-Levers-1/2. Re-ran `bench_sweep2.cpp` (divisors {1,2,4} x caps
{256,512}KB x N {1K,100K,1M} x 7 ops, MIN-of-3 per cell) rebuilt with the full-opt flags against current
dev, on the quietest machine conditions of the program (~3-6% typeperf), 3 full passes, min-across-passes
per cell (raw: `divisor_pass1-3.csv`, untracked scratch).

**Result: no coherent win for divisor 4.** At the shipped 512KB cap, divisor-4 deltas vs 2 are small and
sign-inconsistent across N (create -0.6/-1.1/-2.6%, add +1.8/+0.4/+0.4%, remove -2.2/+1.1/+0.7%,
random_get +1.6/-1.9/-3.8%, iterate2 0/+0.3/-3.7%). The LARGEST-magnitude cells are losses, and they
cluster where the mechanism predicts them: mid-size archetypes iterate worse under divisor 4's slower
chunk ramp (iterate1 +12.2% @512KB/100K; iterate3 +33.4% @256KB/100K — more, smaller chunks at mid-N).
The divisor-1 reference column shows the same mixed-noise character. The Phase-2 "2-10% on some ops"
signal does not reproduce as a consistent direction under honest flags on quiet hardware.

**Decision: `growDivisor = 2` NSDMI stands. Item closed** — reopen only with a workload argument
(e.g. a memory-pressure profile favoring slower ramps), not a throughput one.

## Definitive 3-way scoreboard (2026-07-24)

Full-surface follow-up to the 7-op head-to-head above: `bench-compare/` was extended
(`docs/superpowers/specs/2026-07-24-definitive-3way-benchmark-design.md`, user-approved) to
mirror Astra's shipped `benchmark/Benchmark.cpp` wherever a fair cross-library idiom exists —
15 Tier-A 3-way ops and 5 Tier-B 2-way (Astra/flecs) ops, at 1M (iteration family also at 10M,
relations at 100K nodes) — and run as one interleaved campaign at scale. This section
supersedes the 7-op scoreboard above as the record for every op it covers; the 7-op section is
left in place for its own historical checkpoints (Lever 1/2, Phase 2, growDivisor).

### Campaign conditions

- **Build:** the full-opt Dist-parity recipe, identical across all three libraries —
  `/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc
  /nologo ... /link /LTCG`, `+/DASTRA_BUILD_DIST` for Astra, plus Astra-only
  `/I..\include /I..\vendor\Mosaic\include /I..\tests` (the `/tests` include is a scheduler-op
  dependency — Astra's `ParallelForEach`/`SystemScheduler` benches reuse the shipped test
  suite's reference `IWorkScheduler`, `Astra::Testing::TestWorkerPool`, same as
  `benchmark/Benchmark.cpp`'s own `BenchPool()`). flecs linked against the pre-built
  `flecs.obj` (not rebuilt); EnTT header-only. All three exes rebuilt fresh immediately before
  the campaign (2026-07-24 18:35-18:36 local) against dev `f0734cb` (the two commits on top of
  the `8878112` perf baseline are docs-only — spec + plan — no library code changed; verified
  `git diff --stat 8878112 HEAD -- include/` empty).
- **Cross-lib validation (Step 1):** all three exes run once (`definitive_smoke.csv`,
  `round0,` prefix) and fed through `analyze_definitive.py`'s items-processed check —
  **PASS, zero mismatches** across every 3-way op (create/create_batch/add_component/
  remove_component/add_batch/remove_batch/destroy/iterate1/iterate5/iterate2_half/
  iterate2_one/random_get/get_multi) and every 2-way op (iterate2/iterate3 astra-vs-flecs;
  relations_children/descendants/ancestors, system_tick_seq, parallel_iterate2). No fix-up
  needed before the campaign.
- **Campaign:** 6 interleaved rounds, `astra → flecs → entt` per round, each exe's full stdout
  captured and prefixed `roundN,` into `definitive_campaign.csv` (432 rows total, 18 exe
  invocations, each its own foreground pass, exit code 0 every time).
- **Machine load evidence (`typeperf "\Processor(_Total)\% Processor Time"`, 8 samples/2s):**
  - Pre-campaign: 13.6, 11.9, 9.8, 10.0, 9.8, 7.0, 8.5, 9.8 — avg **≈10.1%**, quiet.
  - Mid-campaign (immediately after round 3): 41.0, 8.6, 12.8, 8.5, 10.4, 8.9, 7.8, 9.4 — avg
    ≈13.4%, driven by one transient first-sample spike; 7-of-8 samples avg ≈9.5%.
  - Post-campaign (immediately after round 6): 61.5, 55.9, 25.4, 23.1, 37.0, 33.9, 47.8, 29.5 —
    avg ≈39.1%, clearly elevated; a re-check ~40s later showed it settling (36.6→17.0, avg
    ≈18.6%). This spike happened **after** round 6's last measurement completed, not during it.
  - **Round-3 entt outlier, attributed to the mid-campaign spike:** cross-checking individual
    ops across all 6 rounds shows round 3's **entt** invocation specifically elevated on
    `create` (83.08 vs ~48 the other 5 rounds), `create_batch` (64.40, though round 5 also ran
    high at 59.81 — see below), `add_component` (22.48 vs ~15-16), `add_batch` (20.95 vs
    ~14-15), `remove_component` (20.01 vs ~16), `iterate1` (1.726 vs ~0.5), `iterate2_half`
    (3.934 vs ~1.2), `random_get` (82.93 vs ~30), `get_multi` (150.43 vs ~65) — the mid-campaign
    typeperf check was taken immediately after round 3's entt run finished, and its first
    sample (41.0%) is consistent with a spike landing inside that exe invocation. **Astra and
    flecs's round-3 numbers, and every lib's round 4-6 numbers, are unaffected** (spot-checked
    across `create`, `iterate1`, `random_get`, `destroy`, `remove_batch` — round 6 in
    particular tracks rounds 1/2/4/5 tightly, confirming the post-campaign spike began only
    after the campaign's last measurement). Net effect: entt's reported **bands** (not medians)
    are honestly widened by this one round on the affected ops — the median-of-6 stays a robust
    central estimate, and the non-overlapping-band rule (spec Sec.6) correctly withholds a gap
    claim wherever this widening bridges the astra/entt gap (see random_get, get_multi below).
    `create_batch`'s additional round-5 elevation (59.81 vs round-3's 64.40, both above the
    ~42-46 baseline of rounds 1/2/4/6) is unexplained by the load evidence and is disclosed as
    ordinary run-to-run variance on that op specifically.
- **Items validation (Step 3):** re-run across the full 432-row campaign CSV, all 6 rounds —
  **PASS, zero mismatches**, same op set as the Step-1 smoke check.
- **Op-name note:** entt reports `iterate2`/`iterate3` under `iterate2_view`/`iterate3_view`
  (plus a bonus owning-group entry `iterate2_group`, kept from the pre-existing file), so the
  analysis script's data-driven op→libs grouping correctly treats those two ops as **2-way**
  (astra vs flecs) with entt's numbers reported alongside as informational 1-way context, never
  fabricated into a false 3-way band.

### Tier-A table (median [min,max] ns/op, 6 rounds; N=1,000,000 unless noted)

| op | N | Astra | flecs | EnTT | verdict |
|---|---|---|---|---|---|
| create | 1M | 53.49 [51.99, 55.96] | 90.00 [88.94, 92.17] | 48.38 [47.55, 83.08]* | **astra beats flecs 1.68×** (non-overlap); vs entt: noise (band widened by round-3 spike*) |
| create_batch | 1M | 34.06 [33.42, 36.85] | 17.17 [16.52, 20.71] | 44.85 [42.60, 64.40]* | **astra behind flecs** (0.50×, non-overlap, tuning target); **astra beats entt 1.32×** (non-overlap) |
| add_component | 1M | 38.77 [38.36, 40.25] | 50.24 [49.30, 53.28] | 15.90 [14.95, 22.48]* | **astra beats flecs 1.30×** (non-overlap); **astra behind entt** (0.41×, non-overlap, tuning target) |
| remove_component | 1M | 26.29 [25.49, 27.38] | 32.89 [32.18, 34.35] | 16.22 [16.00, 20.01]* | **astra beats flecs 1.25×** (non-overlap); **astra behind entt** (0.62×, non-overlap, tuning target) |
| add_batch | 1M | 80.26 [79.30, 81.51] | 93.43 [88.18, 99.22] | 14.56 [13.76, 20.95]* | **astra beats flecs 1.16×** (non-overlap); **astra behind entt** (0.18×, non-overlap, tuning target — deferred-batch idiom, see caveat) |
| remove_batch | 1M | 62.45 [59.49, 64.43] | 63.63 [61.22, 69.01] | 14.78 [14.48, 17.32] | vs flecs: noise (parity, 1.02×); **astra behind entt** (0.24×, non-overlap, tuning target — deferred-batch idiom, see caveat) |
| destroy | 1M | 26.14 [25.84, 27.23] | 14.58 [14.06, 15.36] | 47.34 [46.40, 55.47] | **astra behind flecs** (0.56×, non-overlap, tuning target); **astra beats entt 1.81×** (non-overlap) |
| iterate1 | 1M | 0.461 [0.373, 0.539] | 0.392 [0.368, 0.406] | 0.533 [0.492, 1.726]* | noise vs both (overlap) |
| iterate1 | 10M | 0.765 [0.698, 0.843] | 0.701 [0.688, 0.892] | 0.990 [0.844, 1.824]* | vs flecs: noise; **astra beats entt 1.29×** (non-overlap) |
| iterate2 | 1M | 0.980 [0.821, 1.437] | 0.904 [0.794, 1.215] | 2.231 [2.133, 4.272]† (`iterate2_view`) | vs flecs: noise (2-way op); entt informational only (†, different op name) |
| iterate2 | 10M | 1.171 [1.130, 1.281] | 1.209 [1.156, 1.351] | 2.327 [2.239, 4.302]† | vs flecs: noise; entt informational only (†) |
| iterate3 | 1M | 1.123 [0.926, 1.404] | 0.933 [0.898, 1.493] | 2.958 [2.838, 5.476]† (`iterate3_view`) | vs flecs: noise (2-way op); entt informational only (†) |
| iterate3 | 10M | 1.292 [1.271, 1.380] | 1.333 [1.238, 1.615] | 3.114 [2.953, 5.531]† | vs flecs: noise; entt informational only (†) |
| iterate5 | 1M | 1.450 [1.357, 1.632] | 1.286 [1.033, 1.762] | 5.115 [4.918, 8.489] | vs flecs: noise; **astra beats entt 3.53×** (non-overlap) |
| iterate5 | 10M | 1.542 [1.488, 1.902] | 1.667 [1.492, 1.960] | 5.148 [4.986, 8.152] | vs flecs: noise; **astra beats entt 3.34×** (non-overlap) |
| iterate2_half | 1M (items=500K) | 0.273 [0.250, 0.316] | 0.260 [0.242, 0.346] | 1.214 [1.161, 3.934]* | vs flecs: noise; **astra beats entt 4.45×** (non-overlap — the archetype-skip-vs-pool-intersect asymmetry the op was designed to expose) |
| iterate2_one | 1M (items=1) | 0.000 | 0.000 | 0.000 | all three at/near the timer's resolution floor — not a meaningful differentiator at this scale |
| random_get | 1M | 56.95 [53.85, 59.33] | 56.76 [52.45, 63.60] | 31.48 [29.27, 82.93]* | **vs flecs: PARITY** (0.997×, overlap) — confirms the 2026-07-24 Lever-1 program closed the historical ~10% gap; vs entt: noise (band widened by round-3 spike*; 5-of-6 rounds show entt ~29-34ns, a real but not campaign-certified advantage) |
| get_multi | 1M (items=2M) | 114.31 [109.12, 142.79] | 99.90 [95.37, 105.26] | 67.71 [62.81, 150.43]* | **astra behind flecs** (0.87×, non-overlap, tuning target); vs entt: noise (band widened by round-3 spike*; median gap is large — directional signal, not certified) |

\* = round-3-spike-widened band (see Load evidence). † = entt reports this op under a
different CSV name (`iterate2_view`/`iterate3_view`); the analysis script correctly scores
this as a 2-way astra/flecs op, entt shown for context only, never forced into a false 3-way
band.

### Tier-B table (Astra vs flecs only; median [min,max] ns/op, 6 rounds)

| op | N | Astra | flecs | verdict | caveat |
|---|---|---|---|---|---|
| relations_children | 100K nodes (items=100) | 0.0160 [0.0150, 0.0160] | 0.0090 [0.0090, 0.0130] | **astra behind** (0.56×, non-overlap, tuning target — weak signal, see caveat) | absolute measured region is ~100 children total per rep (≈900-1600ns); signal is thin relative to typical timer/loop overhead — treat as a directional flag, not a confident regression |
| relations_descendants | 100K nodes (items=99,999) | 7.325 [6.981, 7.489] | 48.94 [42.20, 70.01] | **astra beats flecs 6.68×** (non-overlap) | disclosed traversal-strategy difference (spec caveat): Astra's cached `ForEachDescendant` vs flecs's recursive `children()` walk — both visit the identical 99,999-node set, the strategies genuinely differ and Astra's caching wins decisively here |
| relations_ancestors | 100K nodes (items=448,889) | 122.33 [115.81, 125.35] | 76.96 [75.30, 84.07] | **astra behind** (0.63×, non-overlap, tuning target) | both first-class per-leaf walk-ups; a real gap |
| system_tick_seq | 1M entities × 3 systems (items=3M) | 1.533 [1.504, 1.689] | 1.579 [1.388, 1.964] | noise (parity, 1.03×) | one full tick, 3 lambda systems (movement/damage/heal) vs flecs's 3-system pipeline `progress()` |
| parallel_iterate2 | 1M | 0.288 [0.239, 0.391] | 0.270 [0.254, 0.361] | noise (overlap) | `hardware_concurrency()` lanes both libs, one unmeasured warm-up |
| parallel_iterate2 | 10M | 0.850 [0.840, 0.854] | 0.893 [0.876, 0.987] | **astra beats flecs 1.05×** (non-overlap, tight bands) | — |
| parallel_iterate2_handrolled (entt, informational) | 1M / 10M | — | — | 1.611 [1.298, 2.316] / 1.616 [1.566, 2.138] | EnTT ships no parallel-for; hand-rolled `hw`-way chunked `view.get<T>()` per element (strictly more per-element work than the other two libs' columnar walk — disclosed, never compared as a 3-way op) |

### Per-family verdicts

- **Creation/structural** (create, create_batch, add_component, remove_component, add_batch,
  remove_batch, destroy): no library wins every op. Astra's strongest lane is per-entity
  single create/add/remove against **flecs** (1.16-1.68× ahead, all non-overlapping). Astra's
  weakest lane is **batch/bulk paths against both competitors**: create_batch and destroy both
  flip — Astra beats entt on these two (1.32×/1.81×) but loses to flecs (0.50×/0.56×); add_batch
  and remove_batch lose to entt decisively (entt's uniform-value bulk-insert idiom is close to a
  raw memmove — a hard bar for any archetype-based ECS, disclosed by Tasks 2/3 as the honest
  "what a user would write" idiom, not a simplified shortcut) while sitting at parity-to-ahead
  vs flecs's deferred-loop idiom. Net: 4 real tuning targets here (create_batch, destroy,
  add_component, remove_component, remove_batch — see the explicit list below), each isolated
  to a specific competitor, not a blanket structural weakness.
- **Iteration** (iterate1/2/3/5, iterate2_half/one): vs flecs, **every op is within noise**
  (overlapping bands) except parallel_iterate2@10M — this is a direct, larger-scale
  confirmation of the perf-optimization program's standing conclusion ("both candidate
  iteration-lever mechanisms disproven... iterate2/3 measuring at parity-to-noise"), not a new
  finding. vs entt, Astra is decisively ahead on iterate5 (3.3-3.5×) and iterate2_half (4.45×,
  the archetype-vs-pool-intersection asymmetry the op exists to expose) and iterate1@10M
  (1.29×); iterate2_one sits at/below the chrono timer's resolution floor for all three
  libraries and isn't a meaningful signal at any scale tested.
- **Random-access** (random_get, get_multi): **random_get vs flecs is now essential parity**
  (56.95 vs 56.76ns, 0.997×, overlapping) — the strongest confirmation yet that the 2026-07-24
  Lever-1 (random_get record→chunk-pointer) program closed the historical ~10% Astra-behind-
  flecs gap at full campaign scale. get_multi vs flecs is a genuine tuning target (0.87×,
  non-overlapping, ~14% behind). Both ops vs entt show noise (overlapping bands, inflated by
  the round-3 spike) despite large median gaps in entt's favor — honestly withheld per the
  non-overlapping-band rule rather than asserted from medians alone.
- **Relations (Tier B)**: mixed. relations_descendants is Astra's clearest win in the entire
  campaign (6.68×) — direct evidence the cached-traversal design decision pays off against
  flecs's recursive walk. relations_ancestors is a real, non-overlapping loss (0.63×).
  relations_children is also flagged non-overlapping but at a magnitude thin enough (sub-2μs
  total measured region) that it's reported as a weak/directional signal, not a confident
  regression.
- **Scheduler/parallel (Tier B)**: system_tick_seq and parallel_iterate2@1M are both
  within-noise parity vs flecs; parallel_iterate2@10M is a small but non-overlapping Astra win
  (1.05×). No tuning targets in this family.

### Remaining comparative tuning targets (non-overlapping bands, Astra behind)

```
create_batch        @ N=1,000,000   vs flecs  astra 34.06 [33.42,36.85]  flecs 17.17 [16.52,20.71]   (astra +98.4%)
destroy              @ N=1,000,000   vs flecs  astra 26.14 [25.84,27.23]  flecs 14.58 [14.06,15.36]   (astra +79.3%)
get_multi            @ N=1,000,000   vs flecs  astra 114.31[109.12,142.79] flecs 99.90[95.37,105.26]  (astra +14.4%)
relations_ancestors  @ N=100,000     vs flecs  astra 122.33[115.81,125.35] flecs 76.96[75.30,84.07]   (astra +59.0%)
relations_children   @ N=100,000     vs flecs  astra 0.0160[0.0150,0.0160] flecs 0.0090[0.0090,0.0130] (astra +77.8%, thin-magnitude signal)
add_batch            @ N=1,000,000   vs entt   astra 80.26 [79.30,81.51]  entt 14.56 [13.76,20.95]    (astra +451.2%, entt bulk-uniform idiom)
add_component        @ N=1,000,000   vs entt   astra 38.77 [38.36,40.25]  entt 15.90 [14.95,22.48]    (astra +143.8%)
remove_batch         @ N=1,000,000   vs entt   astra 62.45 [59.49,64.43]  entt 14.78 [14.48,17.32]    (astra +322.6%, entt bulk-uniform idiom)
remove_component     @ N=1,000,000   vs entt   astra 26.29 [25.49,27.38]  entt 16.22 [16.00,20.01]    (astra +62.1%)
```

No iteration op, no random-access op, and no scheduler/parallel op appears in this list — every
non-overlapping loss is confined to the structural (create_batch/destroy/add/remove) and
relations families. `random_get` — the prior program's single largest open item — is fully
resolved (parity vs flecs, noise vs entt).

### 19-of-36 shipped-suite mapping

19 of Astra's 36 shipped `benchmark/Benchmark.cpp` cases are mirrored here (6 creation/
structural + 6 iteration shapes + 1 of 6 parallel + 2 gets + 3 of 5 relations + 1 of 6
scheduler), **plus 1 new op (`destroy`)** the shipped suite itself lacks — all 20 measured
successfully across the full 6-round campaign with clean items-parity throughout. 17 cases
remain excluded as Astra-internal or unpairable:

- **range-for family** — Astra-internal API comparison; its cross-lib equivalents are already
  the `iterate*` ops.
- **ForEachLink** — no flecs analog chosen; custom relation pairs exist but a fair shape needs
  its own design.
- **SystemScheduler Parallel/ManyIndependent/WithDependencies/CustomExecutor variants** —
  scheduler-internal shapes with no flecs mapping beyond `system_tick_seq` + `parallel_iterate2`.
- **ParallelForEachDescendant** — a compound of two already-disclosed Tier-B caveats
  (traversal-strategy difference + parallel-dispatch difference); no single fair pairing.

### Reproduce (this section)

`bench-compare/bench_{astra,flecs,entt}.cpp` + `bench_common.hpp` (untracked scratch, per
spec Sec.5.3) implement the full op matrix; `bench-compare/analyze_definitive.py` (untracked)
performs the items cross-check + median/band computation + ratio tables + tuning-target list
consumed above. Raw campaign data: `bench-compare/definitive_campaign.csv` (432 rows, untracked).

## Lever 3 — validate-once destroy + chunk-run batch create (2026-07-24, branch `perf/lever3-create-destroy`)

Design: `docs/superpowers/specs/2026-07-24-lever3-create-batch-destroy-design.md` (`73faa67`). Plan:
`docs/superpowers/plans/2026-07-24-lever3-create-batch-destroy.md` (`8249369`). Goal: attack the
Definitive Scoreboard's two structural-batch tuning targets — `destroy` (0.56× flecs) and
`create_batch` (0.50× flecs) — the two never-optimized bulk/teardown paths the prior levers (1: random_get
chunk-pointer, 2: remove-path) didn't touch.

**Branch built on dev @ `5aa9df9` (the definitive-scoreboard commit). Commits, in order:**
- `13adc0d` — Task 1 characterization tests (destroy behavior table + destroy-with-relations guard,
  pass on unmodified code as required for a behavior-preserving refactor).
- `06a757c` — **Task 1 impl: validate-once `DestroyEntity`.** Single `GetEntityRecord` fetch feeds an
  explicit `IsSignalEnabled` gate, a record-taking `ArchetypeManager::RemoveEntity`/
  `EntityManager::Destroy` overload pair, and a new `RelationshipGraph::Empty()` early-out that skips
  `OnEntityDestroyed` entirely for entities with no parent/children/links.
- `cc57374` — **fix (found in whole-task review): `Empty()` must also count the traversal caches.**
  `GetDescendantsCached`/`GetAncestorsCached` insert a permanent per-entity cache entry even on a
  zero-result query; the original `Empty()` only checked `m_parents`/`m_children`/`m_links`, so an
  entity with a cache entry but no live relations made `Empty()` return `true` and the fast path
  orphaned that cache entry forever (unbounded growth via ordinary create→query→destroy cycles).
  Broadened to `m_parents.Empty() && m_children.Empty() && m_links.Empty() &&
  m_descendantCaches.Empty() && m_ancestorCaches.Empty()` — all `FlatMap`, all O(1) `Empty()`, no new
  probe cost. Regression test added (`RelationsDestroyGuard.CachedTraversalOnUnrelatedEntityKeepsGraphNonEmpty`).
- `442208d` — Task 2 characterization tests (batch-create chunk-boundary values + move-only lifetime
  balance, pass on unmodified code).
- `227f48c` — **Task 2 impl: chunk-run bulk path for `Archetype::AddEntitiesWith`.** Rewrote the old
  entity-major loop (per-entity `GetOrCreateChunk` + per-element `idToColumn`-resolve +
  fn-ptr-dispatched `ConstructComponentAt`) into a chunk-run-major loop: one `GetOrCreateChunk` +
  one bulk `GetEntities().insert(...)` + one `SetCount(...)` per *run* of entities landing in the same
  chunk, typed column base pointers hoisted once per run, and each entity's tuple move-constructed
  straight through the hoisted typed pointer (one move, no `idToColumn`/fn-ptr indirection). Paired
  with an `ArchetypeManager::AddEntitiesWith` record-loop chunk hoist (derive the chunk pointer once
  per run instead of once per entity, writing through the existing 4-arg `SetRecordLocation` funnel).

All 5 commits local to `perf/lever3-create-destroy`, not pushed.

### Step 1 — authoritative 3-config confirmation

No code changed on the branch after Task 2's own 3-config run (`git status` at HEAD `227f48c` shows
only untracked `bench-compare/` scratch files and untracked review docs — no tracked-file diff), so
Task 2's run stands as authoritative without a re-run:

| Config | Total | Passed | Notes |
|---|---|---|---|
| Debug | 742 | 742 | clean |
| Release | 740 | 740 | clean |
| Dist | 740 | 740 | clean |

742/740/740 = 739/737/737 (post-Task-1, post-`cc57374`-fix baseline) + 3 (Task 2's three
characterization tests — two `RegistryTest` cases plus the brief-mandated
`ArchetypeManagerTest.RecordChunkInvariant_BatchAddEntitiesWith` record-invariant loop). Zero
`Tracked::s_live` imbalance, zero `EntityRecord` chunk/location desync aborts in any config; the only
stderr line across every run was the pre-existing, known-expected `CircularHierarchyHandling`
cycle-detection assertion line.

### Checkpoint tables — same-session paired medians, N=1,000,000, ns/op, median [min, max] of 6 rounds

Baseline = Definitive Scoreboard (dev `5aa9df9`, a separate session — shown for absolute-value trend
only, not for ratio claims). ckpt1 = post-Task-1 (`.superpowers/sdd/task-1-report.md`, `lever3_ckpt1.csv`).
ckpt2 = post-Task-2 (`.superpowers/sdd/task-2-report.md`, `lever3_ckpt2.csv`). All ratio claims below use
**same-session** Astra/flecs pairs (flecs's own numbers drift a few percent session-to-session on this
machine — see prior sections — so only within-session pairs are trustworthy for a ratio).

**Primary targets:**

| Op | Baseline Astra / flecs / entt | ckpt1 Astra / flecs / entt | ckpt2 Astra / flecs / entt |
|---|---|---|---|
| **destroy** | 26.14 / 14.58 / 47.34 | **20.02 [19.24,22.04]** / 14.48 [14.11,16.25] / 47.49 [46.73,48.18] | **19.67 [19.34,20.20]** / 14.55 [13.94,14.65] / 48.22 [46.63,50.21] |
| **create_batch** | 34.06 / 17.17 / 44.85 | 32.81 [32.32,33.51] / 17.58 [16.69,27.54] / 42.81 [41.07,56.77] | **22.59 [21.49,23.68]** / 17.21 [16.06,17.96] / 46.44 [41.87,58.79] |

**Flat-watch ops (must stay within noise across both checkpoints — confirmed, see attribution below):**

| Op | Baseline Astra / flecs | ckpt1 Astra / flecs | ckpt2 Astra / flecs |
|---|---|---|---|
| create | 53.49 / 90.00 | 54.83 [52.73,64.81] / 91.52 [89.08,97.02] | 53.21 [52.35,55.04] / 90.83 [89.03,94.74] |
| add_component | 38.77 / 50.24 | 39.63 [39.00,41.26] / 50.77 [50.17,59.15] | 39.84 [39.01,41.37] / 50.49 [49.57,50.86] |
| remove_component | 26.29 / 32.89 | 26.41 [26.15,26.61] / 33.53 [31.94,49.05] | 26.04 [25.75,27.67] / 32.89 [32.40,34.46] |
| random_get | 56.95 / 56.76 | 53.86 [52.98,64.24] / 55.79 [53.24,64.25] | 55.82 [50.98,58.17] / 55.28 [50.58,56.76] |

### Per-sub-lever attribution

- **destroy — attributed to Task 1 (validate-once + empty-graph early-out).** Baseline→ckpt1:
  26.14→20.02 ns (−23.4%). **Caveat, disclosed per the brief: ckpt1's destroy bench predates
  `cc57374`** — the checkpoint-1 build ran validate-once destroy with the *original*, narrower
  `Empty()` (parents/children/links only, not the traversal caches), i.e. it measured Task 1's raw
  perf shape before the correctness fix landed. ckpt2 (post-`cc57374`, post-Task-2, destroy code
  itself untouched by Task 2) measured 19.67 ns — **flat versus ckpt1 (−1.7%, within the checkpoint
  tables' own round-to-round noise band)**, which is the direct evidence that broadening `Empty()`
  to also check two more `FlatMap`s cost effectively nothing (both maps are already O(1)
  `Size()`-backed `Empty()` — no new probe was added, only two more O(1) reads on the already-hot
  early-out check). Net: the whole of destroy's win is Task 1's, and the `cc57374` correctness fix
  is confirmed cost-neutral by ckpt2, not merely assumed.
- **create_batch — attributed to Task 2 (chunk-run bulk path).** ckpt1→ckpt2: 32.81→22.59 ns
  (−31.1%), a large, decisive, non-noise drop (flecs's own ckpt1/ckpt2 medians — 17.58 and 17.21 —
  stayed in the same band across both sessions, so the drop is attributable to the code change, not
  the environment). create_batch was untouched by Task 1 (baseline→ckpt1: 34.06→32.81, −3.7%, noise),
  confirming clean separation between the two sub-levers.
- **Flat-watch ops all held within session noise across both checkpoints** — create, add_component,
  remove_component, and random_get show no directional drift attributable to either sub-lever
  (each delta is a low single-digit percentage, well inside the round-to-round spread already visible
  within a single checkpoint's own 6 rounds). No regression anywhere.

### Honest verdict vs both targets

Neither sub-lever reached full parity with its flecs target — both delivered a large, real,
non-noise fraction of the gap and stopped short, for the same underlying reason: each optimized the
*bulk/chunk* machinery but left *per-entity* overhead on the path unaddressed (out of each task's
scoped brief).

- **destroy: target ~14.6 ns — NOT reached.** Landed at ~20.02 ns (−23.4% from baseline, confirmed
  cost-neutral through the `cc57374` fix by ckpt2's flat 19.67 ns). Remaining gap to flecs ≈ **1.38×**
  (20.02 / 14.48, same-session ckpt1 pair). Task 1's scope was strictly the `Registry::DestroyEntity` seam (validate-once +
  early-out) — it did not touch `Archetype::RemoveEntity`'s own per-entity chunk-erase/backfill cost,
  which the task's own report already flagged as "the remaining dominant term."
- **create_batch: target ~17.2 ns — NOT reached.** Landed at 22.59 ns (−31% from ckpt1, −33.7% from
  baseline). Remaining gap to flecs ≈ **1.31×** (22.59 / 17.21, same-session ckpt2 pair). Task 2's chunk-run rewrite eliminated
  the *per-element* `idToColumn`/fn-ptr resolution and the *per-run* chunk/record derivation, but three
  terms remain squarely outside chunk-run's scope: the mandatory **per-entity `generator(...)` call**
  (the API contract is exactly one call per entity — cannot be batched away), the **per-entity
  `GetOrCreateRecord`** paged-table lookup in the `ArchetypeManager` record loop (only the *chunk
  pointer* derivation was hoisted per-run, not the record lookup itself), and **`GetOrCreateChunk` per
  run** (once per chunk boundary, not per entity, but still not eliminated).

Both sub-levers are correctly scoped, honestly reported wins, not disguised failures: destroy closed
just over half of the baseline-to-target distance ((26.14−20.02)/(26.14−14.58) ≈ 53%),
create_batch closed roughly two-thirds ((34.06−22.59)/(34.06−17.17) ≈ 68%) — real progress that
stopped at each task's deliberately scoped boundary, not at a measurement or implementation ceiling.

### Scoreboard-delta summary

Definitive Scoreboard tuning-target ratios, recomputed from the same-session ckpt2 pairs (Astra speed
relative to flecs — flecs_time / astra_time; >1 = Astra ahead, <1 = Astra behind, matching the
scoreboard's own convention):

| Tuning target | Baseline ratio (vs flecs) | ckpt2 ratio (vs flecs, same-session) | Status |
|---|---|---|---|
| **create_batch** | 0.50× (17.17/34.06) | **~0.76× (17.21/22.59)** | Narrowed materially, still open — moved from "half of flecs's throughput" to "three-quarters" |
| **destroy** | 0.56× (14.58/26.14) | **~0.74× (14.55/19.67)** | Narrowed materially, still open — same shape as create_batch |

Both targets moved a large, real amount and neither closed. The three other Definitive Scoreboard
structural-vs-flecs targets this lever did not touch remain exactly where the scoreboard left them,
open:

- `get_multi` @ 1M — astra 114.31 vs flecs 99.90 ns (0.87×, +14.4% astra-slower) — open.
- `relations_ancestors` @ 100K nodes — astra 122.33 vs flecs 76.96 ns (0.63×, +59.0% astra-slower) — open.
- `relations_children` @ 100K nodes — astra 0.0160 vs flecs 0.0090 ns (0.56×, +77.8% astra-slower,
  thin-magnitude signal per the scoreboard's own caveat) — open.

(The scoreboard's remaining entt-side targets — add_batch, add_component, remove_batch,
remove_component vs entt — are architecture-inherent per the scoreboard's own analysis, not lever
candidates, and are unaffected by this lever.)

### Files changed

- `include/Astra/Registry/Registry.hpp` — `DestroyEntity` rewritten (validate-once + signal gate +
  empty-graph early-out).
- `include/Astra/Archetype/ArchetypeManager.hpp` — new `RemoveEntity(Entity, EntityRecord*)` overload;
  `AddEntitiesWith` record-loop chunk hoist (4-arg funnel).
- `include/Astra/Entity/EntityManager.hpp` — new `Destroy(Entity, EntityRecord*)` overload.
- `include/Astra/Registry/RelationshipGraph.hpp` — new `Empty()` query (broadened in `cc57374` to
  count the traversal caches).
- `include/Astra/Archetype/Archetype.hpp` — `AddEntitiesWith` chunk-run rewrite; `#include <new>`.
- `tests/Registry/SignalLifetimeTest.cpp`, `tests/Registry/RelationsTest.cpp`,
  `tests/Registry/RegistryTest.cpp`, `tests/Registry/ArchetypeManagerTest.cpp` — characterization +
  regression tests (6 total: 2 destroy-path, 1 relations-cache regression, 3 create_batch-path).

## Change detection (Stage 3, 2026-09-11, branch `feat/change-detection` @ `ec5f6c1`)

Merge-gate evidence for the change-detection feature (spec
`docs/superpowers/specs/2026-09-10-astra-change-detection-design.md`, Task 9): a flat-watch
table (does the feature regress the 11 pre-existing ops it must stay zero-cost against) plus
Astra-only `Changed<T>` throughput at 0/10/50/100% changed, coarse (untracked) vs precise
(tracked) tier.

**Recipe** (same Dist-parity full-opt flags as every table above):
`/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc
/DASTRA_BUILD_DIST /I..\include /I..\vendor\Mosaic\include /I..\tests bench_astra.cpp /link /LTCG
advapi32.lib` (the `advapi32.lib` link is the known huge-pages-probe/`advapi32` gotcha carried by
every Astra bench/test binary).

**Setup.** BASELINE: a scratch worktree at `4ac19ff` (the commit the Stage 3 plan was written
against; `f3e311d` on top of it is docs-only, no code) with the PRE-EDIT `bench_astra.cpp` (no
`run_change_detection()`, no `random_get_const` — copied before this task touched the file) +
`bench_common.hpp` + an adjusted `build_one.bat`. BRANCH: the working tree at `ec5f6c1` plus this
task's bench-only edits (untracked `bench_astra.cpp`). Both binaries built fresh from the
identical recipe above, one after the other, immediately before the campaign.

**Quiet check.** `typeperf "\Processor(_Total)\% Processor Time" -sc 5` read 15–19% both
immediately before and immediately after the 6-round campaign (steady, not a transient spike —
`Get-Process | sort CPU -desc` attributed it to the user's own background apps: a running game,
Discord, a browser, none of which this task is authorized to close). This is above the ~10%
target; retried the quiet check twice a few seconds apart with no improvement, and proceeded
because (a) the load was steady across the whole campaign rather than concentrated in one round,
and (b) the interleaved baseline→branch→baseline… protocol exposes both binaries to the same
background conditions within every round, so a paired comparison stays meaningful even though the
absolute numbers carry more noise than the ~10%-quiet sessions elsewhere in this file. Flagged
here as a caveat on every row below, not hidden.

**6 interleaved rounds** (baseline → branch, ×6), N = 1,000,000 unless noted. Raw rows:
`bench-compare/cd_gate.csv` (untracked), `roundN,{baseline|branch},astra,<op>,<N>,<ns>,<items>`.

| Op | N | Baseline median [min, max] (ns) | Branch median [min, max] (ns) | Overlap |
|---|---|---|---|---|
| create | 1M | 56.430 [55.810, 58.822] | 57.837 [56.876, 59.999] | YES |
| create_batch | 1M | 22.566 [22.009, 23.559] | 22.952 [22.375, 24.148] | YES |
| add_component | 1M | 42.560 [41.766, 43.480] | 51.832 [51.477, 58.381] | **NO** |
| remove_component | 1M | 28.014 [27.718, 29.012] | 29.566 [29.065, 29.854] | **NO** (marginal — 0.05 ns gap) |
| destroy | 1M | 20.722 [20.340, 21.454] | 20.569 [20.289, 20.724] | YES |
| random_get | 1M | 59.159 [57.268, 60.215] | 75.981 [72.253, 83.762] | **NO** (expected, see below) |
| random_get_const | 1M | — | 58.947 [58.112, 60.228] | YES vs. baseline `random_get` |
| iterate1 | 1M | 0.459 [0.447, 0.489] | 0.378 [0.369, 0.418] | **NO** (branch faster) |
| iterate1 | 10M | 0.716 [0.697, 0.727] | 0.716 [0.688, 0.753] | YES |
| iterate2 | 1M | 0.907 [0.869, 1.037] | 1.012 [0.883, 1.287] | YES |
| iterate2 | 10M | 1.208 [1.165, 1.309] | 1.255 [1.142, 1.399] | YES |
| iterate3 | 1M | 0.957 [0.882, 0.991] | 1.036 [0.935, 1.212] | YES |
| iterate3 | 10M | 1.329 [1.280, 1.396] | 1.343 [1.335, 1.402] | YES |
| parallel_iterate2 | 1M | 0.253 [0.235, 0.390] | 0.291 [0.236, 0.451] | YES |
| parallel_iterate2 | 10M | 0.867 [0.862, 0.871] | 0.853 [0.849, 0.886] | YES |
| system_tick_seq | 1M | 1.612 [1.553, 1.992] | 1.732 [1.472, 1.890] | YES |

**Verdicts, per the "record as measured, don't fix here" rule (spec Task 9 Step 4):**

- **`random_get` — NO, expected.** The non-const `GetComponent<T>` now stamps the entity's chunk
  version (spec §3.3 row 3), one plain store per call it did not pay before. `random_get_const`
  (branch, `std::as_const(*reg).GetComponent<T>`) measures 58.947 [58.112, 60.228], which
  **overlaps the baseline's un-instrumented `random_get`** (59.159 [57.268, 60.215]) — confirming
  the entire gap is the mandated stamp and nothing else moved. Consumers that only read should use
  the const accessor or a const view, exactly as the spec recommends.
- **`add_component` — NO, real, not flagged as "expected sensitive" in the task brief.**
  `AddComponent<Health>` is a cross-archetype move ({Position,Velocity} → {Position,Velocity,
  Health}); per spec §3.3 every carried column of the destination chunk is stamped on
  add/emplace/set, not just the new one (matches the shipped, tested behavior in
  `ChangeDetectionChunkVersion.AddAndRemoveComponentStampTheDestinationChunk`). Baseline pays 0
  stamp stores on this path; branch pays 3 (Position, Velocity, Health) plus the tracked-column
  gate. Median moved 42.560 → 51.832 ns/entity (+21.8%), clearly outside the baseline band. This
  is architecturally expected given the spec's write-side table, but the *magnitude* was not
  independently verified against a "within noise" claim anywhere in the plan — recorded here
  exactly as measured for the controller to weigh, not adjusted.
- **`remove_component` — NO, marginal.** Same move-stamps-destination mechanism, smaller (2
  carried columns). Gap is 0.053 ns between the baseline's max (29.012) and the branch's min
  (29.065) — bands touch almost exactly at the boundary. Recorded as a real non-overlap per the
  literal rule, but an order of magnitude tighter than `add_component`'s.
- **`iterate1` @ 1M — NO, branch *faster*, likely noise.** 0.378 vs. 0.459 ns/entity median; at
  10M scale the same op overlaps cleanly (0.716 vs. 0.716, effectively identical). `iterate1`'s
  chunk-stamp shape (one store per visited chunk for a non-const single-component view) doesn't
  change between baseline and branch for an all-untracked archetype — `ArchetypeColumnMeta::
  trackedColumnCount == 0` makes the tracked-column loop a single false compare either way — so a
  *speedup* at 1M with parity at 10M (12× the chunks, far less per-call noise) reads as sampling
  noise under the ~17%-load quiet-check caveat above, not a structural change. Recorded as
  measured, not discarded.
- Every other flat-watch op (`create`, `create_batch`, `destroy`, `iterate2`, `iterate3` at both
  scales, `parallel_iterate2` at both scales, `system_tick_seq`) overlaps.

### `Changed<T>` throughput (branch only, median [min, max] of the same 6 rounds)

`run_cd_variant<Vel>()` marks the first `pct%` of entities in creation order (clustered, matching
the acceptance test's shape) **inside the measured region deliberately** — the throughput number
therefore includes the marking pass itself, the same shape the pre-implementation spike measured
(`bench-compare/spike-changeticks/spike.cpp`), not an isolated read-only iteration cost.
`cd_untracked_*` uses plain `Velocity` (coarse tier only, chunk-granular reject); `cd_tracked_*`
uses a file-local `TrackedVelocity` (`AstraChangeTracked = true`; coarse reject **and** the
per-entity run-scan). N = 1,000,000, reps = 7.

| Op | % changed | Median [min, max] (ns/entity) |
|---|---|---|
| `cd_untracked_changed_0` | 0% | 0.000 [0.000, 0.000] |
| `cd_untracked_changed_10` | 10% | 0.549 [0.542, 0.581] |
| `cd_untracked_changed_50` | 50% | 3.041 [2.912, 3.085] |
| `cd_untracked_changed_100` | 100% | 6.074 [5.932, 6.343] |
| `cd_tracked_changed_0` | 0% | 0.000 [0.000, 0.000] |
| `cd_tracked_changed_10` | 10% | 0.818 [0.794, 0.853] |
| `cd_tracked_changed_50` | 50% | 4.361 [4.234, 5.112] |
| `cd_tracked_changed_100` | 100% | 8.856 [8.580, 9.051] |

At 0% changed, the chunk-reject fast path costs nothing measurable at either tier. At 100%, the
tracked tier costs ~46% more than the coarse tier (8.856 vs. 6.074 ns/entity) — the expected price
of the per-entity run-scan running (and finding everything newer) on top of the coarse chunk
accept, exactly the two-tier trade-off the spec's Decision 2 describes.

## 3-way campaign (2026-09-11, dev+change-detection @ b664aa8)

Record-refresh campaign: the last 3-way run of record was 2026-07-24/25 (Definitive scoreboard +
Lever 3); `include/` has since gained the full 3-stage system-queries program (View enrichment,
SystemParam binder) and, most recently, Stage 3 change detection (`b664aa8`, branch
`feat/change-detection` = dev + change detection). This section is a pure evidence refresh — numbers
only, no tuning, no fixes — run on the current tree exactly as instructed.

**Recipe** (identical Dist-parity full-opt flags as every table above):
`/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc
/nologo /DASTRA_BUILD_DIST /I..\include /I..\vendor\Mosaic\include /I..\tests bench_astra.cpp
/Fe:bench_astra.exe /link /LTCG advapi32.lib`, invoked through `build_one.bat` (vcvars64 + `cl`).
`bench_astra.exe` rebuilt fresh from current `include/` (previous exe dated 2026-07-25); build
log (`build_astra.log`) shows zero warnings/errors; one untimed smoke run (34 rows, exit 0, not
counted as a round). `bench_flecs.exe`/`bench_entt.exe` **reused as-is** (built 2026-07-24
18:36) — verified by mtime: `vendor/flecs.c`/`vendor/flecs.h`/`vendor/entt.hpp` all
2026-07-21 19:12, older than both exes (2026-07-24 18:36), so no rebuild triggered per the task's
mtime rule. Both also smoke-run once clean (25 and 22 rows respectively, exit 0). A round-0
smoke cross-check (`analyze_definitive.py` on all three fresh smoke outputs) passed the items
check with zero mismatches and reproduced the same data-driven op→libs grouping as the July
campaign (15 3-way ops, 6 2-way ops, the rest 1-way-exempt) before the real campaign ran.

### Quiet-check — GATE NEVER CLEARED (proceeded per ground rules)

`typeperf "\Processor(_Total)\% Processor Time" -sc 5`, 5 pre-campaign attempts (~2 min apart,
the instructed retry loop) plus one post-campaign check; none dropped near the ~10% target:

| Attempt | Time | Samples (%) | Avg |
|---|---|---|---|
| 1 (pre) | 12:42:59-12:43:03 | 35.29, 25.91, 26.63, 27.38, 36.48 | **30.34%** |
| 2 (pre) | 12:45:36-12:45:40 | 20.85, 25.31, 18.05, 20.25, 22.50 | **21.39%** |
| 3 (pre, right after the astra rebuild) | 12:46:58-12:47:03 | 35.71, 34.79, 41.21, 43.93, 42.18 | **39.57%** |
| 4 (pre) | 12:49:08-12:49:12 | 33.00, 31.51, 28.13, 25.80, 22.20 | **28.13%** |
| 5 (pre, last allowed retry) | 12:51:19-12:51:23 | 41.36, 37.07, 32.31, 36.35, 49.77 | **39.37%** |
| post-campaign (after round 6) | 12:56:08-12:56:13 | 26.68, 26.54, 27.64, 26.06, 27.48 | **26.88%** |

Per ground rules ("if it never drops below ~12%, proceed anyway, record every reading, name the
offending processes, don't kill anything") the campaign ran anyway. `Get-Process | sort CPU -desc`
named the same offenders both times checked: **node** (a long-running background process, by far
the largest cumulative-CPU total), **Discord** (×2 processes), **brave** (×2 processes),
**steamwebhelper** (×2 processes), **ArcaneEditor**. None were closed.

**Caveat on the post-round-3 checkpoint:** the campaign ran as one unattended script (all 6
rounds back-to-back); no discrete typeperf sample was taken specifically between rounds 3 and 4,
only the pre-campaign (attempt 5, immediately before round 1) and post-campaign (immediately
after round 6) readings above. The per-round wall-clock times below show no anomalous slowdown
around round 3 specifically — offered as the closest available evidence of in-campaign
stability, not a substitute for the missed literal measurement.

### Campaign

6 interleaved rounds, `astra → flecs → entt` per round, each exe's stdout prefixed `roundN,` and
appended to `bench-compare/campaign_2026-09-11.csv` (486 rows = 6 × 81; stderr →
`campaign_2026-09-11.err`, only the expected `# <lib> sink=...` debug lines, zero failures).
Wall time per round: round1 46.1s, round2 47.0s, round3 40.5s, round4 40.3s, round5 41.6s,
round6 40.4s (total ≈256s). `python analyze_definitive.py campaign_2026-09-11.csv` — **items
check PASS, zero mismatches**, same op→libs grouping as the round-0 smoke check; full output
saved to `campaign_2026-09-11_analysis.txt`.

### Tier-A table (3-way; median [min,max] ns/op, 6 rounds; N=1,000,000 unless noted)

| op | N | Astra | flecs | EnTT | vs flecs | vs EnTT |
|---|---|---|---|---|---|---|
| create | 1M | 70.427 [60.050, 87.795] | 115.37 [106.28, 135.54] | 62.349 [56.244, 68.125] | 1.638× NON-OVERLAPPING (astra ahead) | 0.885× overlap/noise |
| create_batch | 1M | 30.675 [27.623, 39.927] | 22.997 [21.249, 28.880] | 56.057 [51.382, 72.371] | 0.750× overlap/noise | 1.827× NON-OVERLAPPING (astra ahead) |
| add_component | 1M | 52.755 [48.480, 64.785] | 63.357 [60.268, 71.173] | 19.690 [16.604, 21.586] | 1.201× overlap/noise | 0.373× NON-OVERLAPPING (astra behind) |
| remove_component | 1M | 32.758 [30.964, 38.287] | 39.931 [37.135, 42.847] | 17.939 [17.342, 18.458] | 1.219× overlap/noise | 0.548× NON-OVERLAPPING (astra behind) |
| add_batch | 1M | 113.11 [99.909, 134.07] | 109.28 [106.56, 125.07] | 19.279 [16.519, 22.392] | 0.966× overlap/noise | 0.170× NON-OVERLAPPING (astra behind) |
| remove_batch | 1M | 88.559 [78.387, 124.64] | 81.542 [72.434, 90.489] | 15.526 [15.191, 15.869] | 0.921× overlap/noise | 0.175× NON-OVERLAPPING (astra behind) |
| destroy | 1M | 24.983 [23.000, 29.938] | 15.890 [14.985, 19.003] | 50.435 [49.395, 54.587] | 0.636× NON-OVERLAPPING (astra behind) | 2.019× NON-OVERLAPPING (astra ahead) |
| iterate1 | 1M | 0.9045 [0.4460, 1.944] | 0.7610 [0.4100, 1.713] | 1.047 [0.7120, 2.452] | 0.841× overlap/noise | 1.158× overlap/noise |
| iterate1 | 10M | 1.268 [1.022, 1.769] | 1.195 [1.031, 1.460] | 1.497 [1.263, 2.024] | 0.942× overlap/noise | 1.180× overlap/noise |
| iterate2_half | 1M (items=500K) | 0.7160 [0.3970, 1.418] | 0.3875 [0.2560, 1.304] | 2.262 [1.724, 3.033] | 0.541× overlap/noise | 3.159× NON-OVERLAPPING (astra ahead) |
| iterate2_one | 1M (items=1) | 0.000 | 0.000 | 0.000 | overlap/noise | overlap/noise (all three ~0) |
| iterate5 | 1M | 2.660 [2.183, 4.535] | 2.720 [1.977, 3.399] | 6.258 [5.770, 7.488] | 1.023× overlap/noise | 2.353× NON-OVERLAPPING (astra ahead) |
| iterate5 | 10M | 2.702 [2.243, 3.953] | 2.371 [1.994, 3.692] | 6.585 [5.882, 10.698] | 0.877× overlap/noise | 2.437× NON-OVERLAPPING (astra ahead) |
| random_get | 1M | 150.91 [122.03, 204.07] | 99.251 [88.546, 123.14] | 56.184 [49.805, 89.025] | 0.658× overlap/noise | 0.372× NON-OVERLAPPING (astra behind) |
| get_multi | 1M (items=2M) | 223.18 [183.50, 303.34] | 155.09 [140.68, 209.60] | 108.00 [91.137, 119.99] | 0.695× overlap/noise | 0.484× NON-OVERLAPPING (astra behind) |

`iterate2`/`iterate3` are 2-way (astra/flecs; EnTT reports them under `iterate2_view`/
`iterate3_view`, informational only, same treatment as the July campaign):

| op | N | Astra | flecs | ratio | flag |
|---|---|---|---|---|---|
| iterate2 | 1M | 2.155 [1.752, 3.000] | 1.963 [1.409, 3.138] | 0.911× | overlap/noise |
| iterate2 | 10M | 2.173 [1.563, 2.545] | 1.813 [1.486, 2.620] | 0.834× | overlap/noise |
| iterate3 | 1M | 2.098 [1.854, 3.038] | 2.295 [1.775, 3.164] | 1.094× | overlap/noise |
| iterate3 | 10M | 2.097 [1.605, 2.701] | 2.061 [1.811, 3.674] | 0.983× | overlap/noise |

### Tier-B table (Astra vs flecs only; median [min,max] ns/op, 6 rounds)

| op | N | Astra | flecs | ratio | flag |
|---|---|---|---|---|---|
| relations_children | 100K nodes (items=100) | 0.0190 [0.0190, 0.0200] | 0.0100 [0.0090, 0.0100] | 0.526× | NON-OVERLAPPING (astra behind) |
| relations_descendants | 100K nodes (items=99,999) | 10.869 [10.477, 12.025] | 62.378 [58.633, 141.11] | 5.739× | NON-OVERLAPPING (astra ahead) |
| relations_ancestors | 100K nodes (items=448,889) | 220.47 [196.45, 272.06] | 86.451 [84.086, 93.369] | 0.392× | NON-OVERLAPPING (astra behind) |
| system_tick_seq | 1M × 3 systems (items=3M) | 3.149 [2.278, 5.509] | 3.312 [2.679, 4.585] | 1.052× | overlap/noise |
| parallel_iterate2 | 1M | 0.5180 [0.3810, 0.7250] | 0.4290 [0.2490, 0.8630] | 0.828× | overlap/noise |
| parallel_iterate2 | 10M | 0.9910 [0.9700, 1.053] | 1.041 [0.9860, 1.214] | 1.050× | overlap/noise |

**Tuning-target list** (script-generated, Astra behind with non-overlapping bands): `destroy`
(vs flecs, +57.2%), `relations_ancestors` (vs flecs, +155.0%), `relations_children` (vs flecs,
+90.0%), `add_batch` (vs entt, +486.7%), `add_component` (vs entt, +167.9%), `get_multi` (vs
entt, +106.6%), `random_get` (vs entt, +168.6%), `remove_batch` (vs entt, +470.4%),
`remove_component` (vs entt, +82.6%). These percentages are same-session, paired-ratio numbers
(reliable per the campaign's own load-noise caveat below) — they should not be read against the
July percentages, which ran at a different, much quieter load.

### `random_get` vs `random_get_const` against both competitors' `random_get`

`random_get_const` is new (Stage-3 addition, `std::as_const(*reg).GetComponent<T>` — no chunk
stamp). Not part of the script's automated op-name matching (1-way exempt); computed by hand with
the identical band-overlap rule:

| op | Astra median [min,max] | vs flecs random_get 99.251 [88.546, 123.14] | vs entt random_get 56.184 [49.805, 89.025] |
|---|---|---|---|
| `random_get` (non-const) | 150.91 [122.03, 204.07] | 0.658× overlap/noise | 0.372× NON-OVERLAPPING (astra behind) |
| `random_get_const` | 114.37 [106.55, 155.44] | 0.868× overlap/noise | 0.491× NON-OVERLAPPING (astra behind) |

### Today vs 2026-07-24 (Astra medians; prior-source column states which section the number is from)

| op | N | Today (2026-09-11) | 2026-07-24 Definitive | 2026-07-24 Lever-3 | Today vs July band |
|---|---|---|---|---|---|
| create | 1M | 70.427 [60.050, 87.795] | 53.49 [51.99, 55.96] | — | outside (above) |
| create_batch | 1M | 30.675 [27.623, 39.927] | 34.06 [33.42, 36.85] | 22.59 [21.49, 23.68] (ckpt2) | outside both (between them) |
| add_component | 1M | 52.755 [48.480, 64.785] | 38.77 [38.36, 40.25] | 39.84 [39.01, 41.37] (ckpt2 flat-watch) | outside (above) |
| remove_component | 1M | 32.758 [30.964, 38.287] | 26.29 [25.49, 27.38] | 26.04 [25.75, 27.67] (ckpt2 flat-watch) | outside (above) |
| add_batch | 1M | 113.11 [99.909, 134.07] | 80.26 [79.30, 81.51] | — | outside (above) |
| remove_batch | 1M | 88.559 [78.387, 124.64] | 62.45 [59.49, 64.43] | — | outside (above) |
| destroy | 1M | 24.983 [23.000, 29.938] | 26.14 [25.84, 27.23] | 19.67 [19.34, 20.20] (ckpt2) | outside both (between them) |
| iterate1 | 1M | 0.9045 [0.4460, 1.944] | 0.461 [0.373, 0.539] | — | outside (above) |
| iterate1 | 10M | 1.268 [1.022, 1.769] | 0.765 [0.698, 0.843] | — | outside (above) |
| iterate2 | 1M | 2.155 [1.752, 3.000] | 0.980 [0.821, 1.437] | — | outside (above) |
| iterate2 | 10M | 2.173 [1.563, 2.545] | 1.171 [1.130, 1.281] | — | outside (above) |
| iterate3 | 1M | 2.098 [1.854, 3.038] | 1.123 [0.926, 1.404] | — | outside (above) |
| iterate3 | 10M | 2.097 [1.605, 2.701] | 1.292 [1.271, 1.380] | — | outside (above) |
| iterate5 | 1M | 2.660 [2.183, 4.535] | 1.450 [1.357, 1.632] | — | outside (above) |
| iterate5 | 10M | 2.702 [2.243, 3.953] | 1.542 [1.488, 1.902] | — | outside (above) |
| iterate2_half | 1M | 0.7160 [0.3970, 1.418] | 0.273 [0.250, 0.316] | — | outside (above) |
| iterate2_one | 1M | 0.000 | 0.000 | — | within (both at timer floor) |
| random_get | 1M | 150.91 [122.03, 204.07] | 56.95 [53.85, 59.33] | 55.82 [50.98, 58.17] (ckpt2 flat-watch) | outside (above) |
| random_get_const | 1M | 114.37 [106.55, 155.44] | new | new | n/a |
| get_multi | 1M | 223.18 [183.50, 303.34] | 114.31 [109.12, 142.79] | — | outside (above) |
| relations_children | 100K | 0.0190 [0.0190, 0.0200] | 0.0160 [0.0150, 0.0160] | — | outside (above) |
| relations_descendants | 100K | 10.869 [10.477, 12.025] | 7.325 [6.981, 7.489] | — | outside (above) |
| relations_ancestors | 100K | 220.47 [196.45, 272.06] | 122.33 [115.81, 125.35] | — | outside (above) |
| system_tick_seq | 1M×3 | 3.149 [2.278, 5.509] | 1.533 [1.504, 1.689] | — | outside (above) |
| parallel_iterate2 | 1M | 0.5180 [0.3810, 0.7250] | 0.288 [0.239, 0.391] | — | outside (above) |
| parallel_iterate2 | 10M | 0.9910 [0.9700, 1.053] | 0.850 [0.840, 0.854] | — | outside (above) |
| cd_untracked_changed_{0,10,50,100} | 1M | 0.000 / 0.6055 / 3.896 / 6.986 | new | new | n/a |
| cd_tracked_changed_{0,10,50,100} | 1M | 0.000 / 0.9160 / 5.019 / 9.702 | new | new | n/a |

Every op that existed in July moved outside its July band in the slower direction this session,
**except** `create_batch` and `destroy`, whose today's medians fall between the original 2026-07-24
Definitive-scoreboard band and the later same-day Lever-3-optimized band (i.e. still faster than
the pre-Lever-3 number, slower than the post-Lever-3 number), and `iterate2_one`, which stayed at
the timer's zero floor in both sessions.

### `Changed<T>` throughput today (Astra-only, median [min,max] of the same 6 rounds)

| Op | % changed | Median [min, max] (ns/entity) |
|---|---|---|
| `cd_untracked_changed_0` | 0% | 0.0000 [0.0000, 0.0000] |
| `cd_untracked_changed_10` | 10% | 0.6055 [0.5750, 0.7780] |
| `cd_untracked_changed_50` | 50% | 3.896 [3.440, 4.453] |
| `cd_untracked_changed_100` | 100% | 6.986 [6.784, 8.282] |
| `cd_tracked_changed_0` | 0% | 0.0000 [0.0000, 0.0010] |
| `cd_tracked_changed_10` | 10% | 0.9160 [0.8770, 1.422] |
| `cd_tracked_changed_50` | 50% | 5.019 [4.539, 5.396] |
| `cd_tracked_changed_100` | 100% | 9.702 [8.880, 11.002] |

### What changed since July — plain language, numbers only

Every 3-way and 2-way op moved outside its 2026-07-24 band this session, in the slower direction,
with two partial exceptions (`create_batch`, `destroy` — see table above) and one op that stayed
at the timer floor (`iterate2_one`). But this campaign never got a quiet machine: all 5
pre-campaign typeperf attempts read 21-40% average CPU (vs July's ~10%), and the offending
processes (`node`, `Discord`, `brave`, `steamwebhelper`, `ArcaneEditor`) were the user's own
background applications, not left running by this task. **flecs's own numbers — unmodified code,
rebuilt 2026-07-24, unchanged since — moved by comparable or larger margins this session**:
flecs `create` 90.00→115.37 (1.28×), flecs `iterate1`@1M 0.392→0.761 (1.94×), flecs `iterate2`@1M
0.904→1.963 (2.17×), flecs `random_get` 56.76→99.25 (1.75×), flecs `system_tick_seq`
1.579→3.312 (2.10×), flecs `relations_ancestors` 76.96→86.45 (1.12×), flecs
`relations_descendants` 48.94→62.38 (1.27×). Since flecs's code did not change between the two
sessions, this spread (1.1×-2.2×, heavier on short/tight ops like `iterate1`/`iterate2`/
`system_tick_seq`, lighter on `destroy`/`relations_ancestors`) is attributable to this session's
load, not to code changes in either library — meaning most of the cross-session absolute-value
movement in the table above is not a reliable regression signal on its own. The same-session,
paired Astra-vs-flecs-vs-EnTT ratios in the Tier-A/Tier-B tables (run under identical conditions
within each round) remain the trustworthy same-session comparison, and by that measure Astra's
standing relative to both competitors — ahead on `create`/`create_batch`(vs entt)/`destroy`(vs
entt)/`iterate2_half`(vs entt)/`iterate5`(vs entt)/`relations_descendants`, behind on
`destroy`/`relations_ancestors`/`relations_children` (vs flecs) and `add_batch`/`add_component`/
`get_multi`/`random_get`/`remove_batch`/`remove_component` (vs entt) — is qualitatively the same
shape as the July Definitive scoreboard's per-family verdicts (Astra's structural/batch lane vs
entt, and destroy/relations_ancestors vs flecs, were tuning targets there too). Two op families
have a known, tracked-in-git architectural addition since July, independent of this session's
noise: (1) `add_component`/`remove_component`/non-const `random_get` now pay a mandatory
chunk-version stamp per the change-detection design (measured in isolation, at a quieter load, in
the "Change detection" section above: `add_component` +21.8%, `remove_component` +0.05ns
marginal, `random_get` gap explained entirely by the stamp per the `random_get_const` A/B there);
(2) `relations_children`/`relations_descendants`/`relations_ancestors`'s non-const yields now
stamp too, per commit `be0693c` ("Ruling M" — `Relations::ForEachChild/Descendant/Ancestor`
routed through the stamping `GetComponentMut`), which post-dates the CD-gate section's own
2026-09-11 run and was not previously measured against flecs. No further interpretation is drawn
beyond what these tracked sources and today's numbers state.

## Clean-room campaign (2026-09-19, dev @ `b8291b9`)

### Why this section exists: a machine-specific artifact was root-caused between the 09-11 and 09-19 sessions

The 09-11 campaign above never got a quiet machine (21–40% background load, gate never cleared) and
its absolute numbers were already flagged unreliable cross-session. A follow-up investigation on
09-15 found something worse than load noise: on this box, `bench_astra.exe`'s **structural** ops
(`create`/`add_component`/`create_batch`/…) were **8–60× slower** than every prior baseline, while
`iterate*`/`random_get` were unchanged — a signature pointing at allocation, not the CPU. Root
cause, confirmed by direct measurement (`ctypes` timing 64 raw `VirtualAlloc` calls): Astra's chunk
pool requests `MEM_LARGE_PAGES` for its arenas (`Core/Memory.hpp`), and on a long-uptime box with
fragmented physical RAM, `VirtualAlloc(..., MEM_LARGE_PAGES)` took a **median 150 ms and failed
60% of the time** (vs 1.2 µs for a regular-page allocation of the same size) — a stall paid once
per arena grow, entirely inside the kernel call, invisible to CPU pinning or clock locking. flecs
and EnTT use plain `malloc` and were unaffected. Full writeup: memory
`astra-bench-hugepage-stall.md`.

A second, distinct variable surfaced when this session (post-reboot) re-ran the check: large-page
`VirtualAlloc` failed **instantly** (0.7 µs, not 150 ms) rather than stalling — because
`SeLockMemoryPrivilege` was present in the shell's token but **disabled**, and Astra's
`IsHugePagesAvailable()` gates on the strict `PrivilegeCheck` for the *enabled* bit, not mere
presence. Windows does not auto-enable an assigned-but-unused privilege, and nothing in Astra
calls `AdjustTokenPrivileges` to turn it on, so a normal, unprivileged process launch silently
takes the regular-page path regardless of fragmentation. Confirmed by enabling the privilege via
`AdjustTokenPrivileges` and re-testing from a genuine **child process** (privilege state is
inherited at `CreateProcess`, not grantable after): 32/32 large-page allocations succeeded at a
median 1.9 µs once both (a) the privilege was enabled and (b) the reboot had defragmented RAM.

**Both are now permanent parts of `bench-compare/bench-clean.ps1`**, alongside the pinning/priority/
quiet-gate/power-scheme-lock this script already provided (see its header comment for the full
mechanism list): the script P/Invokes `AdjustTokenPrivileges` to enable `SeLockMemoryPrivilege` in
its own process before spawning any child bench, and logs the outcome either way so a run where the
account lacks the right (a real "not available on this box" case, not a bug) is self-documenting
rather than silently producing misleading structural-op numbers.

### Protocol

Reboot (fresh that afternoon, ~20 min prior — the fragmentation fix) → `bench-clean.ps1` self-elevates
via a UAC prompt (needed only for the `powercfg` base-clock lock; the privilege-enable and P-core
pinning do not require elevation) → enables `SeLockMemoryPrivilege` → switches to a dedicated
"Astra Benchmark" power scheme (Processor performance boost mode = Disabled, min/max state 100%) →
pins the process to the eight P-core HT pairs minus core 0's (mask `0xFFFC`, core 0 takes
interrupts/DPCs) at High priority → per round, gates on ≥4 of those 14 logical cores under 10% busy
(≤6 retries × 20 s, proceeds and records the reading past that) → runs `astra → flecs → entt`,
appending `roundN,`-prefixed stdout to the CSV → restores the original power scheme in a `finally`
(Ctrl+C included).

`bench_astra.exe` rebuilt fresh from current `include/` (`build_fresh.bat`; dev had moved 2 commits
past the last fresh build, both in `TypeContext`/`ComponentRegistry` — see the road-to-A rtti-dangle
fix — neither on any benchmarked hot path, confirmed by the flat `iterate1`/`random_get` numbers
below). `bench_flecs.exe`/`bench_entt.exe` reused as-is (2026-07-24 18:36; competitor sources
unchanged since).

### Per-round record

| Round | Gate | Idle cores | Avg busy% | %Processor Performance | Wall time |
|---|---|---|---|---|---|
| 1 | CLEAR | 5/14 | 30% | 89% | 54.8 s |
| 2 | CLEAR | 5/14 | 28% | 88% | 53.6 s |
| 3 | CLEAR | 9/14 | 20% | 87% | 50.5 s |
| 4 | CLEAR | 10/14 | 15% | 85% | 50.5 s |
| 5 | CLEAR | 7/14 | 27% | 86% | 50.6 s |
| 6 | CLEAR | 10/14 | 17% | 87% | 50.6 s |

Every round cleared the quiet gate on the first attempt (contrast the 09-11 session: 5 retries,
never cleared). Wall times are a tight 50–55 s band — the historical shape (vs. the 09-15
pre-fix smoke run's 230 s for `astra` alone, entirely the large-page stall). Clock held at
85–89% of `MaxClockSpeed` throughout (boost successfully suppressed; not exactly 100% — some
residual variance from the mixed HT/physical-core mask — but stable session-to-session, unlike
turbo-on runs). 486 rows (6 × 81), items check PASS, zero cross-lib mismatches.

### Tier-A table (3-way; N=1,000,000 unless noted; **min of 6 rounds** [min, max] — see "min vs median" below)

| op | N | Astra | flecs | EnTT | vs flecs | vs EnTT |
|---|---|---|---|---|---|---|
| create | 1M | 83.931 [83.931, 94.289] | 138.85 [138.85, 146.59] | 71.938 [71.938, 80.072] | 1.654× NON-OVERLAPPING (astra ahead) | 0.857× NON-OVERLAPPING (astra behind) |
| create_batch | 1M | 28.441 [28.441, 35.302] | 22.812 [22.812, 26.613] | 63.693 [63.693, 71.051] | 0.802× NON-OVERLAPPING (astra behind) | 2.239× NON-OVERLAPPING (astra ahead) |
| add_component | 1M | 66.774 [66.774, 72.614] | 76.963 [76.963, 83.597] | 21.239 [21.239, 24.176] | 1.153× NON-OVERLAPPING (astra ahead) | 0.318× NON-OVERLAPPING (astra behind) |
| remove_component | 1M | 41.521 [41.521, 45.867] | 49.290 [49.290, 52.001] | 24.790 [24.790, 26.527] | 1.187× NON-OVERLAPPING (astra ahead) | 0.597× NON-OVERLAPPING (astra behind) |
| add_batch | 1M | 122.86 [122.86, 134.24] | 133.16 [133.16, 147.83] | 19.438 [19.438, 23.069] | 1.084× overlap/noise | 0.158× NON-OVERLAPPING (astra behind) |
| remove_batch | 1M | 98.714 [98.714, 108.69] | 93.787 [93.787, 104.10] | 23.208 [23.208, 25.064] | 0.950× overlap/noise | 0.235× NON-OVERLAPPING (astra behind) |
| destroy | 1M | 28.707 [28.707, 31.918] | 22.703 [22.703, 25.019] | 74.537 [74.537, 78.114] | 0.791× NON-OVERLAPPING (astra behind) | 2.596× NON-OVERLAPPING (astra ahead) |
| iterate1 | 1M | 0.4530 [0.4530, 0.8720] | 0.4670 [0.4670, 1.712] | 0.6790 [0.6790, 0.9850] | 1.031× overlap/noise | 1.499× overlap/noise |
| iterate1 | 10M | 0.9580 [0.9580, 1.308] | 0.8060 [0.8060, 1.489] | 1.268 [1.268, 1.586] | 0.841× overlap/noise | 1.324× overlap/noise |
| iterate2_half | 1M (items=500K) | 0.3260 [0.3260, 0.6910] | 0.3390 [0.3390, 1.116] | 1.613 [1.613, 1.917] | 1.040× overlap/noise | 4.948× NON-OVERLAPPING (astra ahead) |
| iterate2_one | 1M (items=1) | 0.000 | 0.000 | 0.000 | overlap/noise | overlap/noise (all three at timer floor) |
| iterate5 | 1M | 1.672 [1.672, 3.229] | 1.415 [1.415, 2.197] | 7.273 [7.273, 9.788] | 0.846× overlap/noise | 4.350× NON-OVERLAPPING (astra ahead) |
| iterate5 | 10M | 2.211 [2.211, 2.934] | 2.121 [2.121, 2.546] | 7.980 [7.980, 9.593] | 0.959× overlap/noise | 3.609× NON-OVERLAPPING (astra ahead) |
| random_get | 1M | 117.32 [117.32, 131.40] | 84.297 [84.297, 97.996] | 51.987 [51.987, 80.121] | 0.718× NON-OVERLAPPING (astra behind) | 0.443× NON-OVERLAPPING (astra behind) |
| get_multi | 1M (items=2M) | 198.01 [198.01, 219.54] | 150.28 [150.28, 176.59] | 93.699 [93.699, 134.16] | 0.759× NON-OVERLAPPING (astra behind) | 0.473× NON-OVERLAPPING (astra behind) |

`iterate2`/`iterate3` are 2-way (astra/flecs; EnTT reports them under `iterate2_view`/`iterate3_view`,
informational only):

| op | N | Astra | flecs | ratio | flag |
|---|---|---|---|---|---|
| iterate2 | 1M | 1.286 [1.286, 1.921] | 1.252 [1.252, 1.974] | 0.974× | overlap/noise |
| iterate2 | 10M | 1.676 [1.676, 2.278] | 1.545 [1.545, 2.592] | 0.922× | overlap/noise |
| iterate3 | 1M | 1.125 [1.125, 2.021] | 1.196 [1.196, 3.088] | 1.063× | overlap/noise |
| iterate3 | 10M | 1.863 [1.863, 2.120] | 1.699 [1.699, 2.038] | 0.912× | overlap/noise |

### Tier-B table (Astra vs flecs only)

| op | N | Astra | flecs | ratio | flag |
|---|---|---|---|---|---|
| relations_children | 100K nodes (items=100) | 0.0260 [0.0260, 0.0260] | 0.0130 [0.0130, 0.0210] | 0.500× | NON-OVERLAPPING (astra behind) |
| relations_descendants | 100K nodes (items=99,999) | 14.517 [14.517, 17.374] | 74.576 [74.576, 94.923] | 5.137× | NON-OVERLAPPING (astra ahead) |
| relations_ancestors | 100K nodes (items=448,889) | 240.24 [240.24, 283.89] | 128.44 [128.44, 146.07] | 0.535× | NON-OVERLAPPING (astra behind) |
| system_tick_seq | 1M × 3 systems | 2.098 [2.098, 2.671] | 1.857 [1.857, 2.189] | 0.885× | overlap/noise |
| parallel_iterate2 | 1M | 0.1780 [0.1780, 0.6120] | 0.2460 [0.2460, 1.647] | 1.382× | overlap/noise |
| parallel_iterate2 | 10M | 0.9690 [0.9690, 1.140] | 1.064 [1.064, 1.144] | 1.098× | overlap/noise |

**Tuning-target list** (Astra behind, non-overlapping bands): `create_batch` (vs flecs, +24.7%),
`destroy` (vs flecs, +26.4%), `get_multi` (vs flecs +31.8% / vs entt +111.3%), `random_get` (vs
flecs +39.2% / vs entt +125.7%), `relations_ancestors` (vs flecs, +87.0%), `relations_children`
(vs flecs, +100.0%) — plus EnTT's usual structural/random-access lane (`add_batch`/`add_component`/
`remove_batch`/`remove_component`), which the user's standing ruling treats as model-inherent, not
a target. This list is essentially identical in shape to both the 07-24 Definitive scoreboard and
the 09-11 session's — the same handful of ops have been the real tuning targets across three
sessions now.

### min vs median, and why this section leads with min

`analyze_definitive.py` gained a `--stat {median,min}` flag this session. Every prior table in this
file uses **median** of N rounds. For a CPU-bound microbenchmark, sampling noise (a stray
preemption, a cache-cold round) only ever **adds** time — the true cost is a floor, not a center —
so **min of rounds is the more defensible headline statistic for the clean-room protocol**, and is
what the Tier-A/B tables above report. The median-of-rounds numbers for the same data (not tabulated
above; reproduce with `--stat median`) sit 2–7% above the min table throughout, e.g. `create`
87.417 vs 83.931 (median vs min) — a small, uniform gap, not a different shape or a different
tuning-target list.

### Same-session ratio stability across all 6 rounds (the real trust test)

Absolutes alone don't prove a number is trustworthy — the 09-15 investigation showed absolutes can
be off by 60× and still look self-consistent within one bad round. The test that matters is whether
the **paired ratio** (flecs_ns / astra_ns, computed independently each round) stays put across
rounds:

| op | r1 | r2 | r3 | r4 | r5 | r6 | spread |
|---|---|---|---|---|---|---|---|
| create | 1.624 | 1.555 | 1.694 | 1.685 | 1.615 | 1.593 | 8.6% |
| create_batch | 0.754 | 0.774 | 0.781 | 0.833 | 0.740 | 0.814 | 12.0% |
| add_component | 1.072 | 1.143 | 1.232 | 1.153 | 1.161 | 1.149 | 13.9% |
| destroy | 0.766 | 0.779 | 0.832 | 0.759 | 0.785 | 0.813 | 9.3% |
| random_get | 0.683 | 0.642 | 0.706 | 0.698 | 0.757 | 0.820 | 24.9% |

Spread = (max − min) / mean of the 6 per-round ratios. For comparison, the identical check run
mid-investigation on the **fragmented, privilege-disabled** box (09-15, `ratio_check.csv`, discarded)
gave spreads of 42–118% on these same ops, with `create` reading Astra 4–7× *slower* than flecs —
the wrong sign entirely. Astra's own per-round coefficient of variation on `create` dropped from
80% (fragmented) to **11.8%** this session, now in the same range as flecs's own historical noise
floor (~15–18%, per the 07-23 quiet-machine session). This is the evidence that the fix (reboot +
privilege + clock lock + pinning) restored genuine reproducibility, not just better-looking absolutes.

### Verdict against both historical baselines

Against **07-24 Definitive** (Astra medians, quiet machine, turbo-on, no privilege/pinning
protocol): most ops still read slower today in absolute ns — expected, since this session runs at
a **locked base clock** (no turbo) specifically to buy cross-session comparability, which 07-24 did
not have. `create` 87.4 (median) vs 53.5 then; `random_get` 88.4 (median, `random_get_const`) vs
55.8 then. The *qualitative* standing vs flecs is unchanged: ahead on `create`/`add_component`/
`remove_component`/`create_batch`(vs entt)/`destroy`(vs entt)/`relations_descendants`, behind on
`create_batch`/`destroy`/`relations_ancestors`/`relations_children`/`random_get`/`get_multi` (vs
flecs) and the EnTT structural lane — the same tuning-target roster as every prior session.

Against **09-11** (loaded box, gate never cleared, no privilege/pinning protocol): today's numbers
are a mix of faster and slower depending on op — `create` 87.4 vs 70.4 then (today's is the
locked-clock number, so not directly comparable), but the ratio-stability table above is the
number that actually improved: 09-11 never measured its own within-session reproducibility, and
the 09-15 ad-hoc check that did found it badly broken. This session is the first with both a
quantified reproducibility floor and a clean absolute-number provenance chain.

**Bottom line:** the clean-room protocol works. It does not produce numbers identical to any prior
session (different clock policy, different competitor exe ages), and it should not be expected to —
its contribution is that its *own* numbers are now provably stable round-to-round, which none of
the prior sessions could claim. Future campaigns should use `bench-clean.ps1` and report `--stat
min` as the headline, with `--stat median` and the per-round CSV kept for the reproducibility check
above.

## Reproduce
`bench-compare/` — `build_one.bat` (vcvars+cl wrapper), `bench_{astra,entt,flecs}.cpp`, shared `bench_common.hpp`. EnTT/flecs sources under `bench-compare/vendor/`. **Build with the full-opt flag set above (2026-07-24 baseline), not bare `/O2`.**
`bench-clean.ps1` (2026-09-19): clean-room wrapper for a busy dev box — see its header comment for the full mechanism list (privilege-enable, power-scheme lock, P-core pinning, quiet gate). Requires one UAC approval per run (the `powercfg` clock lock). `python analyze_definitive.py --stat min <csv>` for the matching headline statistic.

## Storage policies (spec 2026-10-01; branch `feat/storage-policies` @ `94f4346`, fix round 4 @ `0b5bd45`, measured 2026-10-25)

**Verdict.**
- **S4 part 1: PENDING (desk under load; numbers recorded).** The Table-only gate is not claimed as passed. The 3-round two-base gate lists 10 (vs 975cdb7) and 11 (vs c2dcf2f) FAIL rows from a window with a contaminated round, and the 9-rep re-measure that followed is mostly LOUD, so neither can be called a pass under the tightened rule. Open rows, stated explicitly:
  - **Re-measured (9 reps), stable positive sign:** `AddRemoveByID` +6..9 %, `CommandBufferAddRemove` +3.8..4.6 %, `RangeForFiveComponents` +1.3..4.9 %, `CreateEntities` +3.4..7.7 %, `RemoveComponents/1000000` +3.5 % (vs 975cdb7; vs c2dcf2f also `RemoveComponents/100000` +2.9 %).
  - **Flagged by this gate, NOT re-measured, UNJUDGED:** the 23 rows with a flagged delta above 2 % and the 25 other flagged rows listed in "Flagged, not re-measured, unjudged" below. Among them are the two 3-round FAIL rows `ParallelIterateSingleComponent/100000` and `ParallelIterateThreeComponents/100000`, and the `ParallelIterate*` family, which is positive at most sizes vs both bases. Fix round 4 tried to re-measure them and found no quiet window (below).
- **S4 part 2 (Universal removes a 48 B component from archetype moves): PASS** (18 % faster structural churn at both sizes, far above the 0.1-4.6 % repetition spread).
- **S4 part 3 (Sparse add/remove churn beats Table): PASS** at 1K, 10K and 100K (32-37 % faster).

**Machine:** 12th Gen Intel(R) Core(TM) i7-12700K (20 logical), 64 GB, Windows 10 Pro 19045, MSVC v143 (VS 18 msbuild), Release (`/O2 /Oi /Ot /Oy /GL` + `/LTCG`, `/arch:AVX`, `/fp:fast`). Power plan and clock were NOT locked and nothing was pinned: this is not the clean-room protocol of the 2026-09-19 section. **Load:** a shared dev desk where other sessions build Arcane trees (background 11-30 % total CPU). Two pre-checks were used. The 3-round gate preceded every process run with a quiet check (no non-`/Zs` `cl`/`link`; mean total CPU <= 25 % over 5 x 2 s samples), watched for `cl`/`link` while it ran and redid a run that saw one (at most 2 tries); its per-process log is quoted below. The 9-rep re-measure (`rerun.log`) and the round-3 A/Bs (`partA`, `m2ab`, `m3`) logged 3 CPU samples per run (e.g. `cpu=19,32,23`) plus the `cl`/`link` count, with the same at-most-2-tries redo in the re-measure; one of those samples reached 32 %, above the 11-30 % band stated above. Fix round 4's runs log `cl`/`link` and 3 x 1 s CPU samples per round, and sample `cl`/`link` every second while each process runs (redo once on a hit).

**Protocol.**
- `975cdb7` (the spec's letter) and `c2dcf2f` (the branch tip before any storage code: `975cdb7` + the dev merge + docs, which isolates the storage work) were exported with `git archive` and built with the identical premake/msbuild Release configuration. The two Table-only benchmarks that do not exist at `975cdb7` (`BM_AddRemoveByID`, `BM_CommandBufferAddRemove`, 6 rows) were carried into both base exports as source, so the base binaries differ from the pure commits only by those two added benchmarks. The comparison is therefore 106 rows, not 100.
- The branch binary is built from `94f4346` and skips the branch-only `BM_Storage_*` rows (`--benchmark_filter=-BM_Storage_`). The gate script refuses a branch row that has no base counterpart.
- 3 interleaved rounds (975cdb7, c2dcf2f, branch; every process once per round) of the full Google Benchmark suite, then a 9-repetition re-measure of the families the gate flagged, interleaved PER REP (rep r runs 975cdb7, c2dcf2f, branch once each; 9 times; not back-to-back blocks).
- `benchmark/s4_gate.ps1` compares the median `real_time`. The rule (tightened 2026-10-25): **LOUD** if the base's round-to-round spread (max-min over the median) exceeds 5 % (unjudgeable in this window, never a PASS); **PASS** if delta <= 2 %; **FAIL** if delta > 2 % and > 2 x spread; else **NOISY** (re-measure). Caveat: in the 9-rep mode the spread is a max-min over 9 single samples, which grows with the sample count, so most rows read LOUD there; the medians over 9 interleaved reps are the usable number.

### S4 (1): Table-only benchmarks vs `975cdb7` and `c2dcf2f`

3-round gate on `94f4346`. Per-process log (`peak` is the most `cl`/`link` processes seen while it ran; a second `try` is a redo):

```
gate2 branch=D:\dev\starworks\Astra-storage\bin\fix3\head.exe
round 1
  975 try 1 start 2026-10-25T05:37:45 quiet cpu=17,21,22,16,20 ; end 2026-10-25T05:40:37 cl/link-peak-during=1
  975 try 2 start 2026-10-25T05:41:17 quiet cpu=21,20,19,21,15 ; end 2026-10-25T05:44:04 cl/link-peak-during=0
  c2 try 1 start 2026-10-25T05:45:46 quiet cpu=20,23,17,18,24 ; end 2026-10-25T05:48:29 cl/link-peak-during=0
  branch try 1 start 2026-10-25T05:48:39 quiet cpu=21,11,16,12,12 ; end 2026-10-25T05:51:26 cl/link-peak-during=0
round 2
  975 try 1 start 2026-10-25T05:51:37 quiet cpu=14,20,13,17,20 ; end 2026-10-25T05:54:11 cl/link-peak-during=0
  c2 try 1 start 2026-10-25T05:54:21 quiet cpu=16,12,14,11,19 ; end 2026-10-25T05:57:04 cl/link-peak-during=0
  branch try 1 start 2026-10-25T05:57:14 quiet cpu=19,13,21,20,16 ; end 2026-10-25T06:00:03 cl/link-peak-during=9
  branch try 2 start 2026-10-25T06:04:46 quiet cpu=20,20,30,13,13 ; end 2026-10-25T06:07:45 cl/link-peak-during=9
round 3
  975 try 1 start 2026-10-25T06:09:57 quiet cpu=23,17,26,22,20 ; end 2026-10-25T06:12:36 cl/link-peak-during=2
  975 try 2 start 2026-10-25T06:12:46 quiet cpu=22,11,23,18,15 ; end 2026-10-25T06:15:29 cl/link-peak-during=0
  c2 try 1 start 2026-10-25T06:15:40 quiet cpu=17,21,19,22,21 ; end 2026-10-25T06:18:28 cl/link-peak-during=1
  c2 try 2 start 2026-10-25T06:18:38 quiet cpu=16,19,21,15,19 ; end 2026-10-25T06:21:30 cl/link-peak-during=0
  branch try 1 start 2026-10-25T06:21:40 quiet cpu=23,18,17,17,21 ; end 2026-10-25T06:24:32 cl/link-peak-during=0
ALL DONE 2026-10-25T06:24:32
```

Round 2's branch run saw up to 9 `cl` on both tries (a foreign build), so its numbers are the contaminated last try; that is what the 9-rep re-measure below is for. Rounds 1 and 3 ended on clean tries.

vs `975cdb7` (all 106 rows):

| benchmark | 975cdb7 median | branch median | delta % | base spread % | rounds | verdict |
|---|---|---|---|---|---|---|
| BM_AddComponents/10000 | 1,028,744.8 | 1,016,699.2 | -1.17 | 5.33 | 3/3 | LOUD |
| BM_AddComponents/100000 | 8,563,534.4 | 8,639,295.6 | +0.88 | 2.64 | 3/3 | PASS |
| BM_AddComponents/1000000 | 86,818,542.9 | 86,156,566.7 | -0.76 | 3.39 | 3/3 | PASS |
| BM_AddComponentsBatch/10000 | 1,634,626.5 | 1,668,437.9 | +2.07 | 2.88 | 3/3 | NOISY |
| BM_AddComponentsBatch/100000 | 15,378,902.4 | 15,742,406.7 | +2.36 | 9.10 | 3/3 | LOUD |
| BM_AddComponentsBatch/1000000 | 189,356,675.1 | 185,845,100.0 | -1.85 | 7.03 | 3/3 | LOUD |
| BM_AddRemoveByID/1000 | 53,652.1 | 57,574.8 | +7.31 | 2.82 | 3/3 | FAIL |
| BM_AddRemoveByID/10000 | 547,262.8 | 577,742.2 | +5.57 | 3.21 | 3/3 | NOISY |
| BM_AddRemoveByID/100000 | 5,768,263.0 | 5,858,871.4 | +1.57 | 7.82 | 3/3 | LOUD |
| BM_CommandBufferAddRemove/1000 | 86,089.4 | 89,903.6 | +4.43 | 5.48 | 3/3 | LOUD |
| BM_CommandBufferAddRemove/10000 | 814,223.2 | 867,698.5 | +6.57 | 9.67 | 3/3 | LOUD |
| BM_CommandBufferAddRemove/100000 | 8,349,165.6 | 9,062,579.7 | +8.54 | 7.16 | 3/3 | LOUD |
| BM_CreateEntities/10000 | 970,013.0 | 980,069.5 | +1.04 | 3.14 | 3/3 | PASS |
| BM_CreateEntities/100000 | 5,337,512.0 | 5,297,172.3 | -0.76 | 11.02 | 3/3 | LOUD |
| BM_CreateEntities/1000000 | 52,949,230.0 | 55,006,400.0 | +3.89 | 4.85 | 3/3 | NOISY |
| BM_CreateEntitiesBatch/10000 | 933,070.1 | 925,894.3 | -0.77 | 9.75 | 3/3 | LOUD |
| BM_CreateEntitiesBatch/100000 | 4,823,357.1 | 4,596,762.3 | -4.70 | 5.94 | 3/3 | LOUD |
| BM_CreateEntitiesBatch/1000000 | 52,077,110.0 | 52,205,040.0 | +0.25 | 9.96 | 3/3 | LOUD |
| BM_ForEachAncestor/1000 | 9,894.3 | 5,726.7 | -42.12 | 6.97 | 3/3 | LOUD |
| BM_ForEachAncestor/10000 | 96,459.2 | 55,598.7 | -42.36 | 1.05 | 3/3 | PASS |
| BM_ForEachAncestor/100000 | 643,797.5 | 366,416.9 | -43.09 | 0.71 | 3/3 | PASS |
| BM_ForEachChild/1000 | 10,381.2 | 6,187.6 | -40.40 | 4.47 | 3/3 | PASS |
| BM_ForEachChild/10000 | 99,291.8 | 62,353.2 | -37.20 | 4.69 | 3/3 | PASS |
| BM_ForEachChild/100000 | 1,018,420.9 | 596,524.8 | -41.43 | 3.44 | 3/3 | PASS |
| BM_ForEachDescendant/1000 | 3,306.4 | 1,789.5 | -45.88 | 4.64 | 3/3 | PASS |
| BM_ForEachDescendant/10000 | 37,825.3 | 20,907.3 | -44.73 | 6.45 | 3/3 | LOUD |
| BM_ForEachDescendant/100000 | 550,903.9 | 322,030.5 | -41.55 | 4.21 | 3/3 | PASS |
| BM_ForEachLink/1000 | 10,134.1 | 6,189.0 | -38.93 | 6.80 | 3/3 | LOUD |
| BM_ForEachLink/10000 | 100,285.6 | 59,744.8 | -40.43 | 6.36 | 3/3 | LOUD |
| BM_ForEachLink/100000 | 1,107,328.8 | 593,732.5 | -46.38 | 12.03 | 3/3 | LOUD |
| BM_GetComponent/10000 | 78,358.3 | 53,194.4 | -32.11 | 4.65 | 3/3 | PASS |
| BM_GetComponent/100000 | 816,968.3 | 568,261.7 | -30.44 | 7.50 | 3/3 | LOUD |
| BM_GetComponent/1000000 | 7,750,834.4 | 5,596,158.0 | -27.80 | 2.51 | 3/3 | PASS |
| BM_GetMultipleComponents/10000 | 150,831.5 | 100,915.5 | -33.09 | 2.20 | 3/3 | PASS |
| BM_GetMultipleComponents/100000 | 1,544,125.7 | 1,012,166.1 | -34.45 | 0.42 | 3/3 | PASS |
| BM_GetMultipleComponents/1000000 | 15,518,460.0 | 10,475,568.0 | -32.50 | 2.19 | 3/3 | PASS |
| BM_IterateFiveComponents/10000 | 64,420.4 | 64,076.2 | -0.53 | 2.14 | 3/3 | PASS |
| BM_IterateFiveComponents/100000 | 665,517.9 | 682,829.8 | +2.60 | 4.07 | 3/3 | NOISY |
| BM_IterateFiveComponents/1000000 | 6,960,090.2 | 7,107,142.2 | +2.11 | 2.50 | 3/3 | NOISY |
| BM_IterateSingleComponent/10000 | 10,467.8 | 10,417.9 | -0.48 | 2.82 | 3/3 | PASS |
| BM_IterateSingleComponent/100000 | 109,294.1 | 108,097.0 | -1.10 | 0.35 | 3/3 | PASS |
| BM_IterateSingleComponent/1000000 | 1,209,910.9 | 1,244,808.4 | +2.88 | 3.64 | 3/3 | NOISY |
| BM_IterateThreeComponents/10000 | 27,425.6 | 28,030.5 | +2.21 | 5.16 | 3/3 | LOUD |
| BM_IterateThreeComponents/100000 | 293,290.8 | 287,384.0 | -2.01 | 5.01 | 3/3 | LOUD |
| BM_IterateThreeComponents/1000000 | 3,263,000.0 | 3,141,374.6 | -3.73 | 8.08 | 3/3 | LOUD |
| BM_IterateTwoComponents/10000 | 19,397.4 | 20,533.0 | +5.85 | 3.64 | 3/3 | NOISY |
| BM_IterateTwoComponents/100000 | 223,810.4 | 221,070.9 | -1.22 | 2.02 | 3/3 | PASS |
| BM_IterateTwoComponents/1000000 | 2,794,408.9 | 2,648,196.2 | -5.23 | 4.47 | 3/3 | PASS |
| BM_IterateTwoComponentsHalf/10000 | 10,112.0 | 9,893.9 | -2.16 | 3.81 | 3/3 | PASS |
| BM_IterateTwoComponentsHalf/100000 | 106,648.2 | 107,619.0 | +0.91 | 3.43 | 3/3 | PASS |
| BM_IterateTwoComponentsHalf/1000000 | 1,259,733.0 | 1,270,485.7 | +0.85 | 6.13 | 3/3 | LOUD |
| BM_IterateTwoComponentsOne/10000 | 10.3 | 10.5 | +1.45 | 1.22 | 3/3 | PASS |
| BM_IterateTwoComponentsOne/100000 | 10.3 | 10.3 | -0.25 | 0.40 | 3/3 | PASS |
| BM_IterateTwoComponentsOne/1000000 | 10.3 | 10.3 | +0.46 | 2.67 | 3/3 | PASS |
| BM_ParallelForEachDescendant/1000 | 4,046.0 | 2,886.9 | -28.65 | 7.24 | 3/3 | LOUD |
| BM_ParallelForEachDescendant/10000 | 115,361.9 | 106,906.7 | -7.33 | 7.37 | 3/3 | LOUD |
| BM_ParallelForEachDescendant/100000 | 1,388,017.7 | 1,291,977.9 | -6.92 | 4.99 | 3/3 | PASS |
| BM_ParallelIterateFiveComponents/10000 | 127,990.5 | 131,132.3 | +2.45 | 2.35 | 3/3 | NOISY |
| BM_ParallelIterateFiveComponents/100000 | 1,337,987.0 | 1,359,438.1 | +1.60 | 4.19 | 3/3 | PASS |
| BM_ParallelIterateFiveComponents/1000000 | 16,162,757.3 | 16,487,932.8 | +2.01 | 7.05 | 3/3 | LOUD |
| BM_ParallelIterateSingleComponent/10000 | 25,437.6 | 25,840.1 | +1.58 | 1.03 | 3/3 | PASS |
| BM_ParallelIterateSingleComponent/100000 | 166,096.0 | 169,534.7 | +2.07 | 0.97 | 3/3 | FAIL |
| BM_ParallelIterateSingleComponent/1000000 | 2,117,778.2 | 2,195,647.1 | +3.68 | 2.71 | 3/3 | NOISY |
| BM_ParallelIterateThreeComponents/10000 | 47,442.1 | 50,416.0 | +6.27 | 6.98 | 3/3 | LOUD |
| BM_ParallelIterateThreeComponents/100000 | 533,685.6 | 545,395.7 | +2.19 | 0.64 | 3/3 | FAIL |
| BM_ParallelIterateThreeComponents/1000000 | 6,232,207.0 | 6,645,298.2 | +6.63 | 5.22 | 3/3 | LOUD |
| BM_ParallelIterateTwoComponents/10000 | 38,082.5 | 39,703.9 | +4.26 | 5.16 | 3/3 | LOUD |
| BM_ParallelIterateTwoComponents/100000 | 364,403.3 | 367,704.7 | +0.91 | 0.77 | 3/3 | PASS |
| BM_ParallelIterateTwoComponents/1000000 | 4,376,385.6 | 4,565,997.2 | +4.33 | 2.59 | 3/3 | NOISY |
| BM_ParallelIterateTwoComponentsHalf/10000 | 25,305.1 | 25,216.2 | -0.35 | 1.07 | 3/3 | PASS |
| BM_ParallelIterateTwoComponentsHalf/100000 | 158,168.9 | 161,483.1 | +2.10 | 0.95 | 3/3 | FAIL |
| BM_ParallelIterateTwoComponentsHalf/1000000 | 1,989,365.7 | 2,072,029.8 | +4.16 | 1.93 | 3/3 | FAIL |
| BM_ParallelIterateTwoComponentsOne/10000 | 14.8 | 13.4 | -9.69 | 3.43 | 3/3 | PASS |
| BM_ParallelIterateTwoComponentsOne/100000 | 14.9 | 13.4 | -9.53 | 4.68 | 3/3 | PASS |
| BM_ParallelIterateTwoComponentsOne/1000000 | 14.5 | 13.4 | -7.37 | 1.37 | 3/3 | PASS |
| BM_RangeForFiveComponents/10000 | 66,156.7 | 72,351.9 | +9.36 | 1.94 | 3/3 | FAIL |
| BM_RangeForFiveComponents/100000 | 698,933.7 | 742,826.9 | +6.28 | 7.80 | 3/3 | LOUD |
| BM_RangeForFiveComponents/1000000 | 7,194,292.0 | 7,422,978.9 | +3.18 | 2.28 | 3/3 | NOISY |
| BM_RangeForSingleComponent/10000 | 9,940.1 | 10,000.9 | +0.61 | 1.73 | 3/3 | PASS |
| BM_RangeForSingleComponent/100000 | 106,647.9 | 105,564.4 | -1.02 | 2.81 | 3/3 | PASS |
| BM_RangeForSingleComponent/1000000 | 1,264,809.8 | 1,195,416.4 | -5.49 | 9.84 | 3/3 | LOUD |
| BM_RangeForThreeComponents/10000 | 30,180.3 | 31,162.0 | +3.25 | 7.23 | 3/3 | LOUD |
| BM_RangeForThreeComponents/100000 | 324,942.5 | 332,730.6 | +2.40 | 9.16 | 3/3 | LOUD |
| BM_RangeForThreeComponents/1000000 | 3,444,881.0 | 3,515,702.8 | +2.06 | 7.02 | 3/3 | LOUD |
| BM_RangeForTwoComponents/10000 | 19,310.8 | 20,020.3 | +3.67 | 3.82 | 3/3 | NOISY |
| BM_RangeForTwoComponents/100000 | 221,869.7 | 222,747.6 | +0.40 | 0.53 | 3/3 | PASS |
| BM_RangeForTwoComponents/1000000 | 2,732,326.4 | 2,647,052.5 | -3.12 | 9.61 | 3/3 | LOUD |
| BM_RangeForTwoComponentsHalf/10000 | 11,751.3 | 11,887.6 | +1.16 | 3.85 | 3/3 | PASS |
| BM_RangeForTwoComponentsHalf/100000 | 123,252.6 | 122,682.5 | -0.46 | 0.96 | 3/3 | PASS |
| BM_RangeForTwoComponentsHalf/1000000 | 1,332,615.7 | 1,539,976.1 | +15.56 | 4.21 | 3/3 | FAIL |
| BM_RemoveComponents/10000 | 609,361.6 | 626,408.3 | +2.80 | 14.09 | 3/3 | LOUD |
| BM_RemoveComponents/100000 | 5,263,991.1 | 5,276,443.0 | +0.24 | 7.74 | 3/3 | LOUD |
| BM_RemoveComponents/1000000 | 54,520,170.0 | 53,971,763.7 | -1.01 | 6.13 | 3/3 | LOUD |
| BM_RemoveComponentsBatch/10000 | 907,200.2 | 929,128.4 | +2.42 | 10.55 | 3/3 | LOUD |
| BM_RemoveComponentsBatch/100000 | 8,892,993.8 | 9,144,774.0 | +2.83 | 3.91 | 3/3 | NOISY |
| BM_RemoveComponentsBatch/1000000 | 104,553,933.3 | 101,077,616.7 | -3.32 | 7.60 | 3/3 | LOUD |
| BM_SystemScheduler_CustomExecutor/10000 | 10,672.5 | 11,021.5 | +3.27 | 0.32 | 3/3 | FAIL |
| BM_SystemScheduler_CustomExecutor/100000 | 108,176.1 | 108,483.3 | +0.28 | 1.88 | 3/3 | PASS |
| BM_SystemScheduler_Lambda/10000 | 10,860.7 | 11,039.0 | +1.64 | 8.49 | 3/3 | LOUD |
| BM_SystemScheduler_Lambda/100000 | 109,532.5 | 113,984.3 | +4.06 | 5.15 | 3/3 | LOUD |
| BM_SystemScheduler_ManyIndependent | 12,021.3 | 12,477.1 | +3.79 | 0.82 | 3/3 | FAIL |
| BM_SystemScheduler_Parallel/10000 | 31,090.7 | 31,948.1 | +2.76 | 1.10 | 3/3 | FAIL |
| BM_SystemScheduler_Parallel/100000 | 151,462.0 | 151,886.1 | +0.28 | 1.13 | 3/3 | PASS |
| BM_SystemScheduler_Sequential/10000 | 12,283.9 | 12,366.4 | +0.67 | 2.42 | 3/3 | PASS |
| BM_SystemScheduler_Sequential/100000 | 126,134.2 | 125,725.0 | -0.32 | 3.36 | 3/3 | PASS |
| BM_SystemScheduler_WithDependencies | 34,350.0 | 35,228.5 | +2.56 | 3.22 | 3/3 | NOISY |

`S4 GATE: 106 compared, 42 PASS, 14 NOISY, 40 LOUD, 10 FAIL, 0 missing on branch, 0 branch-only without base`

vs `c2dcf2f` (the non-PASS rows only):

| benchmark | c2dcf2f median | branch median | delta % | base spread % | rounds | verdict |
|---|---|---|---|---|---|---|
| BM_AddRemoveByID/1000 | 54,370.5 | 57,574.8 | +5.89 | 0.46 | 3/3 | FAIL |
| BM_AddRemoveByID/10000 | 548,744.0 | 577,742.2 | +5.28 | 2.22 | 3/3 | FAIL |
| BM_AddRemoveByID/100000 | 5,648,706.0 | 5,858,871.4 | +3.72 | 4.26 | 3/3 | NOISY |
| BM_CommandBufferAddRemove/1000 | 80,771.7 | 89,903.6 | +11.31 | 4.85 | 3/3 | FAIL |
| BM_CommandBufferAddRemove/10000 | 832,858.4 | 867,698.5 | +4.18 | 3.74 | 3/3 | NOISY |
| BM_CommandBufferAddRemove/100000 | 8,555,870.7 | 9,062,579.7 | +5.92 | 6.89 | 3/3 | LOUD |
| BM_CreateEntities/10000 | 987,293.8 | 980,069.5 | -0.73 | 15.64 | 3/3 | LOUD |
| BM_CreateEntities/100000 | 5,301,051.0 | 5,297,172.3 | -0.07 | 5.35 | 3/3 | LOUD |
| BM_CreateEntities/1000000 | 51,195,254.5 | 55,006,400.0 | +7.44 | 4.44 | 3/3 | NOISY |
| BM_CreateEntitiesBatch/10000 | 870,239.1 | 925,894.3 | +6.40 | 14.48 | 3/3 | LOUD |
| BM_CreateEntitiesBatch/1000000 | 53,244,945.5 | 52,205,040.0 | -1.95 | 13.38 | 3/3 | LOUD |
| BM_ForEachAncestor/1000 | 9,676.8 | 5,726.7 | -40.82 | 7.75 | 3/3 | LOUD |
| BM_ForEachChild/1000 | 10,090.7 | 6,187.6 | -38.68 | 5.58 | 3/3 | LOUD |
| BM_ForEachChild/100000 | 997,366.0 | 596,524.8 | -40.19 | 12.32 | 3/3 | LOUD |
| BM_ForEachDescendant/10000 | 37,047.8 | 20,907.3 | -43.57 | 7.31 | 3/3 | LOUD |
| BM_ForEachDescendant/100000 | 526,707.1 | 322,030.5 | -38.86 | 5.18 | 3/3 | LOUD |
| BM_ForEachLink/100000 | 1,018,430.3 | 593,732.5 | -41.70 | 5.78 | 3/3 | LOUD |
| BM_GetComponent/10000 | 79,616.5 | 53,194.4 | -33.19 | 7.63 | 3/3 | LOUD |
| BM_GetComponent/1000000 | 8,102,928.9 | 5,596,158.0 | -30.94 | 7.38 | 3/3 | LOUD |
| BM_IterateFiveComponents/1000000 | 7,024,491.1 | 7,107,142.2 | +1.18 | 6.74 | 3/3 | LOUD |
| BM_IterateSingleComponent/1000000 | 1,249,007.0 | 1,244,808.4 | -0.34 | 6.81 | 3/3 | LOUD |
| BM_IterateTwoComponents/1000000 | 2,728,839.8 | 2,648,196.2 | -2.96 | 5.36 | 3/3 | LOUD |
| BM_IterateTwoComponentsHalf/100000 | 105,398.3 | 107,619.0 | +2.11 | 0.92 | 3/3 | FAIL |
| BM_IterateTwoComponentsOne/10000 | 9.8 | 10.5 | +6.56 | 0.25 | 3/3 | FAIL |
| BM_IterateTwoComponentsOne/100000 | 9.7 | 10.3 | +6.48 | 4.31 | 3/3 | NOISY |
| BM_IterateTwoComponentsOne/1000000 | 9.7 | 10.3 | +6.82 | 0.09 | 3/3 | FAIL |
| BM_ParallelForEachDescendant/1000 | 3,878.1 | 2,886.9 | -25.56 | 7.10 | 3/3 | LOUD |
| BM_ParallelForEachDescendant/10000 | 114,258.2 | 106,906.7 | -6.43 | 5.22 | 3/3 | LOUD |
| BM_ParallelIterateFiveComponents/10000 | 128,116.1 | 131,132.3 | +2.35 | 5.24 | 3/3 | LOUD |
| BM_ParallelIterateFiveComponents/100000 | 1,365,367.2 | 1,359,438.1 | -0.43 | 6.33 | 3/3 | LOUD |
| BM_ParallelIterateFiveComponents/1000000 | 16,548,492.2 | 16,487,932.8 | -0.37 | 9.87 | 3/3 | LOUD |
| BM_ParallelIterateSingleComponent/10000 | 25,485.6 | 25,840.1 | +1.39 | 10.10 | 3/3 | LOUD |
| BM_ParallelIterateSingleComponent/1000000 | 2,163,664.3 | 2,195,647.1 | +1.48 | 6.46 | 3/3 | LOUD |
| BM_ParallelIterateThreeComponents/10000 | 47,386.5 | 50,416.0 | +6.39 | 6.40 | 3/3 | LOUD |
| BM_ParallelIterateThreeComponents/1000000 | 6,499,223.0 | 6,645,298.2 | +2.25 | 9.52 | 3/3 | LOUD |
| BM_ParallelIterateTwoComponents/10000 | 37,665.7 | 39,703.9 | +5.41 | 5.58 | 3/3 | LOUD |
| BM_ParallelIterateTwoComponents/1000000 | 4,416,665.1 | 4,565,997.2 | +3.38 | 7.33 | 3/3 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/10000 | 25,896.8 | 25,216.2 | -2.63 | 12.14 | 3/3 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/100000 | 157,760.2 | 161,483.1 | +2.36 | 3.00 | 3/3 | NOISY |
| BM_ParallelIterateTwoComponentsHalf/1000000 | 2,002,796.5 | 2,072,029.8 | +3.46 | 4.76 | 3/3 | NOISY |
| BM_ParallelIterateTwoComponentsOne/100000 | 13.8 | 13.4 | -2.85 | 5.25 | 3/3 | LOUD |
| BM_RangeForFiveComponents/10000 | 66,942.8 | 72,351.9 | +8.08 | 1.12 | 3/3 | FAIL |
| BM_RangeForFiveComponents/100000 | 707,118.2 | 742,826.9 | +5.05 | 1.79 | 3/3 | FAIL |
| BM_RangeForFiveComponents/1000000 | 7,154,711.1 | 7,422,978.9 | +3.75 | 1.56 | 3/3 | FAIL |
| BM_RangeForSingleComponent/100000 | 105,951.8 | 105,564.4 | -0.37 | 6.59 | 3/3 | LOUD |
| BM_RangeForSingleComponent/1000000 | 1,164,851.2 | 1,195,416.4 | +2.62 | 13.61 | 3/3 | LOUD |
| BM_RangeForThreeComponents/10000 | 31,198.1 | 31,162.0 | -0.12 | 10.41 | 3/3 | LOUD |
| BM_RangeForThreeComponents/100000 | 317,512.3 | 332,730.6 | +4.79 | 7.38 | 3/3 | LOUD |
| BM_RangeForThreeComponents/1000000 | 3,455,410.3 | 3,515,702.8 | +1.74 | 8.49 | 3/3 | LOUD |
| BM_RangeForTwoComponents/1000000 | 2,795,682.6 | 2,647,052.5 | -5.32 | 6.99 | 3/3 | LOUD |
| BM_RangeForTwoComponentsHalf/1000000 | 1,420,691.2 | 1,539,976.1 | +8.40 | 11.32 | 3/3 | LOUD |
| BM_RemoveComponents/10000 | 614,562.0 | 626,408.3 | +1.93 | 13.70 | 3/3 | LOUD |
| BM_RemoveComponents/100000 | 5,356,350.9 | 5,276,443.0 | -1.49 | 6.86 | 3/3 | LOUD |
| BM_RemoveComponents/1000000 | 51,653,081.8 | 53,971,763.7 | +4.49 | 1.87 | 3/3 | FAIL |
| BM_RemoveComponentsBatch/100000 | 9,395,145.6 | 9,144,774.0 | -2.66 | 6.79 | 3/3 | LOUD |
| BM_RemoveComponentsBatch/1000000 | 104,657,450.0 | 101,077,616.7 | -3.42 | 6.32 | 3/3 | LOUD |
| BM_SystemScheduler_CustomExecutor/10000 | 10,626.7 | 11,021.5 | +3.72 | 7.64 | 3/3 | LOUD |
| BM_SystemScheduler_Lambda/10000 | 10,431.9 | 11,039.0 | +5.82 | 2.61 | 3/3 | FAIL |
| BM_SystemScheduler_Lambda/100000 | 108,981.5 | 113,984.3 | +4.59 | 3.67 | 3/3 | NOISY |
| BM_SystemScheduler_ManyIndependent | 11,746.6 | 12,477.1 | +6.22 | 5.79 | 3/3 | LOUD |
| BM_SystemScheduler_Sequential/100000 | 123,534.2 | 125,725.0 | +1.77 | 6.64 | 3/3 | LOUD |
| BM_SystemScheduler_WithDependencies | 33,916.5 | 35,228.5 | +3.87 | 2.77 | 3/3 | NOISY |

`S4 GATE: 106 compared, 44 PASS, 8 NOISY, 43 LOUD, 11 FAIL, 0 missing on branch, 0 branch-only without base`

An earlier 3-round pass of the same binary (05:00-05:25) ran while other sessions built (the round 2 and 3 pre-checks showed 3 `cl`): `S4 GATE: 106 compared, 45 PASS, 10 NOISY, 45 LOUD, 6 FAIL, 0 missing on branch, 0 branch-only without base` vs 975cdb7 and `S4 GATE: 106 compared, 19 PASS, 7 NOISY, 74 LOUD, 6 FAIL, 0 missing on branch, 0 branch-only without base` vs c2dcf2f. It is not judgeable and is kept only as history.

**Re-measure, 9 interleaved reps, vs `975cdb7`.** 40 rows: the families fix round 2's gate flagged, a SUBSET of the families this gate flagged (the rest were not re-measured; they are listed as unjudged below). One sample in the nine is contaminated: rep 4's branch run saw `peak-cl/link=4` and then `=7` on its only two tries, and the second (contaminated) try was kept (`rerun.log`). That biases at most one rank of the branch median, toward a slower branch. Rep 5's branch run saw 4 on try 1 and was redone clean.

| benchmark | 975cdb7 median | branch median | delta % | base spread % | rounds | verdict |
|---|---|---|---|---|---|---|
| BM_AddRemoveByID/1000 | 54,718.3 | 58,631.3 | +7.15 | 3.93 | 9/9 | NOISY |
| BM_AddRemoveByID/10000 | 548,016.8 | 582,736.7 | +6.34 | 10.24 | 9/9 | LOUD |
| BM_AddRemoveByID/100000 | 5,480,059.8 | 5,951,586.7 | +8.60 | 5.71 | 9/9 | LOUD |
| BM_CommandBufferAddRemove/1000 | 82,660.0 | 86,436.0 | +4.57 | 16.39 | 9/9 | LOUD |
| BM_CommandBufferAddRemove/10000 | 831,302.0 | 863,710.7 | +3.90 | 5.57 | 9/9 | LOUD |
| BM_CommandBufferAddRemove/100000 | 8,532,426.7 | 8,858,120.0 | +3.82 | 14.20 | 9/9 | LOUD |
| BM_CreateEntities/10000 | 906,904.5 | 948,447.1 | +4.58 | 16.43 | 9/9 | LOUD |
| BM_CreateEntities/100000 | 5,291,490.2 | 5,469,058.0 | +3.36 | 11.16 | 9/9 | LOUD |
| BM_CreateEntities/1000000 | 54,534,700.0 | 58,715,209.1 | +7.67 | 18.53 | 9/9 | LOUD |
| BM_IterateTwoComponents/10000 | 19,912.4 | 19,804.5 | -0.54 | 5.50 | 9/9 | LOUD |
| BM_IterateTwoComponents/100000 | 222,978.8 | 223,905.2 | +0.42 | 4.24 | 9/9 | PASS |
| BM_IterateTwoComponents/1000000 | 2,866,039.4 | 2,770,707.2 | -3.33 | 11.78 | 9/9 | LOUD |
| BM_IterateTwoComponentsHalf/10000 | 10,315.2 | 10,071.9 | -2.36 | 4.53 | 9/9 | PASS |
| BM_IterateTwoComponentsHalf/100000 | 106,116.5 | 107,218.5 | +1.04 | 5.50 | 9/9 | LOUD |
| BM_IterateTwoComponentsHalf/1000000 | 1,237,500.5 | 1,240,855.0 | +0.27 | 16.45 | 9/9 | LOUD |
| BM_IterateTwoComponentsOne/10000 | 10.4 | 10.4 | +0.21 | 3.53 | 9/9 | PASS |
| BM_IterateTwoComponentsOne/100000 | 10.3 | 10.3 | +0.17 | 6.04 | 9/9 | LOUD |
| BM_IterateTwoComponentsOne/1000000 | 10.3 | 10.4 | +0.02 | 11.16 | 9/9 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/10000 | 25,067.1 | 25,153.7 | +0.35 | 19.89 | 9/9 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/100000 | 160,317.1 | 159,176.2 | -0.71 | 6.07 | 9/9 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/1000000 | 2,071,625.6 | 2,016,850.0 | -2.64 | 7.93 | 9/9 | LOUD |
| BM_RangeForFiveComponents/10000 | 67,574.4 | 70,895.8 | +4.92 | 14.29 | 9/9 | LOUD |
| BM_RangeForFiveComponents/100000 | 713,324.4 | 722,224.4 | +1.25 | 5.34 | 9/9 | LOUD |
| BM_RangeForFiveComponents/1000000 | 7,128,810.0 | 7,395,720.5 | +3.74 | 10.44 | 9/9 | LOUD |
| BM_RangeForTwoComponentsHalf/10000 | 11,766.1 | 11,789.1 | +0.19 | 9.43 | 9/9 | LOUD |
| BM_RangeForTwoComponentsHalf/100000 | 124,814.8 | 123,445.9 | -1.10 | 8.63 | 9/9 | LOUD |
| BM_RangeForTwoComponentsHalf/1000000 | 1,387,271.1 | 1,437,361.8 | +3.61 | 22.60 | 9/9 | LOUD |
| BM_RemoveComponents/10000 | 596,497.7 | 598,059.6 | +0.26 | 17.37 | 9/9 | LOUD |
| BM_RemoveComponents/100000 | 5,484,542.0 | 5,445,580.0 | -0.71 | 11.68 | 9/9 | LOUD |
| BM_RemoveComponents/1000000 | 55,111,436.4 | 57,042,809.1 | +3.50 | 4.83 | 9/9 | NOISY |
| BM_SystemScheduler_CustomExecutor/10000 | 10,684.4 | 10,614.0 | -0.66 | 9.50 | 9/9 | LOUD |
| BM_SystemScheduler_CustomExecutor/100000 | 111,078.3 | 109,875.9 | -1.08 | 6.31 | 9/9 | LOUD |
| BM_SystemScheduler_Lambda/10000 | 10,639.2 | 10,826.3 | +1.76 | 18.15 | 9/9 | LOUD |
| BM_SystemScheduler_Lambda/100000 | 110,918.0 | 110,431.3 | -0.44 | 10.09 | 9/9 | LOUD |
| BM_SystemScheduler_ManyIndependent | 11,925.6 | 12,113.6 | +1.58 | 6.75 | 9/9 | LOUD |
| BM_SystemScheduler_Parallel/10000 | 31,110.9 | 31,459.1 | +1.12 | 7.90 | 9/9 | LOUD |
| BM_SystemScheduler_Parallel/100000 | 151,723.7 | 151,001.6 | -0.48 | 9.37 | 9/9 | LOUD |
| BM_SystemScheduler_Sequential/10000 | 12,339.9 | 12,863.2 | +4.24 | 9.18 | 9/9 | LOUD |
| BM_SystemScheduler_Sequential/100000 | 123,097.3 | 124,456.5 | +1.10 | 10.08 | 9/9 | LOUD |
| BM_SystemScheduler_WithDependencies | 34,305.0 | 34,581.0 | +0.80 | 4.84 | 9/9 | PASS |

`S4 GATE: 40 compared, 4 PASS, 2 NOISY, 34 LOUD, 0 FAIL, 0 missing on branch, 0 branch-only without base`

**vs `c2dcf2f`:**

| benchmark | c2dcf2f median | branch median | delta % | base spread % | rounds | verdict |
|---|---|---|---|---|---|---|
| BM_AddRemoveByID/1000 | 55,249.9 | 58,631.3 | +6.12 | 9.24 | 9/9 | LOUD |
| BM_AddRemoveByID/10000 | 547,019.6 | 582,736.7 | +6.53 | 4.10 | 9/9 | NOISY |
| BM_AddRemoveByID/100000 | 5,526,609.8 | 5,951,586.7 | +7.69 | 17.62 | 9/9 | LOUD |
| BM_CommandBufferAddRemove/1000 | 83,193.3 | 86,436.0 | +3.90 | 10.82 | 9/9 | LOUD |
| BM_CommandBufferAddRemove/10000 | 834,484.7 | 863,710.7 | +3.50 | 11.82 | 9/9 | LOUD |
| BM_CommandBufferAddRemove/100000 | 8,470,564.4 | 8,858,120.0 | +4.58 | 11.40 | 9/9 | LOUD |
| BM_CreateEntities/10000 | 944,156.1 | 948,447.1 | +0.45 | 14.25 | 9/9 | LOUD |
| BM_CreateEntities/100000 | 5,322,537.5 | 5,469,058.0 | +2.75 | 10.59 | 9/9 | LOUD |
| BM_CreateEntities/1000000 | 54,506,009.1 | 58,715,209.1 | +7.72 | 7.53 | 9/9 | LOUD |
| BM_IterateTwoComponents/10000 | 20,280.1 | 19,804.5 | -2.35 | 2.58 | 9/9 | PASS |
| BM_IterateTwoComponents/100000 | 224,113.2 | 223,905.2 | -0.09 | 2.75 | 9/9 | PASS |
| BM_IterateTwoComponents/1000000 | 2,812,029.5 | 2,770,707.2 | -1.47 | 35.48 | 9/9 | LOUD |
| BM_IterateTwoComponentsHalf/10000 | 9,903.7 | 10,071.9 | +1.70 | 4.54 | 9/9 | PASS |
| BM_IterateTwoComponentsHalf/100000 | 104,418.2 | 107,218.5 | +2.68 | 2.35 | 9/9 | NOISY |
| BM_IterateTwoComponentsHalf/1000000 | 1,313,577.1 | 1,240,855.0 | -5.54 | 15.27 | 9/9 | LOUD |
| BM_IterateTwoComponentsOne/10000 | 9.9 | 10.4 | +4.48 | 7.28 | 9/9 | LOUD |
| BM_IterateTwoComponentsOne/100000 | 9.9 | 10.3 | +4.25 | 7.59 | 9/9 | LOUD |
| BM_IterateTwoComponentsOne/1000000 | 9.8 | 10.4 | +5.16 | 11.09 | 9/9 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/10000 | 25,399.2 | 25,153.7 | -0.97 | 11.94 | 9/9 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/100000 | 157,854.4 | 159,176.2 | +0.84 | 5.41 | 9/9 | LOUD |
| BM_ParallelIterateTwoComponentsHalf/1000000 | 2,030,067.4 | 2,016,850.0 | -0.65 | 6.63 | 9/9 | LOUD |
| BM_RangeForFiveComponents/10000 | 67,775.1 | 70,895.8 | +4.60 | 8.17 | 9/9 | LOUD |
| BM_RangeForFiveComponents/100000 | 706,742.9 | 722,224.4 | +2.19 | 6.23 | 9/9 | LOUD |
| BM_RangeForFiveComponents/1000000 | 7,146,874.1 | 7,395,720.5 | +3.48 | 4.94 | 9/9 | NOISY |
| BM_RangeForTwoComponentsHalf/10000 | 11,827.0 | 11,789.1 | -0.32 | 5.48 | 9/9 | LOUD |
| BM_RangeForTwoComponentsHalf/100000 | 124,664.9 | 123,445.9 | -0.98 | 3.03 | 9/9 | PASS |
| BM_RangeForTwoComponentsHalf/1000000 | 1,369,519.9 | 1,437,361.8 | +4.95 | 28.85 | 9/9 | LOUD |
| BM_RemoveComponents/10000 | 607,514.6 | 598,059.6 | -1.56 | 22.47 | 9/9 | LOUD |
| BM_RemoveComponents/100000 | 5,292,645.0 | 5,445,580.0 | +2.89 | 9.20 | 9/9 | LOUD |
| BM_RemoveComponents/1000000 | 55,433,536.4 | 57,042,809.1 | +2.90 | 10.55 | 9/9 | LOUD |
| BM_SystemScheduler_CustomExecutor/10000 | 10,703.3 | 10,614.0 | -0.83 | 6.36 | 9/9 | LOUD |
| BM_SystemScheduler_CustomExecutor/100000 | 109,623.0 | 109,875.9 | +0.23 | 7.38 | 9/9 | LOUD |
| BM_SystemScheduler_Lambda/10000 | 10,639.0 | 10,826.3 | +1.76 | 11.61 | 9/9 | LOUD |
| BM_SystemScheduler_Lambda/100000 | 109,514.1 | 110,431.3 | +0.84 | 6.05 | 9/9 | LOUD |
| BM_SystemScheduler_ManyIndependent | 11,921.3 | 12,113.6 | +1.61 | 7.56 | 9/9 | LOUD |
| BM_SystemScheduler_Parallel/10000 | 31,283.5 | 31,459.1 | +0.56 | 4.72 | 9/9 | PASS |
| BM_SystemScheduler_Parallel/100000 | 152,205.8 | 151,001.6 | -0.79 | 3.69 | 9/9 | PASS |
| BM_SystemScheduler_Sequential/10000 | 12,369.1 | 12,863.2 | +3.99 | 3.86 | 9/9 | NOISY |
| BM_SystemScheduler_Sequential/100000 | 123,804.5 | 124,456.5 | +0.53 | 8.23 | 9/9 | LOUD |
| BM_SystemScheduler_WithDependencies | 33,994.7 | 34,581.0 | +1.72 | 7.91 | 9/9 | LOUD |

`S4 GATE: 40 compared, 6 PASS, 4 NOISY, 30 LOUD, 0 FAIL, 0 missing on branch, 0 branch-only without base`

**What the numbers say (medians of 9 interleaved reps).**
- Stable positive sign, open (reported, not passed):
  - `AddRemoveByID`: +7.2 / +6.3 / +8.6 % vs 975cdb7 and +6.1 / +6.5 / +7.7 % vs c2dcf2f (N = 1000 / 10000 / 100000). Earlier rounds measured +4..7 %. A bisect (fix round 1, 8 rounds, vs c2dcf2f, `AddRemoveByID/10000`) steps at `7d3ca83` +2.5, `22c0ced` +3.5, `3b28ca5` +3.6, `4af5f20` +9.2, `2f726e8` +6.6, `7946eff` +5.6 %. The hot functions are textually unchanged from `7d3ca83` on, and an experiment that deleted every Sparse/Universal routing check from the by-id paths recovered nothing (fix round 2), so the residue is per-move additions plus code layout, not the routing checks. A Registry-local "may have sparse or universal" bool would be unsound: the flag lives on a ComponentRegistry that registries can share.
  - `CommandBufferAddRemove`: +4.6 / +3.9 / +3.8 % vs 975cdb7, +3.9 / +3.5 / +4.6 % vs c2dcf2f (N = 1000 / 10000 / 100000).
  - `RangeForFiveComponents`: +4.9 / +1.3 / +3.7 % vs 975cdb7, +4.6 / +2.2 / +3.5 % vs c2dcf2f (N = 10K / 100K / 1M). The same residue is present at `dfdfada`, before the fix rounds.
  - `CreateEntities`: +4.6 / +3.4 / +7.7 % vs 975cdb7 (10K / 100K / 1M); vs c2dcf2f +0.5 / +2.8 / +7.7 %. The 1M row is the largest and was not seen in the earlier rounds; it is not bisected.
  - `RemoveComponents`: +3.5 % at 1M vs 975cdb7; +2.9 % at 100K and 1M vs c2dcf2f.
- Resolved: `IterateTwoComponentsHalf` (fix round 2): +1.0 / +0.3 % vs 975cdb7 (100K / 1M). Vs c2dcf2f 100K reads +2.7 % NOISY (spread 2.35 %).
- `IterateTwoComponentsOne` is about 10 ns per call (a zero-work body). It reads +4.3..+5.2 % vs c2dcf2f and +0.0..+0.2 % vs 975cdb7, i.e. the two bases differ by about 0.5 ns on this row; recorded, not argued.
- FAIL rows. The 3-round gate itself lists 10 FAIL rows vs 975cdb7 and 11 vs c2dcf2f (the two summary lines above), from a pass whose round-2 branch run was contaminated (up to 9 `cl`). The 9-rep interleaved re-measure covered the families of all of them except `ParallelIterateSingleComponent/100000` (+2.07 %, spread 0.97 %) and `ParallelIterateThreeComponents/100000` (+2.19 %, spread 0.64 %) vs 975cdb7, which were not re-measured and stay unjudged (next block); each of those two is a near-threshold delta on a single 3-round median. In the re-measure no row is a FAIL: every row above 2 % reads NOISY (spread over half the delta) or LOUD (unjudgeable). That is a statement about this window, not a pass, and it covers only the re-measured families.

**Flagged, not re-measured, unjudged (fix round 4, review I2).** These rows are non-PASS in this gate (vs 975cdb7 and/or c2dcf2f) and are not among the 40 re-measured rows. They are neither passed nor failed. Format: `row` delta vs 975cdb7, verdict (base spread %) / delta vs c2dcf2f, verdict (base spread %), from `gate_975.md` / `gate_c2.md` (the tables above).

With a flagged delta above 2 % (23 rows; the candidates for an open regression):
- `AddComponentsBatch/10000` +2.07 % NOISY (2.88) / -2.09 % PASS (3.07)
- `AddComponentsBatch/100000` +2.36 % LOUD (9.10) / -2.42 % PASS (2.38)
- `CreateEntitiesBatch/10000` -0.77 % LOUD (9.75) / +6.40 % LOUD (14.48)
- `IterateFiveComponents/100000` +2.60 % NOISY (4.07) / +1.08 % PASS (2.36)
- `IterateFiveComponents/1000000` +2.11 % NOISY (2.50) / +1.18 % LOUD (6.74)
- `IterateSingleComponent/1000000` +2.88 % NOISY (3.64) / -0.34 % LOUD (6.81)
- `IterateThreeComponents/10000` +2.21 % LOUD (5.16) / +0.48 % PASS (2.61)
- `ParallelIterateFiveComponents/10000` +2.45 % NOISY (2.35) / +2.35 % LOUD (5.24)
- `ParallelIterateFiveComponents/1000000` +2.01 % LOUD (7.05) / -0.37 % LOUD (9.87)
- `ParallelIterateSingleComponent/100000` +2.07 % FAIL (0.97) / +1.51 % PASS (3.25)
- `ParallelIterateSingleComponent/1000000` +3.68 % NOISY (2.71) / +1.48 % LOUD (6.46)
- `ParallelIterateThreeComponents/10000` +6.27 % LOUD (6.98) / +6.39 % LOUD (6.40)
- `ParallelIterateThreeComponents/100000` +2.19 % FAIL (0.64) / +1.20 % PASS (3.40)
- `ParallelIterateThreeComponents/1000000` +6.63 % LOUD (5.22) / +2.25 % LOUD (9.52)
- `ParallelIterateTwoComponents/10000` +4.26 % LOUD (5.16) / +5.41 % LOUD (5.58)
- `ParallelIterateTwoComponents/1000000` +4.33 % NOISY (2.59) / +3.38 % LOUD (7.33)
- `RangeForSingleComponent/1000000` -5.49 % LOUD (9.84) / +2.62 % LOUD (13.61)
- `RangeForThreeComponents/10000` +3.25 % LOUD (7.23) / -0.12 % LOUD (10.41)
- `RangeForThreeComponents/100000` +2.40 % LOUD (9.16) / +4.79 % LOUD (7.38)
- `RangeForThreeComponents/1000000` +2.06 % LOUD (7.02) / +1.74 % LOUD (8.49)
- `RangeForTwoComponents/10000` +3.67 % NOISY (3.82) / -0.59 % PASS (1.98)
- `RemoveComponentsBatch/10000` +2.42 % LOUD (10.55) / +0.99 % PASS (2.89)
- `RemoveComponentsBatch/100000` +2.83 % NOISY (3.91) / -2.66 % LOUD (6.79)

Flagged with a delta at or below 2 %, or a large win read in a LOUD window (25 rows; not candidate regressions, but not passes either):
- `AddComponents/10000` -1.17 % LOUD (5.33) / +0.33 % PASS (2.03)
- `AddComponentsBatch/1000000` -1.85 % LOUD (7.03) / -2.59 % PASS (4.22)
- `CreateEntitiesBatch/100000` -4.70 % LOUD (5.94) / -4.95 % PASS (3.79)
- `CreateEntitiesBatch/1000000` +0.25 % LOUD (9.96) / -1.95 % LOUD (13.38)
- `ForEachAncestor/1000` -42.12 % LOUD (6.97) / -40.82 % LOUD (7.75)
- `ForEachChild/1000` -40.40 % PASS (4.47) / -38.68 % LOUD (5.58)
- `ForEachChild/100000` -41.43 % PASS (3.44) / -40.19 % LOUD (12.32)
- `ForEachDescendant/10000` -44.73 % LOUD (6.45) / -43.57 % LOUD (7.31)
- `ForEachDescendant/100000` -41.55 % PASS (4.21) / -38.86 % LOUD (5.18)
- `ForEachLink/1000` -38.93 % LOUD (6.80) / -41.30 % PASS (3.10)
- `ForEachLink/10000` -40.43 % LOUD (6.36) / -42.17 % PASS (1.52)
- `ForEachLink/100000` -46.38 % LOUD (12.03) / -41.70 % LOUD (5.78)
- `GetComponent/10000` -32.11 % PASS (4.65) / -33.19 % LOUD (7.63)
- `GetComponent/100000` -30.44 % LOUD (7.50) / -26.77 % PASS (4.86)
- `GetComponent/1000000` -27.80 % PASS (2.51) / -30.94 % LOUD (7.38)
- `IterateThreeComponents/100000` -2.01 % LOUD (5.01) / -0.88 % PASS (0.70)
- `IterateThreeComponents/1000000` -3.73 % LOUD (8.08) / +0.20 % PASS (2.86)
- `ParallelForEachDescendant/1000` -28.65 % LOUD (7.24) / -25.56 % LOUD (7.10)
- `ParallelForEachDescendant/10000` -7.33 % LOUD (7.37) / -6.43 % LOUD (5.22)
- `ParallelIterateFiveComponents/100000` +1.60 % PASS (4.19) / -0.43 % LOUD (6.33)
- `ParallelIterateSingleComponent/10000` +1.58 % PASS (1.03) / +1.39 % LOUD (10.10)
- `ParallelIterateTwoComponentsOne/100000` -9.53 % PASS (4.68) / -2.85 % LOUD (5.25)
- `RangeForSingleComponent/100000` -1.02 % PASS (2.81) / -0.37 % LOUD (6.59)
- `RangeForTwoComponents/1000000` -3.12 % LOUD (9.61) / -5.32 % LOUD (6.99)
- `RemoveComponentsBatch/1000000` -3.32 % LOUD (7.60) / -3.42 % LOUD (6.32)

Fix round 4 tried to re-measure the 13 families of the first list (39 rows, all sizes; 9 reps interleaved per rep, 975cdb7 / c2dcf2f / `0b5bd45`, base exports rebuilt with the two carried benchmarks). It found no quiet window inside its 90-minute budget. The foreign load was a game client, not a compile, so the `cl`/`link` probe could not see it; total CPU showed it:
- 16:08: round 1 started at 61-66 % total CPU; it was stopped during its second leg and the data discarded.
- 16:11-17:19 (`waitquiet.ps1`, 5 x 2 s samples about every 30 s; quiet = no `cl`/`link` and mean <= 25 %, twice in a row): 136 checks, 133 loud (mean total CPU median 52 %, 10th-90th percentile 47-58 %, max 85 %). The only quiet check before 17:18:58 was a single one at 16:43:22, between two games (the next one started at 16:43:37).
- 17:20: round 1 started quiet (12-21 %), but a new game process started at 17:22:18, inside the round and 38 s before its branch leg. The run was stopped and the whole round discarded.
- 17:31: still loud (a 2-row smoke run measured 58 % mean foreign CPU during its leg).

A 9-round run needs about 40 quiet minutes (one round = three legs = about 4.3 min). From fix round 4 on, `ab.ps1` also logs FOREIGN CPU per leg (total CPU minus the benchmark's own share) and redoes a leg above a threshold, so a non-compile load is visible in the log.

**Fix-round history (S4 part 1).**
- Round 1: Relations iteration -40..-47 % vs both bases, typed `GetComponent` -30 %; range-for +22..55 % found by the bisect and fixed (the hot loop called `TypeID<T>::Value()` out of line); `IterateTwoComponentsHalf` +13..18 % found, not fixed.
- Round 2: `IterateTwoComponentsHalf` root cause was escape analysis: any out-of-line call that receives the user's functor makes a `[&counter]` capture escape for the whole caller, so the hot Table loop kept its counter in memory. Fixed by splitting the storage-path chunk visit so that no out-of-line function ever receives the functor. The by-id residue was not recovered.
- Round 3: `NextStorageEntity` and `InvokeStorageEntity` now share one `FetchAndStampEntity` (no functor argument). Codegen-neutral: 8 interleaved rounds, `818cee9` (`base`) vs `94f4346` (`fix`), quiet (CPU 8-22 %, no `cl`/`link`):

```
row                                                     base         fix  spread0%     dfix%
BM_IterateFiveComponents/10000                      64,478.7    65,446.9       4.7      +1.5
BM_IterateFiveComponents/100000                    674,234.2   681,878.7       4.0      +1.1
BM_IterateFiveComponents/1000000                 6,980,345.5 6,910,070.0       6.2      -1.0
BM_IterateTwoComponentsHalf/10000                    9,995.4    10,028.5       3.8      +0.3
BM_IterateTwoComponentsHalf/100000                 107,332.0   107,852.8       5.6      +0.5
BM_IterateTwoComponentsHalf/1000000              1,272,236.6 1,251,702.7       7.4      -1.6
BM_Storage_FilteredIter_Sparse/100000/1              8,817.0     8,691.0      11.5      -1.4
BM_Storage_FilteredIter_Sparse/100000/50           251,637.4   251,033.6       8.3      -0.2
BM_Storage_FilteredIter_Sparse/100000/99           408,900.1   411,262.9       5.3      +0.6
BM_Storage_SoleIter_Sparse/100000/1                  8,610.1     8,486.1       4.5      -1.4
BM_Storage_SoleIter_Sparse/100000/50               422,173.7   417,283.5       4.7      -1.2
BM_Storage_SoleIter_Sparse/100000/99               816,894.2   802,856.9      10.3      -1.7
```

### S4 (2) and (3): storage benchmarks (Google Benchmark, 3 repetitions, median)

Measured at `94f4346`. The `FilteredIter_Sparse` rows below predate fix round 4 (`0b5bd45`), which made them 11-35 % faster (see "Fix round 4" below). The churn rows that S4 (2) and (3) judge do not run the iteration visit and are unaffected.

| benchmark | median real time | unit |
|---|---|---|
| BM_Storage_IdentityChurn_Table/10000 | 801,466.7 | ns |
| BM_Storage_IdentityChurn_Table/100000 | 8,186,437.8 | ns |
| BM_Storage_IdentityChurn_Universal/10000 | 655,746.8 | ns |
| BM_Storage_IdentityChurn_Universal/100000 | 6,679,687.5 | ns |
| BM_Storage_AddRemoveChurn_Table/1000 | 68,297.7 | ns |
| BM_Storage_AddRemoveChurn_Table/10000 | 633,077.9 | ns |
| BM_Storage_AddRemoveChurn_Table/100000 | 6,723,397.8 | ns |
| BM_Storage_AddRemoveChurn_Sparse/1000 | 43,381.6 | ns |
| BM_Storage_AddRemoveChurn_Sparse/10000 | 430,518.5 | ns |
| BM_Storage_AddRemoveChurn_Sparse/100000 | 4,243,437.5 | ns |
| BM_Storage_FilteredIter_Table/100000/1 | 420.2 | ns |
| BM_Storage_FilteredIter_Table/100000/50 | 22,623.5 | ns |
| BM_Storage_FilteredIter_Table/100000/99 | 50,029.3 | ns |
| BM_Storage_FilteredIter_Sparse/100000/1 | 8,522.0 | ns |
| BM_Storage_FilteredIter_Sparse/100000/50 | 250,705.9 | ns |
| BM_Storage_FilteredIter_Sparse/100000/99 | 404,785.4 | ns |
| BM_Storage_SoleIter_Table/100000/1 | 390.9 | ns |
| BM_Storage_SoleIter_Table/100000/50 | 17,295.6 | ns |
| BM_Storage_SoleIter_Table/100000/99 | 35,305.0 | ns |
| BM_Storage_SoleIter_Sparse/100000/1 | 8,403.4 | ns |
| BM_Storage_SoleIter_Sparse/100000/50 | 438,397.2 | ns |
| BM_Storage_SoleIter_Sparse/100000/99 | 792,907.5 | ns |
| BM_Storage_SparseDrivenWrite_Serial/100000/50/real_time | 408,350.1 | ns |
| BM_Storage_SparseDrivenWrite_Serial/1000000/50/real_time | 4,666,487.8 | ns |
| BM_Storage_SparseDrivenWrite_Parallel/100000/50/real_time | 73,198.7 | ns |
| BM_Storage_SparseDrivenWrite_Parallel/1000000/50/real_time | 1,006,199.4 | ns |
| BM_Storage_RandomGet_Table/10000 | 65,842.6 | ns |
| BM_Storage_RandomGet_Table/100000 | 1,263,344.8 | ns |
| BM_Storage_RandomGet_Table/1000000 | 60,679,018.2 | ns |
| BM_Storage_RandomGet_Sparse/10000 | 166,928.5 | ns |
| BM_Storage_RandomGet_Sparse/100000 | 2,739,437.3 | ns |
| BM_Storage_RandomGet_Sparse/1000000 | 101,253,677.8 | ns |
| BM_Storage_RandomGet_Universal/10000 | 189,179.7 | ns |
| BM_Storage_RandomGet_Universal/100000 | 2,504,741.6 | ns |
| BM_Storage_RandomGet_Universal/1000000 | 74,747,671.4 | ns |

S4(2) identity churn N=10000: universal 655,747 vs table 801,467 (18.2% faster) -> PASS
S4(2) identity churn N=100000: universal 6,679,688 vs table 8,186,438 (18.4% faster) -> PASS
S4(3) add/remove churn N=1000: sparse 43,382 vs table 68,298 (36.5% faster) -> PASS
S4(3) add/remove churn N=10000: sparse 430,518 vs table 633,078 (32.0% faster) -> PASS
S4(3) add/remove churn N=100000: sparse 4,243,437 vs table 6,723,398 (36.9% faster) -> PASS
S4 STORAGE ACCEPTANCE: PASS

Repetition spread of the churn rows (min / median / max over 3 reps): `IdentityChurn_Table/100000` 7.88 / 8.19 / 8.26 ms (4.6 %), `Universal/100000` 6.51 / 6.68 / 6.68 ms (2.6 %); `AddRemoveChurn_Table/1000` and `Sparse/1000` 5.1 %, all others <= 3.5 %. The wins (18-37 %) are well above that. No per-process load log was kept for this run or for the hand-rolled rounds (both finished by 07:06, after the quiet 9-rep re-measure ended 07:01; the Google Benchmark spread above is the only noise evidence); the margins (18-37 %) are far above it.

Workloads:
- `IdentityChurn` is one add + one remove of `Velocity` per entity, while a 48 B `std::string`-bearing Identity sits on every entity, as Table or Universal.
- `AddRemoveChurn` is one add + one remove of a 16 B component.
- `FilteredIter` is `View<Position, S>` over 100K entities, with S on 1/50/99 % (scattered).
- `SoleIter` is `View<S>` at the same densities; the Sparse variant is sparse-driven.
- `SparseDrivenWrite` is an untracked non-const Sparse yield, serial vs `ParallelForEach` on a worker pool.
- `RandomGet` is a shuffled `GetComponent` per kind.

Caveats:
- Compare raw `real_time` across kinds, never items/s: `FilteredIter` counts `count` items, `SoleIter` counts `matched`.
- In the Table iteration variants the non-holders sit in a separate Position-only archetype and are skipped by chunk, so Table iterates only matched entities (its best case). The Sparse variants walk the shared archetype and filter 64 entities per word.
- `SparseDrivenWrite_Parallel` shows scaling vs serial (408 us -> 73 us at 100K, 4.67 ms -> 1.01 ms at 1M); it does not prove contention absent (the coarse stamp is per visited chunk).
- Only the Google Benchmark numbers feed S4. The hand-rolled harness below builds fresh state each round, so first-touch page and chunk allocation is inside its timed region (Sparse pays page allocation every round).

**Sparse iteration since `dfdfada` (fix round 2: run cursor + visit split; re-review m3): `dfdfada` vs `94f4346`, 8 interleaved rounds, quiet (CPU 9-29 %, no `cl`/`link`).** `dfdfada` is the tree before fix round 2 (`ce21c91` run cursor + force-inlined visit, `4302024`, `1c1c531` visit split, `818cee9`); the cost below is not isolated to one of those commits. The split visit makes archetype-driven Sparse/Universal walks pay one out-of-line call per visited entity and one per chunk. This is the state BEFORE fix round 4 (next block):

```
row                                                  dfdfada         fix  spread0%     dfix%
BM_Storage_FilteredIter_Sparse/100000/1              7,840.0     8,656.3       4.3     +10.4
BM_Storage_FilteredIter_Sparse/100000/50           197,827.7   249,914.4       4.9     +26.3
BM_Storage_FilteredIter_Sparse/100000/99           281,026.2   406,566.1       3.8     +44.7
BM_Storage_FilteredIter_Table/100000/1               1,494.7       421.6       3.4     -71.8
BM_Storage_FilteredIter_Table/100000/50             70,691.9    22,664.0       3.1     -67.9
BM_Storage_FilteredIter_Table/100000/99            141,198.0    47,890.1       2.6     -66.1
BM_Storage_SoleIter_Sparse/100000/1                  8,526.0     8,534.5       9.1      +0.1
BM_Storage_SoleIter_Sparse/100000/50               422,615.0   415,248.7       9.0      -1.7
BM_Storage_SoleIter_Sparse/100000/99               827,194.1   805,578.0      11.8      -2.6
BM_Storage_SoleIter_Table/100000/1                   1,493.3       389.2       3.1     -73.9
BM_Storage_SoleIter_Table/100000/50                 72,364.4    17,497.0       2.8     -75.8
BM_Storage_SoleIter_Table/100000/99                145,517.8    35,437.8       3.4     -75.6
```

- `FilteredIter_Sparse` is +10.4 / +26.3 / +44.7 % slower at 1 / 50 / 99 % density. `SoleIter_Sparse` is unchanged (+0.1 / -1.7 / -2.6 %, base spread 9-12 %); it is sparse-driven and goes through the inline `InvokeStorageEntity`.
- The Table rows of the same benchmarks are 66-76 % FASTER on `94f4346` than on `dfdfada` (consistent with the round-2 functor-escape fix; not separately disassembled).
- S4 sets no numeric bar for Sparse iteration ("recorded"). The split is only needed where the storage visit is the cold branch of a pure-Table view; fix round 4 gives Sparse-argument views an inline body instead (next block).

**Fix round 4 (`0b5bd45`, Task 42 review I1): Sparse-argument views take an inline storage visit.** A view with a Sparse argument never instantiates the Table loop, so its storage visit is its hot path and the split bought it nothing. It now runs `BeginStorageChunk` (out of line, once per chunk) and then a `MatchingRunCursor` loop that calls `InvokeStorageEntity` (the shared `FetchAndStampEntity` sequence) inline per entity. The split NOINLINE `NextStorageEntity` visit is kept only for the cold `m_anyUniversal` arms of Table-only views. 8 interleaved rounds (order rotated per round), `e542abf` (the tree before this fix) vs `0b5bd45` vs `dfdfada`, 15:55-16:03, quiet (load log first; no `cl`/`link`, CPU 9-19 %); columns are medians over the 8 rounds, `spread0%` is `e542abf`'s (max-min)/median, `sp_X%` is X's own spread, `d_X%` is X vs `e542abf`:

```
round 1 start 2026-10-25T15:55:41 cl/link=0 msbuild(cpu-s)=[] cpu=14,10,10
round 2 start 2026-10-25T15:56:39 cl/link=0 msbuild(cpu-s)=[] cpu=10,9,18
round 3 start 2026-10-25T15:57:36 cl/link=0 msbuild(cpu-s)=[] cpu=16,10,10
round 4 start 2026-10-25T15:58:35 cl/link=0 msbuild(cpu-s)=[] cpu=19,17,11
round 5 start 2026-10-25T15:59:33 cl/link=0 msbuild(cpu-s)=[] cpu=11,16,12
round 6 start 2026-10-25T16:00:30 cl/link=0 msbuild(cpu-s)=[] cpu=10,13,15
round 7 start 2026-10-25T16:01:28 cl/link=0 msbuild(cpu-s)=[] cpu=11,9,11
round 8 start 2026-10-25T16:02:25 cl/link=0 msbuild(cpu-s)=[] cpu=10,13,15
row                                                        e542abf       0b5bd45       dfdfada  spread0% sp_0b5bd45% d_0b5bd45% sp_dfdfada% d_dfdfada%
BM_IterateFiveComponents/10000                            64,375.5      64,031.8      64,022.3       2.6       3.8      -0.5       3.1      -0.5
BM_IterateFiveComponents/100000                          665,239.5     669,612.3     666,668.3       4.7       4.0      +0.7       4.8      +0.2
BM_IterateFiveComponents/1000000                       6,778,036.6   6,763,324.1   6,780,779.5       2.9       6.1      -0.2       2.1      +0.0
BM_IterateTwoComponentsHalf/10000                          9,874.3       9,788.9      11,502.5       1.8      11.8      -0.9       4.7     +16.5
BM_IterateTwoComponentsHalf/100000                       105,410.7     103,403.8     120,310.0       3.2       3.5      -1.9       1.4     +14.1
BM_IterateTwoComponentsHalf/1000000                    1,224,850.2   1,197,008.8   1,312,115.8      11.1       8.6      -2.3       7.7      +7.1
BM_Storage_FilteredIter_Sparse/100000/1                    8,596.9       7,620.2       7,732.1       2.8       7.3     -11.4       4.5     -10.1
BM_Storage_FilteredIter_Sparse/100000/50                 245,347.0     192,985.3     197,196.7       4.3       4.1     -21.3       5.1     -19.6
BM_Storage_FilteredIter_Sparse/100000/99                 400,495.8     258,579.0     282,637.8       6.7       3.2     -35.4       5.7     -29.4
BM_Storage_FilteredIter_Table/100000/1                       415.3         412.6       1,455.7       2.1       6.8      -0.7       3.2    +250.5
BM_Storage_FilteredIter_Table/100000/50                   21,908.7      21,834.2      69,314.7       6.3      11.7      -0.3       3.5    +216.4
BM_Storage_FilteredIter_Table/100000/99                   46,582.5      47,689.0     137,303.3       5.8       9.3      +2.4       2.4    +194.8
BM_Storage_SoleIter_Sparse/100000/1                        8,309.8       8,335.6       8,246.5      23.7       3.0      +0.3       4.9      -0.8
BM_Storage_SoleIter_Sparse/100000/50                     405,762.3     424,461.1     415,379.9       5.3       3.9      +4.6       4.4      +2.4
BM_Storage_SoleIter_Sparse/100000/99                     791,525.8     837,190.8     808,809.2       7.7       4.6      +5.8       1.9      +2.2
BM_Storage_SoleIter_Table/100000/1                           382.2         383.9       1,459.8       1.8      16.1      +0.5       2.8    +282.0
BM_Storage_SoleIter_Table/100000/50                       17,237.9      17,571.7      70,839.6       4.3       7.6      +1.9       3.9    +311.0
BM_Storage_SoleIter_Table/100000/99                       34,943.1      35,038.8     141,406.4       4.5       3.1      +0.3       3.3    +304.7
```

- `FilteredIter_Sparse` is back to `dfdfada` or better: -11.4 / -21.3 / -35.4 % vs `e542abf` at 1 / 50 / 99 % (and 1.4 / 2.1 / 8.5 % faster than `dfdfada`). Two earlier 8-round A/Bs of the same binaries agree (-12.7 / -21.2 / -36.4 % and -11.2 / -21.6 / -34.4 %).
- The pure-Table rows do not move, as expected (a pure-Table view does not instantiate the changed arm): `IterateFiveComponents` -0.5 / +0.7 / -0.2 %, `IterateTwoComponentsHalf` -0.9 / -1.9 / -2.3 %, the `_Table` storage rows -0.7..+2.4 % with the branch's own spreads at 3-16 %.
- `SoleIter_Sparse` is sparse-driven (`VisitSparseRange`), not this visit. Over four A/Bs it read +0.3 / +4.6 / +5.8 % (above), +1.0 / +3.8 / +1.3 %, +0.8 / +1.0 / +0.2 % and, in a focused 12-round run, +1.4 / +1.1 / +2.4 % (spreads 6-13 %: LOUD under the gate rule). The disassembly of `VisitSparseRange<..SoleIter<ChurnS>..>` is instruction-for-instruction identical in `e542abf` and `0b5bd45` (159 lines each, addresses stripped); only its placement differs (`...F120` vs `...0840`, a different 64-byte offset), and the calling `SoleIter<ChurnS>` grew (393 -> 600 lines) because its never-taken archetype fallback now inlines the visit. So no per-entity work changed; the small positive reading is recorded as layout/noise, not claimed as zero.
- In `FilteredIter<ChurnS>` the two `NextStorageEntity` calls are gone (`BeginStorageChunk` is the only storage-visit call left; the cold `SparseSet::Find` / `UniversalColumn` lookups of the fetchers now inline).
- **Universal-member views keep the split cost by necessity.** A Table-only view with a Universal member in its registry runs the split visit (its storage visit is the cold branch of a view that also instantiates the Table loop, where an out-of-line call receiving the functor would make captures escape, fix round 2). That is the Arcane `Identity` adoption path. No committed benchmark iterates such a view, so its per-entity cost is not measured here; by construction it is the same one-out-of-line-call-per-entity shape `FilteredIter_Sparse` paid at `e542abf`.

**Enableable Table filtered walk (re-review m2): `dfdfada` vs `94f4346`, 8 interleaved rounds, quiet (CPU 10-31 %, no `cl`/`link`).** No committed benchmark iterates an enableable Table view, and `ForEachEnabledRun` was rebuilt on `MatchingRunCursor` in round 2. A scratch benchmark (not in the committed suite): `View<Position, EnV>::ForEach` over 100K entities with `EnV` enableable and disabled on 1 / 10 / 50 / 90 / 99 % scattered, and in clustered runs of 256, built standalone from each tree's headers with the Release flags above. The original source was deleted with `bin\fix3`; fix round 4 recreated it from this definition as `.superpowers/sdd/2026-10-01-astra-storage-policies/evidence-task42/m2_enableable_table.cpp` (local evidence, not committed; build line in its header; it compiles and lists the six rows at `0b5bd45`, but it is not byte-identical to the binary measured below):

```
row                                                  dfdfada         fix  spread0%     dfix%
BM_EnableableTableClustered                         68,847.1    67,678.9       3.5      -1.7
BM_EnableableTableFiltered/1                       142,214.9   138,543.9       4.1      -2.6
BM_EnableableTableFiltered/10                      137,338.9   133,595.8       2.7      -2.7
BM_EnableableTableFiltered/50                      212,249.7   202,031.7       4.3      -4.8
BM_EnableableTableFiltered/90                       40,775.7    40,388.9       9.5      -0.9
BM_EnableableTableFiltered/99                        3,643.7     2,844.9      13.9     -21.9
```

The cursor-based walk is not slower on any shape (-0.9 to -4.8 %; the 99 % row's -21.9 % has a 13.9 % base spread).

### Hand-rolled harness (`bench_storage.exe`, 6 rounds, median [min, max] ns/op)

A second view of the same workloads; it does not feed S4.

| op | N | median ns/op | min | max | rounds |
|---|---|---|---|---|---|
| addremove_sparse | 1000 | 30.850 | 30.000 | 32.050 | 6 |
| addremove_sparse | 10000 | 29.675 | 28.485 | 30.100 | 6 |
| addremove_sparse | 100000 | 29.191 | 28.046 | 29.610 | 6 |
| addremove_table | 1000 | 41.100 | 39.750 | 43.000 | 6 |
| addremove_table | 10000 | 36.995 | 35.785 | 43.205 | 6 |
| addremove_table | 100000 | 37.079 | 35.874 | 38.225 | 6 |
| filter_sparse_1 | 1000000 | 0.170 | 0.158 | 0.186 | 6 |
| filter_sparse_50 | 1000000 | 2.601 | 2.524 | 2.645 | 6 |
| filter_sparse_99 | 1000000 | 3.992 | 3.914 | 4.136 | 6 |
| filter_table_1 | 1000000 | 0.005 | 0.005 | 0.005 | 6 |
| filter_table_50 | 1000000 | 0.379 | 0.314 | 0.474 | 6 |
| filter_table_99 | 1000000 | 1.155 | 0.987 | 1.228 | 6 |
| identity_churn_table | 100000 | 47.718 | 45.563 | 49.632 | 6 |
| identity_churn_universal | 100000 | 36.169 | 35.409 | 36.859 | 6 |
| random_get_sparse | 1000000 | 109.529 | 106.497 | 128.267 | 6 |
| random_get_table | 1000000 | 67.522 | 61.816 | 74.974 | 6 |
| random_get_universal | 1000000 | 103.760 | 93.566 | 111.761 | 6 |
| sole_sparse_1 | 1000000 | 0.146 | 0.129 | 0.161 | 6 |
| sole_sparse_50 | 1000000 | 4.364 | 4.138 | 4.545 | 6 |
| sole_sparse_99 | 1000000 | 7.229 | 7.002 | 7.443 | 6 |
| sole_table_1 | 1000000 | 0.004 | 0.004 | 0.004 | 6 |
| sole_table_50 | 1000000 | 0.233 | 0.217 | 0.325 | 6 |
| sole_table_99 | 1000000 | 0.645 | 0.561 | 0.742 | 6 |

ns/op is defined per op:
- `identity_churn_*`, `addremove_*`: per entity, for one add + one remove;
- `filter_*`: per scanned entity (1M);
- `sole_*`: per registry entity (1M);
- `random_get_*`: per lookup.

### Reading the numbers

- **Churn (Sparse).** Sparse add/remove churn is 32-37 % faster than Table at every measured size (43.4 us vs 68.3 us at 1K, 431 us vs 633 us at 10K, 4.24 ms vs 6.72 ms at 100K); the hand-rolled harness agrees (29-31 vs 37-41 ns per add+remove).
- **Iteration and lookup (Table stays ahead).** Dense iteration is a Table strength: `FilteredIter` at 99 % is 47.7 us (Table) vs 259 us (Sparse) at `0b5bd45` (fix-round-4 A/B above, median of 8 rounds; the 3-rep storage run at `94f4346` read 50.0 vs 405 us, before the inline Sparse visit), `SoleIter` at 99 % is 35.3 us vs 793 us (storage run; that hot loop is codegen-identical at `0b5bd45`). At 1 % the Table variants are still far ahead (0.41 us vs 7.6 us at `0b5bd45`) because they skip the non-holder archetype wholesale. Random `GetComponent` at 100K is 1.26 ms (Table) vs 2.74 ms (Sparse) vs 2.50 ms (Universal); at 1M, 67.5 / 109.5 / 103.8 ns per lookup.
- **Universal.** A 48 B string-bearing Identity as Universal makes structural churn 18.2 % (10K) and 18.4 % (100K) faster than keeping it in the archetype (0.656 vs 0.801 ms; 6.68 vs 8.19 ms), and 24 % faster per entity in the hand-rolled harness (36.2 vs 47.7 ns).
- **Table-only users.** Not shown to be regression-free: S4 part 1 is PENDING. In the interleaved 9-rep re-measure on a shared desk, the by-id and command-buffer paths read 4-9 % slower, `RangeForFiveComponents` 1-5 %, `CreateEntities` up to +7.7 % at 1M and `RemoveComponents/1000000` +3.5 % against 975cdb7, with a stable sign; `IterateTwoComponentsHalf` is resolved. Iteration is NOT shown to be within noise: two `ParallelIterate*` rows are 3-round FAILs, and 23 flagged rows (most of them `ParallelIterate*`, `Iterate*`, `RangeFor*`) were never re-measured and are unjudged (listed above). Re-judge all of them in a clean-room window (`bench-clean.ps1`).
