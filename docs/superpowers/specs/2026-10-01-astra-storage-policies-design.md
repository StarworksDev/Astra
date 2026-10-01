# Astra Storage Policies: Sparse and Universal Component Storage (design)

**Status:** DRAFT for user review (2026-10-01). Brainstormed and approved in conversation section by section (sections 1-2 explicitly; sections 3-7 approved as a whole: "I like the whole idea, go ahead and write it all out").
**Repo / branch:** Astra, `feat/storage-policies` (worktree `D:\dev\starworks\Astra-storage`), based on `dev` @ `975cdb7`. Kept OFF `dev` until Arcane's node-page phase has vendored `975cdb7` (its task T1-C7).
**Consumers:** every Astra user (generic feature); Arcane adopts it at the start of its entity-record undo arc (s10, out of scope here).
**Evidence:** the read-only internals map produced 2026-10-01 (all `file:line` citations below are at `975cdb7`, paths relative to `include/Astra/` unless rooted at `tests/`, `docs/` or `benchmark/`).
**Notation:** `entity.Index()` / `e.Index()` below is shorthand for the entity's id; the code's accessor is `Entity::GetID()` (stores index by `static_cast<std::size_t>(e.GetID())`).

## 0. Why

Astra is a pure archetype ECS: every component lives in an archetype column, and every structural change (add/remove any component) moves EVERY column of the entity to a new chunk (`ArchetypeManager::MoveEntityInternal`, `Archetype/ArchetypeManager.hpp:1257-1287`; `Archetype::MoveEntityFrom`, `Archetype/Archetype.hpp:708-772`). Two workloads pay for that model without benefiting from it:

1. **Components on (nearly) every entity that are rarely iterated with the hot set.** Arcane's `Identity { Guid id; std::string name; }` (~48 B, non-trivially-copyable) is string-moved on every archetype transition and inflates the per-entity size of every chunk, so every hot query walks more chunks.
2. **Components that are frequently toggled, or rare.** Adding/removing them moves the whole entity and multiplies archetypes. Astra's own grading review records the EnTT gap as "model-inherent (sparse sets)" (`docs/reviews/2026-09-11-astra-grading.md:42`).

Bevy (per-component `SparseSet` storage) and Flecs (a non-fragmenting trait) address this; Astra has no per-component storage policy today (`docs/` has no design for it; the enableable spec put "sparse-set component storage" out of scope, `docs/superpowers/specs/2026-07-25-enableable-components-design.md` s13).

## 1. Decisions (from the brainstorm, binding)

- **D1 Hybrid declaration.** The component TYPE declares its storage class (Table or Sparse) with a trait. The REGISTRY declares which components are universal. Reason: storage follows a type's access pattern; "every entity has one" follows how a registry is used, and a `ComponentRegistry` can be shared by several registries (`Registry/Registry.hpp:74-83, 1313`), so universality cannot live in the descriptor.
- **D2 Storage is invisible at the API.** `View`/`Query`/`Get`/`Has`/`Add`/`Remove`/`SetEnabled`/`Changed`/`Added`/`Optional`/`Not`/`Any`/`OneOf`/`With`/`IncludeDisabled`/Relations filters/CommandBuffer all behave identically for every storage class; only performance differs. The single unavoidable exception is that a universal component cannot be removed (s3.3).
- **D3 "Option 3": out-of-table data, in-chunk presence.** Sparse data lives outside archetypes and does NOT fragment them; sparse PRESENCE is mirrored into per-chunk bitsets consumed by the existing run extractor (`Registry/EnabledRuns.hpp:29-92`, `ForEachEnabledRun`), so queries filter on sparse components word-at-a-time with whole-chunk skips.
- **Rejected:** fragmenting sparse storage (Bevy-style archetype/table split: a deep core refactor, superseded by D3); adaptive auto-promotion between layouts (non-deterministic performance, migration hitches, unsettable heuristics).

## 2. Goals, non-goals, success criteria

**Goals.** G1 three layouts (Table, Sparse, Universal) behind one API; G2 no behaviour difference observable through the public API (D2), proven by a storage-parity test suite (s9.1); G3 cheaper structural changes for entities carrying Sparse/Universal components; G4 no regression for registries that use only Table components; G5 archives load regardless of the storage class in force when they were written (storage-agnostic load, s8.3).

**Non-goals.** Arcane adoption (s10); an entity clone/prefab API (none exists today, map s8); a GUID index (Arcane-side, entity-record arc); fragmenting sparse; auto-promotion; JSON registry serialization (none exists today).

**Success criteria.**
- S1 Every existing Astra test stays green with no edits beyond the deliberate ones named in this spec.
- S2 The storage-parity suite (s9.1) runs the same behavioural test bodies over Table, Sparse and Universal variants and is green.
- S3 The invariant fuzzer (s9.2) runs green for the configured iteration count on every commit of the branch's final state.
- S4 Benchmarks (s9.3): pure-Table benchmarks within 2% of `975cdb7` (noise-checked, 3 runs); a 48-byte non-trivial component made Universal removes it from archetype moves (structural-change benchmark improves measurably, recorded); Sparse add/remove churn beats Table add/remove at every measured size.

## 3. The model (s1 of the brainstorm)

### 3.1 Three layouts

| Layout | Data | In archetype `ComponentMask`? | Presence | Removable |
|---|---|---|---|---|
| **Table** (default; today) | archetype chunk column | yes (fragments) | the mask | yes |
| **Sparse** | per-component sparse set (s4.1) | **no** (never fragments) | sparse set = truth; per-chunk bit mirror (s5) | yes |
| **Universal** | per-component slot-indexed paged column (s4.2) | **no** | always present | **no** |

### 3.2 Declaration

- **Type trait (D1).** Mirrors `AstraEnableable` / `AstraChangeTracked` (`Component/Component.hpp:49-91`):
  ```cpp
  namespace Astra { enum class Storage : uint8_t { Table, Sparse }; }
  struct Health { float hp; static constexpr auto AstraStorage = Astra::Storage::Sparse; };
  ```
  `StorageTraits<T>` / `StorageOfV<T>` default to `Table`, gated with `if constexpr` exactly like `EnableableTraits` (the MSVC rationale at `Component.hpp:52-57` applies). `ComponentRegistry::MakeDescriptor` (`Component/ComponentRegistry.hpp:305-394`, next to `isEnableable`/`isChangeTracked` at 348-353) writes it into a new `ComponentDescriptor::storage` field (`Component.hpp:104-154`). Both registration paths (anonymous `RegisterComponentImpl` and `ComponentModule::RegisterOneChecked`, `Component/ComponentModule.hpp:203`) share `MakeDescriptor`, so both pick it up.
- **Empty (tag) types may be Sparse.** A sparse tag has a sparse set with no data buffer (presence only); its disabled state lives in the store (s4.1).
- **Every tag may be enableable, Table tags included (parity, user 2026-10-01).** Today a tag has no column, so it cannot carry disabled bits and enableable tags are `static_assert`-refused (`ComponentRegistry.hpp:360-362`; `Archetype::BuildColumnMeta` skips `size == 0`, `Archetype.hpp:75`). An enableable Table tag now gets a **bit-only column**: an `ArchetypeColumnMeta` entry with `stride 0`, no data region, and a disabled-word region carved like any enableable column (`InitializeColumns`, `ArchetypeChunkPool.hpp:738-787`, and the capacity math `ComputeCapacityForBytes`/`ComputeLayoutBytesForCapacity`/`ChunkBytesToHold`, `Archetype.hpp:113-218`, count its words). `idToColumn` is `>= 0` for it, so `SetEnabledByID`, `IsEnabledByID`, every view filter path and every bit carry work unchanged. `GetComponentArray<T>()` still returns nullptr for it (no data), `SerializeColumn` writes zero data bytes plus the existing disabled section, and non-enableable tags keep having no column at all (no cost). The `static_assert` is deleted.
- **Universal (D1).** `Registry::Config` (`Registry/Registry.hpp:40-50`) gains
  ```cpp
  struct UniversalSpec { ComponentID id; void (*init)(Entity, void* component, void* user); void* user; };
  std::vector<UniversalSpec> universal;          // filled with config.AddUniversal<T>(initFn = nullptr, user = nullptr)
  ```
  `AddUniversal<T>` registers `T` (so its id is known) and records the optional initializer. The list is retained in `m_config` (`Registry.hpp:1918`), survives `Clear()`, and is honoured by `Load` because Load builds the new registry with the caller's `Config` (`Registry.hpp:1818-1837, 1859-1862`).
- **Universal overrides the trait.** A universal component uses the universal column in that registry whatever its trait says; a registry that does not declare it universal uses the trait's layout.
- **Sparse slots.** Each Sparse-trait component gets a dense *sparse slot* in `[0, ASTRA_MAX_SPARSE_COMPONENTS)` (default **32**, macro beside `ASTRA_MAX_COMPONENTS`, `Component.hpp:17-21`) the first time a registry stores it. Slots are per `ComponentStorage` (per registry), assigned on first use, so a shared `ComponentRegistry` does not couple slot spaces. Exceeding the cap is a registration-time assert + refusal (Release: the add returns false).

### 3.3 Per-call behaviour (the API contract, D2)

| Call | Table | Sparse | Universal |
|---|---|---|---|
| `HasComponent<T>` | mask test (`ArchetypeManager.hpp:614-621`) | sparse lookup | `true` for a live entity |
| `GetComponent<T>` (const / mut) | chunk column | dense slot via the sparse index | `column[entity.Index()]` |
| `AddComponent`/`Emplace` when absent | archetype move | sparse insert + chunk bit set; NO move | n/a (always present) |
| `AddComponent`/`Emplace` when present | no-op, returns `true` (today: `ArchetypeManager.hpp:314-315` + `Registry.hpp:337-368`); by-id `AddComponentByID` returns `false` (today's by-id answer for an already-present id, `ArchetypeManager::AddComponentByID`) | same: typed no-op `true`, by-id `false` | same: typed no-op `true`, by-id `false` (Universal is always present) |
| `RemoveComponent` when present | archetype move, `true` | sparse erase + bit clear, `true` | **refused**: `ASTRA_ASSERT` in Debug, returns `false` in Release, no signal |
| `RemoveComponent` when absent | `false` (`ArchetypeManager.hpp:373-374`) | `false` | n/a |
| `SetEnabled<T>` / `IsEnabled<T>` | per-chunk disabled bits | per-entity flag in the store + chunk mirror (s5.2) | per-slot flag in the column page + chunk mirror |
| `Modified`/`IsChanged`/`IsAdded`/`SetIfNeq` | column ticks | per-element ticks in the store | per-slot ticks in the page |
| Signals `ComponentAdded`/`ComponentRemoved`/`ComponentEnabled`/`ComponentDisabled` | as today | identical timing and payload | `ComponentAdded` at create (as for any create-with component); never `ComponentRemoved` except entity destroy semantics (which emit none today, `Registry.hpp:230-258`) |
| Reflection (`GetComponentByHash`, `HasComponentByHash`, `GetEntityComponents`, `InspectEntity`, `GetComponentByName`, `Registry.hpp:857-1099`) | mask + chunk | must include sparse/universal members | same |

Pointer validity: the public contract stays today's table rule ("valid until the next structural change"). Universal pointers are in fact stable (never move); that is an implementation property, not a promise.

Add-when-present asymmetry (plan consistency pass, 2026-10-01): the typed and by-id surfaces already answer differently for an already-present Table component (typed `true`, by-id `false`). D2 is parity with each surface's current answer, so an already-present Universal (or Sparse) component also answers typed `true`, by-id `false`. This refines s12 item 2.

Change-tick tiers (plan consistency pass): tracked Sparse/Universal components are exact per element / per slot. Untracked ones are coarse, the restatement of today's chunk-column version: one version per Sparse component (any write makes every member answer changed), and one version per 1024-slot page for a Universal component. One coarse scheme per kind; the sparse set itself keeps none.

## 4. The stores (s2 of the brainstorm)

A new `ComponentStorage` (new header `Registry/ComponentStorage.hpp`, plus `Registry/SparseSet.hpp` and `Registry/UniversalColumn.hpp`) is owned by `Registry` beside `ArchetypeManager`. It owns the sparse-slot table, every sparse set, every universal column, and a copy of the universal config. Both stores are type-erased through the `ComponentDescriptor` function pointers (`defaultConstruct`, `moveConstruct`, `destruct`, `constructWith`, `serializeVersioned`/`deserializeVersioned`; `Component.hpp:104-234`), holding the descriptor BY VALUE (the `ResourceStorage` lifetime pattern, `Component/ResourceStorage.hpp:699-702`). Like `ArchetypeManager`, it is rebuilt fresh on `Load` (`Registry.hpp:1875`).

### 4.1 Sparse set (one per Sparse component per registry)

- **Sparse half:** a paged array indexed by `entity.Index()` holding a `uint32_t` dense position (`INVALID = UINT32_MAX`). Pages of 4096 entries are allocated on first insert into their range and released when they empty (the `EntityTable` segment pattern, `Entity/EntityTable.hpp:140-143, 266-315`).
- **Dense half:** `Entity entities[]` (full handle incl. version: a stale handle misses), a stride-packed, `desc.alignment`-aligned data buffer grown geometrically (no buffer for empty types), a per-element `EntityTicks` array iff the type is change-tracked, and a per-element `uint8_t disabled` iff the type is enableable.
- **Remove:** swap-and-pop; the moved element's sparse entry is rewritten. `Has`/`Get`: two loads + a version compare.
- **Iteration:** a sparse-driven query (s6.2) walks the dense arrays in order.

### 4.2 Universal column (one per universal component per registry)

- **Layout:** a paged, slot-indexed array, position = `entity.Index()`, no indirection. Pages of 1024 slots (`ASTRA_UNIVERSAL_PAGE_SLOTS`) are allocated when an entity id first lands in their range and released when their live count drops to zero.
- **Per page:** the aligned data block, a 1024-bit `live` bitmap (which slots hold a constructed value; `Clear`, destroy and serialization walk it instead of the `EntityManager`), a live count, an `EntityTicks` array iff change-tracked, and a 1024-bit disabled bitmap iff enableable.
- **Construction order at entity creation:** (1) default-construct, or move-construct from the value passed to `CreateEntityWith`/`CreateEntitiesWith`; (2) only when no value was passed, the registry's `init(entity, ptr, user)` runs (Arcane will mint the GUID here; an explicitly supplied `Identity` is never overwritten); (3) `ComponentAdded` signal if enabled.
- **Destruction:** on `DestroyEntity`/`DestroyEntities`, `Clear`, and registry destruction.

### 4.3 Threading

Identical to tables: structural changes run on the registry's context (`AssertContextAffinity`); parallel iteration over disjoint entities may read and write their own elements (each entity owns its slot in every store). Sparse-set growth during a parallel iteration is a structural change and is refused exactly like a table structural change (`tests/Registry/IterationSafetyTest.cpp` semantics extend to sparse adds/removes).

"Refused" means exactly what it means for a Table change today: the Debug structural-mutation guard REPORTS it (`ASTRA_ASSERT` after the walk; the guard now also compares a sparse membership counter beside the archetype-set counter). The add/remove is not rejected at its call site, in any build. The walk itself stays memory-safe in every build: presence-word pointers are copied per chunk, Sparse elements are re-fetched by entity, the sparse-driven walk is index-based, and a required element that vanished mid-walk is skipped. Parity means "same outcome as Table today": the guard trips for archetype-set changes (as today) plus sparse membership changes. Destroying a bystander, or a Table move into an already-existing archetype, does not trip it today and is not made to; destroying an entity that holds a Sparse component changes sparse membership and does bump the sparse counter.

### 4.4 Module unload

A plugin `ComponentModule` can register a Sparse type and later unload (`ComponentRegistry::UnregisterModuleRange`, `ComponentRegistry.hpp:153-223`; `ReleaseModule` 555-582). Descriptor function pointers into an unloaded image dangle, so `ComponentStorage` must destroy and drop that component's sparse set or universal column (and release its sparse slot and every chunk presence block for that slot) **before** the module's descriptors are released.

No existing unload path purges archetype state for a module's components (that was a mis-reading at spec time), so the unload purge is NEW: `ComponentRegistry` gains store-release hooks (`AddStoreReleaseHook`/`RemoveStoreReleaseHook`). `ReleaseModule` and `UnregisterModuleRange` collect every departing entry (live and shadowed) under the registration lock, then call every hook OUTSIDE all locks while the departing descriptors are still installed, and only then change any slot. Every `Registry` subscribes for its lifetime; for a store built from the departing descriptor (matched by its destructor pointer) it destroys every element, drops the store, frees the sparse slot and releases that slot's presence block in every chunk. Table columns of an unloaded type are still not purged (pre-existing behaviour, out of scope).

Unloading a Universal type drops its column, so every entity loses the value and "always present" is SUSPENDED for that type until it is registered again (a later create or access rebuilds the column from the then-registered descriptor). Universal types should therefore be engine-resident; s10's Arcane adoption records this.

## 5. Chunk presence bits (D3)

### 5.1 Placement: lazy per-chunk blocks, not the chunk arena

The enableable bits live inside the chunk arena carved at chunk creation and are indexed by **column ordinal** (`ArchetypeChunk::Column`, `Archetype/ArchetypeChunkPool.hpp:821-828`; carve at 738-787; `GetDisabledWords(int column)` 528-532). A sparse component has no column, and a fixed carve per sparse slot would cost every chunk (minimum 4 KB, `ArchetypeChunkPool::Config` 858-872) space for components it never holds. So presence uses **lazily allocated side blocks**:

- `ArchetypeChunk` gains `SmallVector<SparseBlock, 2> m_sparseBlocks`, kept sorted by slot. `struct SparseBlock { uint16_t slot; uint32_t presentCount; uint32_t disabledCount; uint64_t* present; uint64_t* disabled; }`. `present`/`disabled` hold `WordsForCapacity(capacity)` words each (`ArchetypeChunkPool.hpp:730-733`); `disabled` is allocated only for enableable sparse types (and only when a bit is first set).
- A block is created the first time an entity in that chunk gains the slot's component, and freed with the chunk (`ChunkDeleter`, 885-902) or when compaction empties it. A missing block means "no entity in this chunk has it".
- Words come from a small word-block freelist owned by `ArchetypeChunkPool` (power-of-two word counts), so block churn does not hit the global allocator.
- Universal components need no `present` words (they are always present). An **enableable** Universal component does need the `disabled` mirror, so it is also assigned a slot and its blocks carry `present == nullptr` (meaning "all present") plus `disabled` words; a non-enableable Universal component has no slot and no blocks.

### 5.2 Invariants

- **P1 (presence).** For every live entity `e` located at `(chunk, i)` and every sparse slot `s`: bit `i` of `chunk.block(s).present` is set ⇔ `e` is a member of sparse set `s`.
- **P2 (counts).** `presentCount == popcount(present)` and `disabledCount == popcount(disabled)` (the enableable rule, `ArchetypeChunkPool.hpp:551-574`).
- **P3 (disabled mirror).** For enableable sparse/universal types, bit `i` of `disabled` ⇔ the store's per-element/per-slot disabled flag. The store is the truth; the chunk bits are a query-acceleration mirror.
- A Debug-only `Registry::ValidateStorageInvariants()` checks P1-P3 exhaustively; the fuzzer (s9.2) calls it after every operation.

### 5.3 Carrying bits through relocation

Every site that relocates a chunk slot already carries per-slot side data (disabled bits and ticks). Each gains the sparse-block carry through two helpers in `ArchetypeChunk`: `CarrySparseBits(srcChunk, srcIdx, dstIdx)` (copy every set bit of the source slot across all blocks, allocating destination blocks lazily) and `SwapRemoveSparseBits(idx, lastIdx)` (move the tail slot's bits into `idx`, clear the tail). The sites, from the internals map:

| Site | File:line | Existing carry it mirrors |
|---|---|---|
| `ArchetypeChunk::RemoveEntity` (swap-remove) | `ArchetypeChunkPool.hpp:382-451` | disabled 429-435, ticks 439-444 |
| `Archetype::MoveEntityFrom` | `Archetype.hpp:708-772` | 757-758 |
| `ArchetypeManager::MoveAndAdd<T>` | `ArchetypeManager.hpp:1307-1359` | 1349-1350 |
| `ArchetypeManager::MoveAndAddByID` | `ArchetypeManager.hpp:1574-1651` | 1642-1643 |
| `ArchetypeChunk::BatchMoveComponentsFrom` (via `Archetype::BatchMoveEntitiesFrom` 1763-1926) | `ArchetypeChunkPool.hpp:230-297` | 281-285 |
| `Archetype::CompactChunks` (via `Registry::Defragment` 1395-1489) | `Archetype.hpp:1329-1472` | 1425-1429 |
| `Archetype::MoveEntitiesBetweenChunks` (no callers) | `Archetype.hpp:2124-2196` | 2174-2178 |

The three caller-less functions the map found (`MoveEntitiesBetweenChunks`, `MoveEntitiesWithComponent` `ArchetypeManager.hpp:1361-1411`, `BatchMoveEntitiesWithComponentByID` 1656-1671) are **deleted** in this work rather than taught the carry (dead code that would silently break P1 if revived).

## 6. Queries (D2 + D3)

### 6.1 Classification

`QueryClassifier` (`Registry/Query.hpp:188-259`) and `QueryBuilder` (475-604) gain a per-argument **storage kind**: Table or Sparse is known at compile time from the trait; Universal is a per-registry runtime fact, so the `View` resolves each argument's effective kind once, when it is built against a registry (an `std::array<StorageKind, N>` plan). Only **Table** arguments contribute to the archetype masks used by `QueryBuilder::Matches` (571-603), the `CollectArchetypes` pre-filter count (`View.hpp:745-775`, the popcount at 754-767), and archetype caching (`EnsureArchetypes`, 709-743). The scheduler's access masks (`ViewAccess` `View.hpp:1441-1455`, `ArgAccess` `Query.hpp:320-351`) stay id-level and therefore stay correct for every kind.

### 6.2 Driver selection

| Required arguments (after modifiers) | Driver | Notes |
|---|---|---|
| at least one Table | **archetype-driven** (today's path) | sparse/universal arguments become chunk filters and per-entity fetches (6.3, 6.4) |
| no Table, at least one Sparse | **sparse-driven**: iterate the dense array of the required sparse set with the fewest members | each candidate is checked against the other filters by entity (record → archetype mask for Table modifiers; sparse lookups for Sparse ones) |
| only Universal (or none) | **archetype-driven over every archetype** | every live entity matches the universal part |

Order is unspecified for tables today; sparse-driven order is the dense order, which is deterministic for a given operation sequence.

### 6.3 Chunk filtering: one generalised run extractor

`ForEachEnabledRun(wordSets, setCount, count, fn)` (`EnabledRuns.hpp:29-92`) ORs "excluded" word sets. It is generalised (old entry point kept as a wrapper) to
```cpp
ForEachMatchingRun(const uint64_t* const* exclude, size_t nExclude,
                   const uint64_t* const* require, size_t nRequire, size_t count, Fn&& fn);
// match word = AND(require[k][w]) & ~OR(exclude[k][w]), tail-masked; maximal ascending runs as today
```
Inside `VisitChunkFiltered` (`View.hpp:1287-1388`):
- a **required Sparse** argument adds its block's `present` words to `require`; a missing block means **skip the chunk** (the existing Tier-2 skip, 1329-1330);
- **`Not<Sparse>`** adds `present` to `exclude` (missing block: no-op);
- an **enableable** Sparse/Universal required argument adds its `disabled` mirror to `exclude` unless wrapped in `IncludeDisabled` (the existing default, `Query.hpp:294-316, 401-408`);
- **`Optional<Sparse>`** is a per-entity bit test producing a pointer or null (`BindOptional` semantics, `View.hpp:563-568`);
- **`Any`/`OneOf`** groups containing a Sparse member are evaluated per entity (bit tests) after the mask-level part;
- the change-detection "excluded" synthesis (1340-1352) already appends a per-chunk word set; `Changed<S>`/`Added<S>` for Sparse/Universal append theirs the same way from store ticks.

The gates `HasRequiredFilter`/`HasChunkFilter` (`View.hpp:47-70`) become true whenever a Sparse argument is present, which routes those views through `VisitChunkFiltered`. Pure-Table views keep their unfiltered path untouched (G4).

### 6.4 Fetching

Per argument, per chunk, a fetcher is resolved once: Table `base[i]` (today, `RequiredElement` `View.hpp:1030-1042`); Universal `page->data + (idx & 1023) * stride` with the page pointer cached across consecutive indices of one page; Sparse `dense[sparse[e.Index()]]`. Table and Sparse fetchers are selected at compile time; Universal is a runtime branch per chunk (hoisted out of the per-entity loop). `Mut<T>` stamps the store's ticks for Sparse/Universal like column ticks today (`StampChunkForYields` 804-820, `ResolveTrackedTicks` 1095-1105).

### 6.5 Every iteration surface

- Serial `ForEachImpl` (`View.hpp:859-895`), `ForEachWithOptional` (897-943), `InvokeEntityCallback` (1111-1138): via 6.3/6.4.
- Parallel `ParallelForEachSince` / `ParallelForEachWithContext` / `ParallelForEachChunkImpl` (204-414, 945-974): archetype-driven work items stay `(Archetype*, chunkIndex)`; sparse-driven views partition the dense range into fixed-size work items.
- `Size()`/`SizeFiltered` (1392-1426), `Get(Entity)`/`Single()`/`Contains` (`VisibleRecord` 672-699, 623-666): presence via store lookup.
- **Range-for** (`ViewIterator`, `Registry/ViewIterator.hpp:105-207`): today it is `static_assert`-refused when a required filter exists (`View.hpp:491-494, 518-521`). A required Sparse argument must NOT trigger that refusal (D2: a Table component never does). `ViewIterator` therefore learns to step through matching runs (it already caches per-chunk arrays, 157-177). The existing refusal for enableable filters is left exactly as it is (it predates this work).
- `Archetype::ForEach*` (`Archetype.hpp:838-852, 1953-2057`) is internal to table iteration and only ever sees Table arguments.
- Relations filters (`Registry/Relations.hpp:400-529`) call `HasComponent`/`GetComponent`, which are storage-aware (s3.3), so they need no change beyond tests.

## 7. Structural changes, CommandBuffer, signals

### 7.1 Creation

- `Registry::CreateEntity<Cs...>` / `CreateEntityWith` / `CreateEntities` / `CreateEntitiesWith` (`Registry.hpp:94-228`) split the pack by kind: Table members choose the archetype (`GetOrCreateArchetype`, `ArchetypeManager.hpp:1116-1160`, gets the Table subset only); Sparse members are inserted into their sets after placement (bits set at the entity's slot); then every Universal column constructs the entity's slot (s4.2 order).
- **One creation funnel.** `CommandBuffer::ExecuteCreateEntity` / `ExecuteCreateEntities` (`Commands/CommandBuffer.hpp:1441-1496`) call `ArchetypeManager::AddEntity` directly (1452, 1489) and emit `EntityCreated` themselves, bypassing `Registry`. They are rerouted through a new private `Registry::CreateEntityInternal(Entity preallocated)` so universal construction and init cannot be skipped.
- **No silent drops.** `ArchetypeChunk::ConstructComponentAt` returns silently when a component has no column (`ArchetypeChunkPool.hpp:140-143`), and `Archetype::AddEntitiesWith` placement-news through hoisted typed bases (`Archetype.hpp:554-579`). After this work no Sparse/Universal value can reach those paths (they are diverted first); the silent return becomes `ASTRA_ASSERT(false && "component has no column")` for non-empty types.

### 7.2 Add / remove / enable

- Typed and by-id paths (`ArchetypeManager.hpp:297-536`; `Registry.hpp:337-527, 673-723, 736-845`) branch on the component's effective kind before any archetype edge resolution. Sparse add = insert + bit set (+ `ComponentAdded`); Sparse remove = `ComponentRemoved` (before removal, as today) + erase + bit clear; Universal add/remove per s3.3.
- Batch forms (`AddComponents`, `RemoveComponents`, `EmplaceComponents`) apply the same split; a batch mixing Table and Sparse components performs ONE archetype move for the Table part and set operations for the rest.
- No archetype changes for Sparse/Universal means view archetype caches (`m_structuralChangeCounter`, `m_generation`, `ArchetypeManager.hpp:1713-1715`) need no invalidation for them. A separate `m_sparseStructuralCounter` feeds the iteration-safety guard (s4.3).
- `SetEnabledByID` (`Registry.hpp:673-705`) today refuses when `idToColumn < 0` (683-688); it now routes Sparse/Universal ids to the store, updates the chunk mirror, and fires `ComponentEnabled`/`ComponentDisabled` on change only.
- `CommandBuffer` record and execute paths need no change beyond 7.1: add/remove/enable executors already funnel through the by-id `Registry` entry points (`CommandBuffer.hpp:1521-1588, 1541-1548`).

### 7.3 Destroy, Clear, Defragment

- `DestroyEntity` / `DestroyEntities` (`Registry.hpp:230-309`): after `EntityDestroyed` and before the archetype swap-remove, walk the entity's chunk `m_sparseBlocks`; for each set `present` bit erase the entity from that slot's sparse set; destruct its slot in every universal column. Per-component `ComponentRemoved` is still NOT emitted on destroy (today's behaviour).
- `Clear()` (`Registry.hpp:1281-1286`): `ComponentStorage::Clear()` destroys every element (sets keep their slot assignments; pages are released), alongside `ArchetypeManager::Clear`, `RelationshipGraph::Clear` and the recycling `EntityManager::Clear` (T1-C2 semantics: ids are reused at their next version, so sparse pages may be re-touched by the same indices).
- `Defragment` / `CompactChunks`: carry per s5.3. Sparse sets themselves never need compaction (swap-and-pop keeps them dense).

### 7.4 Signals

Unchanged event set and timing (`Core/Signal.hpp:21-145`). For a Sparse empty type the typed `ComponentAdded` payload pointer is the per-type static used for tags today; by-id paths keep `EmptyComponentSentinel()` (`Component.hpp:249-253`).

## 8. Serialization

### 8.1 Format v6

`BINARY_FORMAT_VERSION` 5 → **6** (`Serialization/BinaryArchive.hpp:80-97`). Top-level order (`Registry::Save` 1725-1791): header, `EntityManager`, `ArchetypeManager`, **`ComponentStorage` (new)**, `RelationshipGraph`, `ResourceStorage`, checksum. Archetype records are unchanged: their descriptor lists already contain only mask (Table) components.

### 8.2 The ComponentStorage block

`uint32 storedCount`, then per stored component (sorted by hash for deterministic output): `{ uint64 hash; uint32 version; uint8 kind (Sparse|Universal); uint8 flags (hasDisabled = 0x01, hasValues = 0x02); uint32 elementCount; }` (18 bytes), then `elementCount` entities in ascending entity order, then the values iff `hasValues` (per-element `serializeVersioned`, the same element I/O as columns, `Archetype.hpp:1539-1576`), then a disabled bitmap if `hasDisabled`. Details fixed while planning: `version` is `uint32`, matching `ComponentDescriptor::version` and the archetype record (was `uint16`); `hasValues` is an explicit flag, so a tag block is presence-only; stores with zero elements are not written; "ascending entity order" is by `Entity::GetID()` (a raw `Entity` comparison would order by version first). With LZ4 enabled each component's payload is its own compressed block (the per-column precedent, 967-985). Change ticks are not persisted (today's rule). Chunk presence blocks are NOT serialized: they are derived on load from the sets and the entity records.

### 8.3 Storage-agnostic load (G5)

Every serialized component is resolved by hash to the LOADING registry's descriptor (as columns are today, `Archetype.hpp:1085-1098`) and placed in whatever storage that registry assigns, whatever the archive used:
- archived as a Table column, now Sparse/Universal: `Archetype::Deserialize` diverts that column's values into `ComponentStorage` keyed by the chunk's entities and excludes it from the rebuilt archetype mask (which relaxes the `mask.Count() == descriptors.size()` check at 1122-1125: the ON-DISK mask's popcount must equal the number of ARCHIVED descriptors -- the literal "mask count == number of Table descriptors" would refuse every migrating archive, because the disk mask still counts the diverted components; archetypes whose masks collapse after diversion are merged by one batched move);
- archived Sparse/Universal, now Table: the values are staged and applied after archetype load by one batched archetype move per destination archetype (`ApplyMigratedTableComponents`);
- declared Universal now but absent for some entities in the archive (an older save, or it was Table/Sparse and missing): those entities get default construction + the registry's `init` (Arcane: a fresh GUID for an entity that never had one).
v5 and older archives load through the same path (all components arrive as Table columns).

### 8.4 Cross-compiler type hashes (flagged, partly out of scope)

Descriptors resolve by type-name hash. The 2026-10-01 Linux build inventory found `TypeNameInternal` wrong on GCC (`Core/TypeID.hpp:171-188`, finding I1) and that MSVC spells template names differently from GCC/Clang. I1 (GCC/Clang agreement) and the missing `<limits>` in `Container/SmallVector.hpp` (I2) are fixed in this branch as prerequisites. Making MSVC and GCC/Clang hashes agree for templated components is NOT in scope; it needs its own decision (s12).

## 9. Testing, benchmarks, tooling

### 9.1 Storage-parity suite (the proof of D2)

Typed GoogleTest suites instantiate the SAME behavioural bodies over three variants of each test component that differ only in storage: `PosT` (Table), `PosS` (`AstraStorage = Sparse`) and `PosT` in a registry declaring it Universal (bodies that remove the component are skipped for the Universal variant, which instead asserts the refusal). Bodies are lifted from the existing suites: Registry add/remove/get/has (`tests/Registry/RegistryTest.cpp`), View/Query modifiers and enableable filtering (`tests/Registry/ViewTest.cpp:491-587`, `ViewIteratorTest.cpp`, `ViewEnrichmentTest.cpp`), parallel iteration (`ParallelIterationTest.cpp`), iteration safety (`IterationSafetyTest.cpp`), change detection (`ChangeDetection*Test.cpp`), CommandBuffer (`tests/Commands/CommandBufferTest.cpp:735-761` and the add/remove cases), serialization round trips (`tests/Serialization/BinarySerializationTests.cpp:862-1017`, `RegistrySerializationTest.cpp:601`), Relations filters. New test components respect the TypeID budget noted in `tests/TestComponents.hpp`.

### 9.2 Invariant fuzzer

A seeded random-operation fuzzer (create/destroy/batch create/batch destroy/add/remove/emplace/enable/disable/defragment/clear/save→load/CommandBuffer playback) over a mix of Table, Sparse, Sparse-enableable, Sparse-tag and Universal components runs against a reference model (`std::unordered_map<Entity, ...>` per component) and calls `ValidateStorageInvariants()` (P1-P3) plus a full model comparison after every operation. Seeds are printed; a fixed-seed short run is part of the normal suite; a long run is a separate filter.

### 9.3 Benchmarks

In `benchmark/Benchmark.cpp` (Google Benchmark) and `bench-compare/` (hand-rolled vs EnTT/Flecs, `RESULTS.md`):
- structural churn with a 48-byte non-trivially-copyable component present as Table vs Universal (the Arcane `Identity` shape);
- add/remove churn of a 16-byte component, Table vs Sparse, at 1K/10K/100K entities;
- filtered iteration `View<Pos, S>` with S at 1% / 50% / 99% density, Sparse vs Table, plus a sparse-driven `View<S>`;
- random `GetComponent` for each kind;
- every existing benchmark, Table-only, as the G4 regression gate (S4: within 2%, 3 runs).
Results are recorded in `bench-compare/RESULTS.md` with the machine and commit.

### 9.4 Inspector and Studio

`Debug::Inspector` (`Debug/Inspector.hpp:82-267`) reports each component's storage kind, sparse set sizes and page counts, universal page counts, and per-chunk presence blocks (Studio reads internals only through the Inspector). It also reports an **advisory**: per-component add/remove counts against population and the number of archetypes a Table component multiplies, flagging components that "look sparse-shaped". Advisory only; nothing is promoted automatically.

## 10. Arcane adoption (sketch; out of scope, opens the entity-record arc)

- Re-vendor Astra after this branch merges to `dev` (Arcane's normal Astra-first flow, `scripts/sync-astra.ps1`); ABI bump (Registry layout grows).
- Scene registries declare `config.AddUniversal<Arcane::Identity>(MintGuidInit)`; every Arcane creator stops adding `Identity` by hand; copy paths keep minting fresh GUIDs.
- Play snapshots, prefab and test registries choose per registry (they may keep `Identity` as Table).
- Candidate Sparse types are picked from the Inspector advisory and benchmarks, not up front.
- The registry-maintained GUID → entity index needed by GUID-keyed undo records is built on top of the universal column in the entity-record arc.

## 11. Risks

- **R1 Hidden column-ordinal assumptions.** Many paths index `idToColumn[...]` (the map lists `ResolveOneRequired`, `EnabledVisibleImpl`, `BindOptional`, `FilteredOptionalArg`, `CheckOptionalAllEnabled`, `GetComponentMut`, `MarkWritten`, `IsChanged`, `ChangeChunkPasses`, `StampChunkForYields`). Mitigation: every one is enumerated in the plan with its new kind-dispatch; the parity suite exercises each through the public API.
- **R2 Table-path regression.** Kind dispatch adds branches. Mitigation: Table/Sparse dispatch is compile-time; the universal check is resolved per view build and per chunk, never per entity; benchmark gate S4.
- **R3 Mask-invisibility.** Sparse/Universal members are absent from the `ComponentMask`, which is archetype identity, `Has`, matching and reflection. Every mask consumer in the map's checklist is visited; reflection APIs merge store membership explicitly.
- **R4 Module unload** (s4.4): stores must be torn down before descriptors are released, or destructor pointers dangle (Arcane has hit this class before).
- **R5 Caps.** 128 component ids shared with resources (`Component.hpp:17-21`); 32 sparse slots per registry by default.
- **R6 Load migration complexity** (s8.3): mitigated by the migration tests in 9.1 and the fuzzer's save→load step.

## 12. Decisions taken while writing (NEED USER REVIEW)

These were not discussed explicitly; each is the writer's pick with its reason.

1. **`ASTRA_MAX_SPARSE_COMPONENTS` = 32** per registry (configurable macro). Reason: bounds the per-chunk block search and the slot table; a typical game has a handful of sparse types.
2. **Adding an already-present Universal component is a no-op returning `true`**, matching today's Table rule (`ArchetypeManager.hpp:314-315`). The alternative (assign the new value) would be a behaviour difference from Table, violating D2. Refined while planning: that is the TYPED answer; the by-id `AddComponentByID` returns `false`, which is today's by-id Table answer for an already-present id (see s3.3).
3. **Removing a Universal component: Debug assert, Release `false`, no signal.**
4. **Range-for works for views with Sparse arguments** (s6.5), at the cost of teaching `ViewIterator` run-stepping. The existing refusal for *enableable* filters is untouched.
5. **Every tag may be enableable** (s3.2): Sparse tags through the store, Table tags through a bit-only column (user-requested parity, 2026-10-01).
6. **Storage-agnostic load with migration in both directions** (s8.3), rather than refusing an archive whose storage classes differ from the build's.
7. **Universal `init` runs only when no explicit value was supplied** (s4.2), including for entities restored by load that lack the component.
8. **Caller-less relocation functions are deleted** (s5.3) instead of maintained.
9. **The Inspector "sparse-shaped" advisory is in scope** (s9.4); auto-promotion stays rejected.
10. **Astra I1 (GCC type names) and I2 (`<limits>`) from the Linux inventory are fixed on this branch first.**
11. **Open, NOT decided:** MSVC vs GCC/Clang template type-name hashes differ (s8.4). If Astra archives or Arcane content must load across platforms, the hash needs a canonical spelling. That is its own design decision, probably before Arcane's Linux phase.

## 13. Plan shape (for writing-plans)

One plan on `feat/storage-policies`, TDD throughout, every task ending with the full `AstraTest` run (shuffled, seed recorded):
1. **Prerequisites:** I1 type-name fix, I2 `<limits>`; delete the three caller-less relocation functions.
2. **Declaration:** `Storage` trait, `ComponentDescriptor::storage`, `Config::AddUniversal`, sparse-slot table; enableable Table tags via bit-only columns (the tag/enableable `static_assert` deleted, parity-tested against an enableable data component).
3. **`ComponentStorage` + `UniversalColumn`:** create/destroy/Clear/Get/Has/enable/ticks; the creation funnel (7.1), including the CommandBuffer reroute; Remove refusal.
4. **`SparseSet`:** insert/erase/Get/Has/enable/ticks; typed and by-id add/remove routing; batches.
5. **Chunk presence blocks:** `SparseBlock`, the word freelist, P1-P3, `ValidateStorageInvariants`, carry at every s5.3 site, destroy walk.
6. **Queries:** classification + plan, `ForEachMatchingRun`, `VisitChunkFiltered` integration, fetchers, sparse-driven driver, parallel, Size/Get/Single, range-for, reflection APIs.
7. **Serialization:** format v6 block, storage-agnostic load and migration both ways, v5 compatibility.
8. **Module unload** teardown (s4.4).
9. **Parity suite + fuzzer** (9.1, 9.2).
10. **Benchmarks + RESULTS.md** (9.3), the S4 gate; **Inspector** reporting and advisory (9.4).
