# Astra Storage Policies Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Astra three component storage layouts behind one unchanged public API, plus enableable Table tags:
- **Table:** today's archetype columns;
- **Sparse:** out-of-table, non-fragmenting, with per-chunk presence bits;
- **Universal:** always present, slot-indexed.

**Architecture:**
- **Declaration.** A component type declares Table or Sparse through a trait, which is read into its `ComponentDescriptor`. A `Registry::Config` declares which components are universal.
- **Storage.** A new `ComponentStorage`, sitting beside `ArchetypeManager`, owns one `SparseSet` per sparse component and one `UniversalColumn` per universal component.
- **Presence bits.** Sparse presence is mirrored into per-chunk `SparseBlock` bitsets, allocated lazily. A generalised run extractor (`ForEachMatchingRun`) consumes them in `View`'s filtered path, so queries over sparse components filter a word at a time.
- **Dispatch.** Every structural, iteration, serialization and module-unload path dispatches on the component's effective `StorageKind`.

**Tech Stack:**
- C++23, header-only Astra (`include/Astra/`).
- MSVC v143 (VS 18 msbuild).
- premake5 (`D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`).
- GoogleTest (`AstraTest`) and Google Benchmark (`AstraBenchmark`).
- The hand-rolled `bench-compare/` harness.

**Spec:** `docs/superpowers/specs/2026-10-01-astra-storage-policies-design.md` (this branch, `2611c37`).
- Read s1-s3 before any task, then the sections your task cites.
- All `file:line` anchors are at base `975cdb7`. If an earlier task has edited the file and an anchor has drifted, locate the site by its quoted content.

## Global Constraints

**Repo, branch and files**
- Work in `D:\dev\starworks\Astra-storage` (a git worktree), branch `feat/storage-policies`, base `975cdb7`.
- Never touch `D:\dev\starworks\Astra`. Its `dev` branch is pinned at `975cdb7` for Arcane's T1-C7 re-vendor.
- Never push. Stage files BY NAME; never `git add -A`.
- The working copy is CRLF. Keep each file's existing line endings: no MSYS `sed -i` on tracked files, because it rewrites them to LF.

**Build**
- Use PowerShell, because Git Bash mangles `/p:`:
  `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
- `ide/` is generated and untracked. After ADDING a new `tests/**.cpp` file, run `D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026` from the worktree root.

**Tests**
- While iterating: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite.*>`.
- Before every commit, the full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`.
  - Do not pass `--gtest_brief=1`: it hides the seed line.
  - Record `Randomizing tests' orders with a seed of N` in the task report.
- Run every build and test in the FOREGROUND.

**Commits**
- One or more per task, in conventional form: `feat(storage): ...`, `fix(...)`, `test(storage): ...`, `refactor(...)`, `docs(storage): ...`, `perf(storage): ...`.
- End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

**Behaviour and limits**
- **D2, binding:** storage is invisible at the public API. Every public call behaves identically for Table, Sparse and Universal components, with one exception. Removing a Universal component is refused: `ASTRA_ASSERT` in Debug, returns `false` in Release, no signal.
  - Parity is with each SURFACE's current answer: an already-present component's typed `AddComponent` returns `true` and its by-id `AddComponentByID` returns `false`, for every kind, Universal included (spec s3.3, refined).
- **Constants (spec s3.2, s4):**

  | Constant | Value |
  |---|---|
  | `ASTRA_MAX_SPARSE_COMPONENTS` (sparse slots per registry) | 32 |
  | `ASTRA_SPARSE_PAGE_SIZE` (entries per sparse page) | 4096 |
  | `ASTRA_UNIVERSAL_PAGE_SLOTS` (slots per universal page) | 1024 |
  | `BINARY_FORMAT_VERSION` | 5 -> 6 |

- **No silent drops:** no Sparse or Universal value may reach a chunk column path. `ConstructComponentAt`'s silent `col < 0` return becomes an assert for non-empty types (spec s7.1).
- **Benchmark gate (spec S4):** Table-only benchmarks stay within 2% of `975cdb7` over 3 runs.

**Plan code is unrun code.** If a code block in this plan does not compile against the real headers:
1. make the smallest correct fix;
2. keep the task's intent and its tests;
3. record the deviation in the report.

## Canonical interfaces (every task uses these exact names)

This block is the FINAL truth after the consistency pass: every name below is defined, with this signature, by the task named in its comment. The spec writes `entity.Index()`; the code's accessor is `Entity::GetID()` (stores index by `static_cast<std::size_t>(e.GetID())`).

```cpp
// ---- Component/Component.hpp (Task 3)
namespace Astra {
  enum class Storage : uint8_t { Table, Sparse };                       // type trait value
  template<typename T> struct StorageTraits;                            // T::AstraStorage if convertible, else Table
  template<typename T> inline constexpr Storage StorageOfV = StorageTraits<std::remove_const_t<T>>::value;
  enum class StorageKind : uint8_t { Table, Sparse, Universal };        // effective kind in one registry
  constexpr std::size_t MAX_SPARSE_COMPONENTS = ASTRA_MAX_SPARSE_COMPONENTS;   // constexpr mirrors of the macros
  constexpr std::size_t SPARSE_PAGE_SIZE      = ASTRA_SPARSE_PAGE_SIZE;
  constexpr std::size_t UNIVERSAL_PAGE_SLOTS  = ASTRA_UNIVERSAL_PAGE_SLOTS;
  // ComponentDescriptor gains:  Storage storage = Storage::Table;
}
// macros, beside ASTRA_MAX_COMPONENTS (Component.hpp:17-21)
#define ASTRA_MAX_SPARSE_COMPONENTS 32
#define ASTRA_SPARSE_PAGE_SIZE 4096
#define ASTRA_UNIVERSAL_PAGE_SLOTS 1024

// ---- Registry/ComponentStorage.hpp -- NAMESPACE scope (Task 4), aliased by Registry::Config (Task 5)
using UniversalInitFn = void (*)(Entity entity, void* component, void* user);
struct UniversalSpec {
  ComponentID id = INVALID_COMPONENT; UniversalInitFn init = nullptr; void* user = nullptr;
  void (*registerComponent)(ComponentRegistry& registry) = nullptr;     // filled by AddUniversal<T>; called by ComponentStorage's ctor
};

// ---- Registry/Registry.hpp
// Registry::Config gains (Task 5)
using UniversalInitFn = Astra::UniversalInitFn;  using UniversalSpec = Astra::UniversalSpec;
std::vector<UniversalSpec> universal;                                   // last declaration of an id wins
template<Component T> Config& AddUniversal(UniversalInitFn init = nullptr, void* user = nullptr);
// Registry gains:
ComponentStorage*       GetComponentStorage() noexcept;                 // Task 5 -- returns a POINTER; tests use ->
const ComponentStorage* GetComponentStorage() const noexcept;
std::shared_ptr<ComponentStorage> m_componentStorage;                   // Task 5 (private); rebuilt in LoadInternal BEFORE archetype Deserialize
std::string ValidateStorageInvariants() const;   // Task 16; every config; empty string == invariants P1-P3 hold (spec s5.2)
// private creation funnel used by every create path, incl. the CommandBuffer executors (friend class CommandBuffer):
Entity CreateEntityInternal(Entity preallocated);   // Task 9: root placement + universal construction/init + EntityCreated
// private routing (Tasks 9, 11, 14): KindOfT<T>(), RoutesToStore<T>(), StoreLookup(e, id), StoreGet<T>(e),
//   IsUniversalId(id) (Task 12), MarkWrittenRouted / MarkStoreWritten / StoreTickNewer (Task 14; Sparse case Task 20),
//   ResolveKind<T>(), SparseAttach, SparseDetach, SparseTypedPtr<T> (Task 16), SetEnabledInStore (Task 19),
//   SparseTicksOf / StampSparseWrite / SparseChangedSince (Task 20), StoredElementByID (Task 30),
//   FinishStorageLoad (Task 32), InstallStoreReleaseHook / OnStoreRelease / ReleaseStoredComponent (Task 36).

// ---- Registry/StorageSplit.hpp (Task 11): compile-time pack split by storage trait
//   StorageIndexSeq<S, Cs...>, TypesWithStorage<S, Cs...> (decayed std::tuple of the members whose trait is S)

// ---- Registry/SparseSet.hpp (Task 10)
class SparseSet {
public:
  static constexpr std::size_t PAGE_ENTRIES = ASTRA_SPARSE_PAGE_SIZE;  static constexpr std::uint32_t INVALID_INDEX = UINT32_MAX;
  explicit SparseSet(const ComponentDescriptor& desc);   // descriptor held BY VALUE
  bool          Contains(Entity e) const;
  void*         Get(Entity e);              const void* Get(Entity e) const;   // nullptr if absent or stale; a tag yields EmptyComponentSentinel()
  void*         Insert(Entity e);           // default-construct; precondition !Contains(e)
  void*         InsertMove(Entity e, void* src);
  bool          Erase(Entity e);            // swap-and-pop; false if absent or stale
  size_t        Size() const;
  const Entity* Entities() const;           void* DataAt(size_t dense);   EntityTicks* TicksAt(size_t dense);   // DataAt: nullptr for a tag; TicksAt: nullptr unless tracked
  bool          IsDisabled(Entity e) const; bool SetDisabled(Entity e, bool disabled);   // true on change
  void          Clear();
  const ComponentDescriptor& Descriptor() const;
  void   SetTickSource(const Tick* tick) noexcept;
  bool   MarkWritten(Entity e, Tick now) noexcept;   // false: absent, stale or tag; stamps only a TRACKED element
  Tick   AddedTick(Entity e) const noexcept;         // tracked: per element; untracked or absent: 0 (no store-level coarse tier)
  Tick   ChangedTick(Entity e) const noexcept;
  size_t PageCount() const noexcept;                 // live sparse pages
  size_t Capacity() const noexcept;                  // dense data capacity (elements)
};

// ---- Registry/UniversalColumn.hpp (Task 8)
class UniversalColumn {
public:
  static constexpr std::size_t PAGE_SLOTS = ASTRA_UNIVERSAL_PAGE_SLOTS;
  explicit UniversalColumn(const ComponentDescriptor& desc);
  void*        Construct(Entity e);         void* ConstructMove(Entity e, void* src);
  void         Destroy(Entity e);           // no-op for an absent or stale handle
  void*        Get(Entity e);               const void* Get(Entity e) const;   // nullptr unless live with matching version
  bool         IsLive(Entity e) const;
  bool         IsDisabled(Entity e) const;  bool SetDisabled(Entity e, bool disabled);
  EntityTicks* Ticks(Entity e);             // nullptr unless live and change-tracked
  size_t       LiveCount() const;
  template<class Fn> void ForEachLive(Fn&& fn) const;   // fn(Entity, const void*), ascending index
  void         Clear();
  const ComponentDescriptor& Descriptor() const;
  void   SetTickSource(const Tick* tick) noexcept;
  bool   MarkWritten(Entity e, Tick now) noexcept;   // false: absent, stale or tag
  Tick   AddedTick(Entity e) const noexcept;         // tracked: per slot; untracked: per-PAGE coarse version; 0 if not live
  Tick   ChangedTick(Entity e) const noexcept;
  size_t PageCount() const noexcept;
};

// ---- Registry/ComponentStorage.hpp
class ComponentStorage {
public:
  ComponentStorage(std::shared_ptr<ComponentRegistry> registry, const std::vector<UniversalSpec>& universal);  // Task 4
  StorageKind      KindOf(ComponentID id) const;                // Task 4; Universal (config) > trait > Table; unregistered -> Table
  int              SparseSlotOf(ComponentID id) const;          // Task 4; -1 if none
  int              AcquireSparseSlot(ComponentID id);           // Task 4; idempotent, lowest free slot, kind NOT checked; -1 at the 32 cap
  ComponentID      ComponentOfSlot(uint16_t slot) const;        // Task 4; INVALID_COMPONENT for a free slot
  size_t           UsedSparseSlotCount() const;                 // Task 4
  const UniversalSpec* FindUniversalSpec(ComponentID id) const; // Task 4
  const std::vector<UniversalSpec>& UniversalSpecs() const;     // Task 4
  void             SetTickSource(const Tick* tick);             // Task 9 (+ sparse sets, Task 11)
  bool             HasUniversals() const;  bool HasAnyStore() const;   // Task 9
  UniversalColumn* FindUniversal(ComponentID id) const;         // Task 9; never creates
  UniversalColumn* Universal(ComponentID id);                   // Task 9; created lazily for Universal kinds, else nullptr
  SparseSet*       FindSparse(ComponentID id) const;            // Task 11; never creates
  SparseSet*       Sparse(ComponentID id);                      // Task 11; created lazily for Sparse kinds (acquires the slot)
  void             ConstructUniversals(Entity e, const ComponentID* supplied, size_t suppliedCount);   // Task 9
  void             OnEntityDestroyed(Entity e, ArchetypeChunk& chunk, size_t index);   // Task 9; final body (presence walk) Task 21
  void             Clear();                                     // Task 4 no-op; Tasks 9, 11 fill it; slots + specs survive
  Tick             CoarseVersion(ComponentID id) const;  void StampCoarse(ComponentID id, Tick tick);   // Task 20: untracked SPARSE tier
  void             Serialize(BinaryWriter& w) const;            // Task 31 (declared in-class, defined in ComponentStorageSerialization.hpp)
  bool             Deserialize(BinaryReader& r, class Registry& registry);   // Task 31 declares; defined out of line after class Registry (Task 31 stub, Task 32 body)
  std::vector<StagedTableColumn>& PendingTableMigrations();     // Task 34
  void             ReleaseComponent(ComponentID id);            // Task 35; module unload (s4.4)
  // private: m_registry, m_universal, m_universalMask, m_slotOf, m_componentOfSlot, m_usedSlots (Task 4);
  //          m_universalColumns, m_tickSource, m_storeCount (Task 9); m_sparseSets, m_sparseIds (Task 11);
  //          m_coarseVersion (Task 20); m_pendingTable (Task 34)
};
// Change-detection tiers, one coarse scheme per kind (integration ruling):
//   tracked Sparse/Universal: exact per element/slot; untracked Universal: UniversalColumn per-page version;
//   untracked Sparse: ComponentStorage::CoarseVersion(id). SparseSet keeps NO coarse tick.

// ---- Archetype/ArchetypeChunkPool.hpp (Task 15) -- ArchetypeChunk gains (SparseBlock is NESTED)
struct ArchetypeChunk::SparseBlock { uint16_t slot; uint32_t presentCount; uint32_t disabledCount; uint64_t* present; uint64_t* disabled; };
SparseBlock*       FindSparseBlock(uint16_t slot) noexcept;    const SparseBlock* FindSparseBlock(uint16_t slot) const noexcept;
SparseBlock&       GetOrCreateSparseBlock(uint16_t slot, bool withDisabled);
bool               SetSparsePresent(uint16_t slot, size_t index, bool present);   // true on change
bool               IsSparsePresent(uint16_t slot, size_t index) const noexcept;
bool               SetSparseDisabled(uint16_t slot, size_t index, bool disabled);
bool               IsSparseDisabled(uint16_t slot, size_t index) const noexcept;
void               CarrySparseBits(const ArchetypeChunk& src, size_t srcIndex, size_t dstIndex);
void               SwapRemoveSparseBits(size_t index, size_t lastIndex) noexcept;
std::span<const SparseBlock> GetSparseBlocks() const noexcept;
bool               HasSparseBlocks() const noexcept;
void               ReleaseSparseBlock(uint16_t slot) noexcept;  // module unload (Task 36)
class SparseWordFreelist { uint64_t* Acquire(size_t words); void Release(uint64_t*, size_t) noexcept; void Trim() noexcept;
                           size_t LiveBlocks() const noexcept; size_t CachedBlocks() const noexcept; };   // non-copyable
struct ArchetypeChunkPool::SparseWordStats { size_t liveBlocks; size_t cachedBlocks; };
SparseWordStats    ArchetypeChunkPool::GetSparseWordStats() const noexcept;

// ---- Archetype/ArchetypeManager.hpp
const Tick*       TickSource() const noexcept;                          // Task 9 (&m_tick)
uint32_t          GetSparseStructuralCounter() const noexcept;          // Task 16; bumped by every sparse membership change
void              BumpSparseStructuralCounter() noexcept;               // Task 16 (member std::atomic<uint32_t> m_sparseStructuralCounter)
void              SetComponentStorage(ComponentStorage* storage) noexcept;   // Task 22 (non-owning; View/Relations reach the stores here)
ComponentStorage* GetComponentStorage() const noexcept;                 // Task 22
bool              Deserialize(BinaryReader& reader, const StorageLoadHooks* hooks = nullptr);   // Task 33 (Archetype::Deserialize gains the same trailing param)
bool              ApplyMigratedTableComponents(std::vector<StagedTableColumn>& staged);       // Task 34

// ---- Registry/EnabledRuns.hpp (Task 23), namespace Astra::Detail
template<class Fn> void ForEachMatchingRun(const uint64_t* const* exclude, size_t nExclude,
                                           const uint64_t* const* require, size_t nRequire, size_t count, Fn&& fn);
size_t CountMatching(const uint64_t* const* exclude, size_t nExclude, const uint64_t* const* require, size_t nRequire, size_t count);
void   WriteMatchWords(const uint64_t* const* exclude, size_t nExclude, const uint64_t* const* require, size_t nRequire,
                       size_t count, uint64_t* out);   // out: (count + 63) / 64 words

// ---- Queries (Chunk D)
static ComponentMask QueryBuilder<...>::GetTableRequiredMask();  static ComponentMask QueryBuilder<...>::GetTableExcludedMask();   // Task 22
static constexpr bool View<...>::HasSparseArg;                                                                                   // Task 22
// Registry/StorageAccess.hpp, namespace Astra::Detail:
EntityTicks* SparseTicksOf(SparseSet& set, Entity e) noexcept;                                        // Task 25
template<typename T> StorageKind EffectiveKindOf(const ComponentStorage* storage) noexcept;           // Task 30
template<typename T> bool HasInStorage(ArchetypeManager& am, Entity e, StorageKind kind);             // Task 30
template<typename T> std::remove_const_t<T>* GetInStorage(ArchetypeManager& am, Entity e, StorageKind kind);   // Task 30
// Registry/ViewIterator.hpp (Task 29):
struct ViewIterHooks { const void* view; bool (*chunkMask)(const void*, Archetype*, ArchetypeChunk*, std::vector<uint64_t>&);
                       bool (*resolve)(const void*, Archetype*, ArchetypeChunk*, size_t, void**); bool perEntity; };
ViewIterable<Cs...>::Iterator(Archetype* const*, size_t, Tick now, const ViewIterHooks* hooks = nullptr);
// Views never cache SparseSet*/UniversalColumn* across calls: store pointers are re-resolved at every entry point.

// ---- Serialization (Chunk E)
// Serialization/StorageMigration.hpp: namespace Astra::StorageFormat { KIND_SPARSE = 1, KIND_UNIVERSAL = 2,
//   FLAG_HAS_DISABLED = 0x01, FLAG_HAS_VALUES = 0x02, FLAG_MASK, BLOCK_HEADER_BYTES = 18, DisabledWordCount,
//   WriteDisabledBitmap, ReadDisabledBitmap }   (Task 31; BINARY_FORMAT_VERSION 5 -> 6)
struct StorageLoadHooks { void* ctx; bool (*isDiverted)(void*, ComponentID); void* (*acquire)(void*, const ComponentDescriptor&, Entity);
                          void (*setDisabled)(void*, ComponentID, Entity); bool IsDiverted(ComponentID) const; };   // Task 33
class StagedTableColumn;   // Task 34: move-only staged Table-now values (MoveOut into a column slot)

// ---- Component/ComponentRegistry.hpp (Task 35): store-release hooks for module unload
using StoreReleaseFn = void (*)(void* ctx, ComponentID id, const ComponentDescriptor& departing);
uint64_t AddStoreReleaseHook(void* ctx, StoreReleaseFn fn);  void RemoveStoreReleaseHook(uint64_t handle);  size_t StoreReleaseHookCount() const;
```

## Review Focus

Each item is the behaviour a user should get, plus the task whose tests pin it.

1. **A stale handle after `Clear()` or destroy-and-recycle** (since T1-C2, ids are reused at version+1). Passed to `Get`, `Has`, `Add`, `Remove` or `SetEnabled` on a Sparse or Universal component, it must miss or be refused. It must never read or write the new occupant's element. Pinned in Task 9 (Universal Get/Has), Task 12 (Universal Add/Remove), Task 13 (Universal SetEnabled), Task 10 (Sparse, store level), Task 11 (Sparse Get/Has) and Task 19 (Sparse Add/Remove/SetEnabled); Task 37's parity suite re-pins it for every kind.
2. **A sparse add or remove during iteration of a view that requires that sparse component.** It must be refused exactly like a Table structural change (the iteration-safety guard), and must never corrupt the dense arrays mid-walk. Pinned in Task 25 (`IterationSafety.SparseRemoveDuringRequiringViewTripsTheGuardAndStaysSafe`, `SparseAddDuringRequiringViewTripsTheGuard`, `SparseRemoveDuringSparseOnlyViewTripsTheGuardAndStaysSafe`, `SparseValueEditsAndDeferredRemovesDoNotTripTheGuard`); parity re-pinned in Task 38 (`StructuralChangeDuringForEachTripsTheGuard`).
   - "Refused" means what it means for Table today: the Debug structural-mutation guard REPORTS it (`ASTRA_ASSERT` after the walk, now also comparing `ArchetypeManager::GetSparseStructuralCounter()`). The add/remove itself is not rejected at the call site, in any build.
   - The walk is memory-safe in every build: word pointers are copied per chunk, Sparse elements are re-fetched by entity, the sparse-driven walk is index-based, and a required element that vanished mid-walk is skipped.
   - Parity ruling (Task 38): the guard trips for exactly what trips it for Table today (archetype-set changes) plus sparse membership changes. Destroying a bystander and a Table move into an already-existing archetype do not trip it today, so Task 38 asserts neither. (Destroying an entity that HOLDS a Sparse component changes sparse membership, so it bumps the sparse counter, Task 21.)
3. **A swap-remove where the removed slot and the tail slot carry different sparse memberships,** including the case where the removed slot IS the tail. P1 must hold for the moved entity afterwards. Pinned in Task 17 (`SparseCarry.SwapRemoveCarriesTheTailsMembershipIntoTheVacatedSlot`, `SwapRemoveOfTheTailDropsOnlyItsOwnBits`); hammered by Task 39's fuzzer.
4. **Loading an LZ4-compressed archive in which a component was Table and is now Sparse.** The column must be diverted out of its compressed block into the sparse set. Pinned in Task 33 (`StorageMigration.Lz4TableColumnIsDivertedIntoTheSparseSet`).
5. **Unloading a plugin module whose Sparse component is still held by live entities, then destroying those entities.** No dangling destructor may be called, and no presence block may be left for the released slot. Pinned in Task 36 (`StorageModuleUnload.UnloadingAPluginSparseComponentHeldByLiveEntitiesLeavesNoDanglingDestructorOrBlock`, `RangePurgeOfAPluginSparseComponentLeavesNoDanglingDestructorOrBlock`).

## Task index

| Tasks | Chunk | Covers (spec) |
|---|---|---|
| 1-7 | A: prerequisites + declaration | I1/I2 (s8.4), dead relocation code (s5.3), `Storage` trait + descriptor (s3.2), `Config::AddUniversal` (s3.2), `ComponentStorage` skeleton + `KindOf` + sparse slots (s3.2, s4), enableable Table tags via bit-only columns (s3.2) |
| 8-14 | B: stores + creation | `UniversalColumn` (s4.2), `SparseSet` (s4.1), creation funnel incl. the CommandBuffer reroute (s7.1), Get/Has routing (s3.3), Universal Add no-op + Remove refusal (s3.3), Universal enable + ticks (s3.3) |
| 15-21 | C: sparse structure + presence | Sparse add/remove typed/by-id/batch (s7.2), Sparse enable + ticks, `SparseBlock` + word freelist (s5.1), `ValidateStorageInvariants` P1-P3 (s5.2, introduced in Task 16), carry at every relocation site (s5.3), destroy walk + Clear + Defragment (s7.3) |
| 22-30 | D: queries | classification + plan (s6.1), `ForEachMatchingRun` (s6.3), `VisitChunkFiltered` integration + fetchers (s6.3, s6.4), Optional/Not/Any/OneOf, change detection, sparse-driven driver (s6.2), parallel, Size/Get/Single/Contains, range-for `ViewIterator`, reflection APIs, Relations (s6.5) |
| 31-36 | E: serialization + unload | format v6 block (s8.1, s8.2), storage-agnostic load in both directions + LZ4 + universal init on load + v5 compatibility (s8.3), module-unload teardown (s4.4) |
| 37-42 | F: proof + perf + tooling | storage-parity typed suites (s9.1), invariant fuzzer (s9.2), benchmarks + RESULTS.md + the S4 gate (s9.3), Inspector reporting + advisory (s9.4), spec status + README |

---
## Chunk A: prerequisites + declaration (Tasks 1-7)

Spec sections: s8.4 (I1/I2), s5.3 (dead relocation code), s3.2 (declaration, enableable Table tags), s4 (the `ComponentStorage` owner).

**What this chunk leaves behind for Chunks B-F.**
- `Astra::Storage`, `StorageTraits<T>`, `StorageOfV<T>`, `StorageKind`, `ComponentDescriptor::storage`, and the three macros. The macros also have `constexpr` mirrors: `MAX_SPARSE_COMPONENTS`, `SPARSE_PAGE_SIZE`, `UNIVERSAL_PAGE_SLOTS`.
- `Registry/ComponentStorage.hpp`:
  - `Astra::UniversalInitFn` and `Astra::UniversalSpec`, both at namespace scope. `Registry::Config` aliases them.
  - `ComponentStorage` with `KindOf`, the sparse slot table, `FindUniversalSpec`, `UniversalSpecs` and `Clear`.
- `Registry` owns `std::shared_ptr<ComponentStorage> m_componentStorage`. It is built in every constructor, cleared in place by `Clear()`, and rebuilt in `LoadInternal`. It is exposed through `GetComponentStorage()`.
- Enableable Table tags get a bit-only column: `stride == 0`, with a disabled-word region.
  - Its `base` points into the chunk arena at the running carve offset. It is non-null and never dereferenced.
  - **Later chunks must not read `GetComponentArrayByID(id) != nullptr` as "has data".** Test `desc.size` or the column stride.
- New test components in `tests/TestComponents.hpp`, two new TypeIDs: `Astra::Test::PosS` (Sparse twin of `Position`) and `Astra::Test::EnTag` (enableable Table tag).
- New test files:
  - `tests/Container/SmallVectorHeaderTest.cpp`;
  - `tests/Registry/StorageDeclarationTest.cpp`;
  - `tests/Registry/EnableableTagTest.cpp`.

All commands run from `D:\dev\starworks\Astra-storage` in **PowerShell**:
- build: `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
- full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Record the `Randomizing tests' orders with a seed of N` line in the task report.

---

### Task 1: Linux prerequisites I1 + I2 (GCC type names, `<limits>`)

Spec s8.4, s12 item 10. Linux inventory `D:\dev\starworks\Arcane\docs\research\2026-10-01-linux-gcc-clang-build-inventory.md`, findings I1 and I2. The fix matches the inventory's proof diff.

**Files:**
- Modify: `include/Astra/Core/TypeID.hpp:157-205` (`Detail::TypeNameInternal`: the GCC comment at 171, and a new cut after the `end` check at 186-188)
- Modify: `include/Astra/Container/SmallVector.hpp:3-10` (include list)
- Test: `tests/Core/TypeIDTests.cpp` (append at end of file)
- Create: `tests/Container/SmallVectorHeaderTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - On GCC, `Astra::Detail::TypeNameInternal<T>()` is the bare qualified name: no `; std::string_view = ...` tail. So `TypeID<T>::Name()` and `TypeID<T>::Hash()` agree between GCC and Clang.
  - `Astra/Container/SmallVector.hpp` compiles as the first include of a TU.

- [ ] **Step 1: Write the failing tests**

Append to `tests/Core/TypeIDTests.cpp`, after the last test:

```cpp
// ---- Linux inventory 2026-10-01, finding I1 ----------------------------------
// GCC's __PRETTY_FUNCTION__ lists the function's OTHER bindings after T:
//   "... [with T = X; std::string_view = std::basic_string_view<char>]".
// The type name, and therefore every TypeHash, must stop at T. A named, non-template
// namespace type keeps the expected spelling identical on MSVC, Clang and GCC.
// Name()/Hash()/TypeNameInternal mint NO ComponentID (zero TypeID budget).
namespace AstraTypeNameProbe
{
    struct Probe { int v; };
    template<typename T> struct Box { T v; };
}

// Compile-time pin: this is what fails the GCC build before the fix.
static_assert(Astra::Detail::TypeNameInternal<AstraTypeNameProbe::Probe>().find(';') == std::string_view::npos,
              "I1: a type name must not carry the compiler's trailing binding list");

TEST(TypeNameSpelling, NamespacedTypeIsTheBareQualifiedName)
{
    constexpr std::string_view name = Astra::TypeID<AstraTypeNameProbe::Probe>::Name();
    EXPECT_EQ(name, std::string_view("AstraTypeNameProbe::Probe"));
    EXPECT_EQ(Astra::TypeID<AstraTypeNameProbe::Probe>::Hash(),
              Astra::Detail::XXHash::XXHash64(std::string_view("AstraTypeNameProbe::Probe")));
}

TEST(TypeNameSpelling, TemplateNameCarriesNoBindingList)
{
    // MSVC spells template arguments differently from GCC/Clang. That is spec s8.4 and out
    // of scope, so this test pins only the GCC binding-list tail.
    constexpr std::string_view name = Astra::TypeID<AstraTypeNameProbe::Box<int>>::Name();
    EXPECT_EQ(name.find(';'), std::string_view::npos) << name;
    EXPECT_NE(name.find("AstraTypeNameProbe::Box<"), std::string_view::npos) << name;
}
```

Create `tests/Container/SmallVectorHeaderTest.cpp`:

```cpp
// Linux inventory 2026-10-01, finding I2: SmallVector::max_size() uses
// std::numeric_limits, and the header did not include <limits>. libstdc++ does not pull
// <limits> in transitively, so GCC fails with "incomplete type
// 'std::numeric_limits<long unsigned int>'". SmallVector.hpp MUST stay the FIRST include
// of this TU: that ordering is the self-containment proof.
#include "Astra/Container/SmallVector.hpp"

#include <cstddef>
#include <limits>
#include <gtest/gtest.h>

TEST(SmallVectorHeader, MaxSizeInstantiatesFromASelfContainedInclude)
{
    Astra::SmallVector<int, 4> v;
    EXPECT_EQ(v.max_size(), std::numeric_limits<std::size_t>::max());
}
```

- [ ] **Step 2: Run the tests, then reproduce the GCC failure**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=TypeNameSpelling.*:SmallVectorHeader.*
```

Expected on MSVC: PASS. MSVC's `<memory>` already pulls in `<limits>`, and MSVC/Clang already spell the name bare. These tests pin MSVC and Clang. The GCC failure is reproduced on this desk's WSL2 Ubuntu 24.04:

```powershell
New-Item -ItemType Directory -Force bin-int\probe | Out-Null
@'
#include <Astra/Container/SmallVector.hpp>
#include <Astra/Core/TypeID.hpp>
namespace AstraTypeNameProbe { struct Probe { int v; }; }
static_assert(Astra::Detail::TypeNameInternal<AstraTypeNameProbe::Probe>() == "AstraTypeNameProbe::Probe",
              "I1: GCC type name carries a trailing binding list");
int main() { Astra::SmallVector<int, 4> v; return v.max_size() == 0 ? 1 : 0; }
'@ | Set-Content -Encoding ascii bin-int\probe\i1i2.cpp
wsl -e g++ -std=c++23 -fsyntax-only -I/mnt/d/dev/starworks/Astra-storage/include -I/mnt/d/dev/starworks/Astra-storage/vendor/Mosaic/include /mnt/d/dev/starworks/Astra-storage/bin-int/probe/i1i2.cpp
```

Expected: FAIL with `static assertion failed: I1: GCC type name carries a trailing binding list`.
- If `g++-14` is installed, run it instead of `g++`. GCC 14 also reports I2 (`incomplete type 'std::numeric_limits<long unsigned int>' used in nested name specifier`).
- GCC 13 may already reach `<limits>` transitively. Record what you see.
- If WSL or g++ is unavailable, or the probe fails for an unrelated reason, record it in the task report and continue. `bin-int/` is git-ignored, so the probe is never staged.

- [ ] **Step 3: Implement**

In `include/Astra/Container/SmallVector.hpp`, add `<limits>` to the include list (alphabetical, after `<iterator>`):

```cpp
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
```

In `include/Astra/Core/TypeID.hpp`, in `TypeNameInternal`, replace the GCC branch comment:

```cpp
            #elif defined(ASTRA_COMPILER_GCC)
                // GCC: __PRETTY_FUNCTION__ gives
                //   "constexpr std::string_view TypeName() [with T = MyClass; std::string_view = ...]"
                // -- the tail after ';' is cut below.
                constexpr std::string_view funcName = __PRETTY_FUNCTION__;
```

Then insert this block between the `end` check (`if (end == std::string_view::npos || end <= start) return "Unknown";`) and `// Extract the type name`:

```cpp
            #if defined(ASTRA_COMPILER_GCC)
                // GCC lists the function's OTHER bindings after T, ';'-separated:
                //   "[with T = Ns::X; std::string_view = std::basic_string_view<char>]".
                // Without this cut every GCC type name, and so every TypeHash, carries that
                // tail, and name-keyed lookups ("Ns::X") never match Clang's
                // (Linux inventory 2026-10-01, finding I1). A type name itself never
                // contains ';'.
                if (const size_t semi = funcName.find(';', start); semi != std::string_view::npos && semi < end)
                    end = semi;
            #endif
```

- [ ] **Step 4: Run the tests**

```powershell
wsl -e g++ -std=c++23 -fsyntax-only -I/mnt/d/dev/starworks/Astra-storage/include -I/mnt/d/dev/starworks/Astra-storage/vendor/Mosaic/include /mnt/d/dev/starworks/Astra-storage/bin-int/probe/i1i2.cpp
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=TypeNameSpelling.*:SmallVectorHeader.*:TypeIDTests.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- the WSL probe compiles with no output (or the unrelated-failure note from Step 2 is recorded);
- the filtered run passes;
- the full run is green. Record its seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Core/TypeID.hpp include/Astra/Container/SmallVector.hpp tests/Core/TypeIDTests.cpp tests/Container/SmallVectorHeaderTest.cpp
git commit -m @'
fix(core): GCC type names stop at T (cut the __PRETTY_FUNCTION__ binding list) and SmallVector.hpp includes <limits> -- Linux inventory I1/I2, storage-policies prerequisites

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

### Task 2: Delete the three caller-less relocation functions

Spec s5.3, last paragraph, and s12 item 8. These three functions relocate chunk slots without the sparse-bit carry. Revived, they would silently break P1, so they are deleted rather than taught the carry. All three are `private` and have no caller in `include/`, `tests/`, `benchmark/`, `bench-compare/` or `studio/`.

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp:2124-2194`. Delete `MoveEntitiesBetweenChunks`.
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:1361-1411`. Delete `MoveEntitiesWithComponent`.
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:1653-1671`. Delete `BatchMoveEntitiesWithComponentByID` and its doc comment.
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp:406` and `:603`. Fix two comments that name the deleted function.

**Interfaces:**
- Consumes: nothing.
- Produces: nothing new. After this task, the only same-archetype relocation is `Archetype::CompactChunks`. Chunk C's Task 17 carries sparse bits at the six remaining s5.3 sites.

- [ ] **Step 1: Pin the starting state (no new test: a pure deletion of private dead code; the unchanged green suite is the regression net)**

```powershell
git grep -n -E "MoveEntitiesBetweenChunks|MoveEntitiesWithComponent\(|MoveEntitiesWithComponent<|BatchMoveEntitiesWithComponentByID" -- include tests benchmark bench-compare studio
```

Expected: exactly these 5 hits. The 3 definitions are:
- `Archetype.hpp:2124`
- `ArchetypeManager.hpp:1362`
- `ArchetypeManager.hpp:1656`

The 2 comments are at `ArchetypeChunkPool.hpp:406` and `:603`. There are NO call sites. If any call site appears, stop and report: the spec's premise no longer holds.

- [ ] **Step 2: Delete the three functions**

1. In `include/Astra/Archetype/Archetype.hpp`, delete the whole member function. It starts at:
   ```cpp
           std::vector<std::pair<Entity, EntityLocation>> MoveEntitiesBetweenChunks(size_t srcChunkIndex, size_t destChunkIndex, size_t count)
   ```
   It ends at the `return movedEntities;` line and the closing `}` that follows. Also delete the blank line between that `}` and `ComponentMask m_mask;`. Afterwards, the closing `}` of `AllocateEntitySlot` (`return EntityLocation::Create(chunkIndex, entityIndex);` then `}`) is followed by one blank line, then `ComponentMask m_mask;`.

2. In `include/Astra/Archetype/ArchetypeManager.hpp`, delete the whole member template. It starts at:
   ```cpp
           template<Component T, typename... Args>
           void MoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entities, Args&&... args)
   ```
   It ends at its closing `}`, which is the line before the blank line and `template<typename Predicate>` / `FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> GroupEntitiesByArchetype(...)`. Keep one blank line between the end of `MoveAndAdd` and `template<typename Predicate>`.

3. In the same file, delete this block in full: the doc comment, the signature and the body, up to the closing `}` before `// The ONLY sanctioned writers of a record's storage fields`.
   ```cpp
           /**
            * Type-erased version of BatchMoveEntitiesWithComponent for CommandBuffer use.
            */
           size_t BatchMoveEntitiesWithComponentByID(Archetype* srcArchetype, Archetype* dstArchetype,
                                                   SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch,
                                                   ComponentID componentId, const void* data, const ComponentDescriptor& desc)
           {
               return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch,
                   [componentId, data, &desc](Archetype* dst, const std::vector<EntityLocation>& locs)
                   {
                       // Set the new component for all entities at their new locations
                       for (const auto& location : locs)
                       {
                           auto [chunk, entityIdx] = dst->ResolveLocation(location);
                           void* dstPtr = chunk->GetComponentPointer(componentId, entityIdx);
                           desc.ConstructWith(dstPtr, data);
                       }
                   });
           }
   ```
   Keep exactly one blank line between the end of `MoveAndAddByID` and `// The ONLY sanctioned writers ...`.

4. In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, `RemoveEntity`, replace
   ```cpp
                       // moved-from source slot (mirrors Archetype::MoveEntitiesBetweenChunks).
   ```
   with
   ```cpp
                       // moved-from source slot (the same pairing Archetype::CompactChunks uses).
   ```
   Then replace
   ```cpp
           // Same-archetype relocation carry (CompactChunks / MoveEntitiesBetweenChunks):
   ```
   with
   ```cpp
           // Same-archetype relocation carry (CompactChunks):
   ```

- [ ] **Step 3: Verify the code is gone, then build and run**

```powershell
git grep -n -E "MoveEntitiesBetweenChunks|MoveEntitiesWithComponent\(|MoveEntitiesWithComponent<|BatchMoveEntitiesWithComponentByID" -- include tests benchmark bench-compare studio
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- the grep prints nothing (exit code 1);
- the build succeeds;
- the full run is green. Record the seed line.

`BatchMoveEntitiesInternal`, `BatchMoveEntitiesWithComponent`, `BatchMoveEntitiesWithoutComponent`, `MoveAndAdd` and `MoveAndAddByID` all keep their live callers.

- [ ] **Step 4: Commit**

```powershell
git add include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Archetype/ArchetypeChunkPool.hpp
git commit -m @'
refactor(archetype): delete the caller-less MoveEntitiesBetweenChunks, MoveEntitiesWithComponent and BatchMoveEntitiesWithComponentByID -- dead relocation paths that would silently break sparse presence (storage-policies spec s5.3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

### Task 3: `Storage` trait, `StorageKind`, the constants, `ComponentDescriptor::storage`

Spec s3.2, first bullet, and the sparse-slot cap paragraph. Spec s1 D1.

**Files:**
- Modify: `include/Astra/Component/Component.hpp`:
  - `:17-21`: the macros and constants;
  - after `:91`: the traits;
  - `:135`: the descriptor field.
- Modify: `include/Astra/Component/ComponentRegistry.hpp:348-353` (`MakeDescriptor` writes `desc.storage`)
- Modify: `tests/TestComponents.hpp`: add `PosS` after the `static_assert(Component<TrackedPos> && Component<TrackedVel>);` line (`:387`).
- Create: `tests/Registry/StorageDeclarationTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (canonical names):
  ```cpp
  namespace Astra {
    enum class Storage : uint8_t { Table, Sparse };
    template<typename T> struct StorageTraits;                        // T::AstraStorage if convertible, else Table
    template<typename T> inline constexpr Storage StorageOfV = StorageTraits<std::remove_const_t<T>>::value;
    enum class StorageKind : uint8_t { Table, Sparse, Universal };
    constexpr std::size_t MAX_SPARSE_COMPONENTS;   // = ASTRA_MAX_SPARSE_COMPONENTS (32)
    constexpr std::size_t SPARSE_PAGE_SIZE;        // = ASTRA_SPARSE_PAGE_SIZE (4096)
    constexpr std::size_t UNIVERSAL_PAGE_SLOTS;    // = ASTRA_UNIVERSAL_PAGE_SLOTS (1024)
    // ComponentDescriptor gains:  Storage storage = Storage::Table;
  }
  namespace Astra::Test { struct PosS; }           // Sparse twin of Position; ONE new TypeID
  ```

- [ ] **Step 1: Write the failing test**

Add `PosS` to `tests/TestComponents.hpp`, directly after `static_assert(Component<TrackedPos> && Component<TrackedVel>);`:

```cpp
    // Storage-policies suite (spec 2026-10-01 s3.2, s9.1): Position's Sparse twin --
    // same fields, ctor and equality; only the storage trait differs. ONE TypeID
    // (budget): every storage test that needs a Sparse data type reuses it.
    struct PosS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        PosS() = default;
        PosS(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
        bool operator==(const PosS&) const = default;
    };
    static_assert(Component<PosS>);
```

Create `tests/Registry/StorageDeclarationTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

// Storage-policies declaration surface (spec 2026-10-01 s3.2, s4): the Storage trait,
// the descriptor snapshot, the universal Config, and ComponentStorage's kind and
// sparse-slot tables. Registered types: Astra::Test::PosS (the one Sparse test type)
// plus existing Table types. The StorageTraitTestDetail types are compile-time only
// and mint NO ComponentID (MakeDescriptor takes the id as an argument).

namespace StorageTraitTestDetail
{
    struct MemberSparse { static constexpr auto AstraStorage = Astra::Storage::Sparse; int v; };
    struct MemberTable  { static constexpr auto AstraStorage = Astra::Storage::Table; int v; };
    struct Plain        { int v; };
    struct SpecSparse   { int v; };
    struct SparseTag    { static constexpr auto AstraStorage = Astra::Storage::Sparse; };
}
template<> struct Astra::StorageTraits<StorageTraitTestDetail::SpecSparse>
{
    static constexpr Astra::Storage value = Astra::Storage::Sparse;
};

namespace
{
    // ComponentModule::Open requires an EXPLICITLY installed TypeContext slot; the
    // module-default context keeps every id identical to TypeID<T>::Value().
    struct InstalledContext
    {
        InstalledContext()  { Astra::SetTypeContext(&Astra::DefaultTypeContext()); }
        ~InstalledContext() { Astra::SetTypeContext(nullptr); }
    };
}

TEST(StorageTrait, DetectionCoversBothSpellingsDefaultsAndConst)
{
    using namespace Astra;
    using namespace StorageTraitTestDetail;
    static_assert(StorageOfV<MemberSparse> == Storage::Sparse);
    static_assert(StorageOfV<MemberTable> == Storage::Table);
    static_assert(StorageOfV<SpecSparse> == Storage::Sparse);          // specialization escape hatch
    static_assert(StorageOfV<Plain> == Storage::Table);                // default
    static_assert(StorageOfV<int> == Storage::Table);                  // non-class T: no hard error
    static_assert(StorageOfV<const MemberSparse> == Storage::Sparse);  // const-stripped like IsEnableableV
    static_assert(StorageOfV<SparseTag> == Storage::Sparse && std::is_empty_v<SparseTag>);   // sparse tags are legal
    static_assert(StorageOfV<Test::PosS> == Storage::Sparse);
    static_assert(StorageOfV<Test::Position> == Storage::Table);
    static_assert(StorageKind::Table != StorageKind::Sparse && StorageKind::Sparse != StorageKind::Universal);
    SUCCEED();
}

TEST(StorageTrait, ConstantsMatchTheSpec)
{
    static_assert(Astra::MAX_SPARSE_COMPONENTS == 32);
    static_assert(Astra::SPARSE_PAGE_SIZE == 4096);
    static_assert(Astra::UNIVERSAL_PAGE_SLOTS == 1024);
    SUCCEED();
}

TEST(StorageTrait, DescriptorSnapshotsTheTraitOnTheAnonymousPath)
{
    // MakeDescriptor is the one builder BOTH registration paths share; a hand-built
    // descriptor defaults to Table (the safe default, like isEnableable).
    EXPECT_EQ(Astra::ComponentRegistry::MakeDescriptor<StorageTraitTestDetail::MemberSparse>(0, nullptr).storage,
              Astra::Storage::Sparse);
    EXPECT_EQ(Astra::ComponentDescriptor{}.storage, Astra::Storage::Table);

    Astra::ComponentRegistry registry;
    registry.RegisterComponents<Astra::Test::PosS, Astra::Test::Position>();
    const auto* sparse = registry.GetComponentDescriptor(Astra::TypeID<Astra::Test::PosS>::Value());
    const auto* table  = registry.GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value());
    ASSERT_NE(sparse, nullptr);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(sparse->storage, Astra::Storage::Sparse);
    EXPECT_EQ(table->storage, Astra::Storage::Table);
}

TEST(StorageTrait, DescriptorSnapshotsTheTraitOnTheModulePath)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "StorageTraitModule");
    ASSERT_TRUE(static_cast<bool>(mod));
    mod.Register<Astra::Test::PosS>();
    const auto* desc = creg->GetComponentDescriptor(Astra::TypeID<Astra::Test::PosS>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->storage, Astra::Storage::Sparse);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS. `TestComponents.hpp` reports `'Storage': is not a member of 'Astra'`, plus the follow-on errors for `StorageOfV`, `StorageTraits`, `StorageKind`, `MAX_SPARSE_COMPONENTS` and `ComponentDescriptor::storage`.

- [ ] **Step 3: Implement**

In `include/Astra/Component/Component.hpp`, replace

```cpp
    #ifndef ASTRA_MAX_COMPONENTS
        #define ASTRA_MAX_COMPONENTS 128u
    #endif

    constexpr std::size_t MAX_COMPONENTS = ASTRA_MAX_COMPONENTS;
```

with

```cpp
    #ifndef ASTRA_MAX_COMPONENTS
        #define ASTRA_MAX_COMPONENTS 128u
    #endif

    // Storage policies (spec 2026-10-01 s3.2, s4): sparse slots per registry, entries
    // per sparse-set page, slots per universal-column page. Overridable like
    // ASTRA_MAX_COMPONENTS; the constexpr mirrors below are what code reads.
    #ifndef ASTRA_MAX_SPARSE_COMPONENTS
        #define ASTRA_MAX_SPARSE_COMPONENTS 32u
    #endif
    #ifndef ASTRA_SPARSE_PAGE_SIZE
        #define ASTRA_SPARSE_PAGE_SIZE 4096u
    #endif
    #ifndef ASTRA_UNIVERSAL_PAGE_SLOTS
        #define ASTRA_UNIVERSAL_PAGE_SLOTS 1024u
    #endif

    constexpr std::size_t MAX_COMPONENTS = ASTRA_MAX_COMPONENTS;
    constexpr std::size_t MAX_SPARSE_COMPONENTS = ASTRA_MAX_SPARSE_COMPONENTS;
    constexpr std::size_t SPARSE_PAGE_SIZE = ASTRA_SPARSE_PAGE_SIZE;
    constexpr std::size_t UNIVERSAL_PAGE_SLOTS = ASTRA_UNIVERSAL_PAGE_SLOTS;

    static_assert(MAX_SPARSE_COMPONENTS >= 1 && MAX_SPARSE_COMPONENTS <= MAX_COMPONENTS && MAX_SPARSE_COMPONENTS <= 0x7FFF,
                  "ASTRA_MAX_SPARSE_COMPONENTS must be in [1, min(ASTRA_MAX_COMPONENTS, 32767)]");
    static_assert(SPARSE_PAGE_SIZE > 0 && (SPARSE_PAGE_SIZE & (SPARSE_PAGE_SIZE - 1)) == 0,
                  "ASTRA_SPARSE_PAGE_SIZE must be a power of two");
    static_assert(UNIVERSAL_PAGE_SLOTS >= 64 && (UNIVERSAL_PAGE_SLOTS & (UNIVERSAL_PAGE_SLOTS - 1)) == 0,
                  "ASTRA_UNIVERSAL_PAGE_SLOTS must be a power of two >= 64 (its bitmaps are 64-bit words)");
```

In the same file, insert directly after `inline constexpr bool IsChangeTrackedV = ChangeTrackedTraits<std::remove_const_t<T>>::value;`:

```cpp

    /**
     * Storage policy (spec 2026-10-01 s3.2, decision D1). A component TYPE declares where
     * its data lives with `static constexpr auto AstraStorage = Astra::Storage::Sparse;`
     * or by specializing Astra::StorageTraits<T>. Default: Table (archetype columns).
     * Universal is NOT a type trait: a Registry declares it (Registry::Config::AddUniversal),
     * so the EFFECTIVE layout in one registry is a StorageKind (ComponentStorage::KindOf).
     * Same if-constexpr-gated shape as EnableableTraits (the MSVC short-circuit rationale).
     */
    enum class Storage : uint8_t { Table, Sparse };

    template<typename T>
    struct StorageTraits
    {
        static constexpr Storage value = []() constexpr
        {
            if constexpr (requires { { T::AstraStorage } -> std::convertible_to<Storage>; })
                return static_cast<Storage>(T::AstraStorage);
            else
                return Storage::Table;
        }();
    };

    template<typename T>
    inline constexpr Storage StorageOfV = StorageTraits<std::remove_const_t<T>>::value;

    // Effective layout of a component in ONE registry: the trait's Table/Sparse, or
    // Universal when that registry declares it (Universal overrides the trait).
    enum class StorageKind : uint8_t { Table, Sparse, Universal };
```

In `struct ComponentDescriptor`, directly after the `bool isChangeTracked = false;` line (`:135`), add:

```cpp
        Storage storage = Storage::Table;   // the type's Storage trait (spec 2026-10-01 s3.2); hand-built descriptors default to Table. Universal is per-registry (ComponentStorage::KindOf) and never stored here.
```

In `include/Astra/Component/ComponentRegistry.hpp`, `MakeDescriptor`, directly after `desc.isChangeTracked = IsChangeTrackedV<T>;`, add:

```cpp

            // Storage trait snapshot (spec 2026-10-01 s3.2). Both registration paths
            // (RegisterComponentImpl and ComponentModule::RegisterOneChecked) build
            // through here, so both pick it up.
            desc.storage = StorageOfV<T>;
```

- [ ] **Step 4: Run the tests**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageTrait.*:EnableableTrait.*:ComponentModule.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: the filtered run PASSES (4 `StorageTrait` tests), and the full run is green. Record the seed line. `PosS` is only declared here: no code path reads `desc.storage` yet. Chunks B and C route it.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Component/Component.hpp include/Astra/Component/ComponentRegistry.hpp tests/TestComponents.hpp tests/Registry/StorageDeclarationTest.cpp
git commit -m @'
feat(storage): the Storage trait (Table/Sparse), StorageKind, the sparse/universal constants and ComponentDescriptor::storage snapshotted by MakeDescriptor on both registration paths (spec s3.2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

### Task 4: `ComponentStorage` skeleton: `UniversalSpec`, `KindOf`, the sparse-slot table

Spec s4, first paragraph (the owner), s3.2 (sparse slots; universal overrides the trait), s5.1 (an enableable Universal component also takes a slot).

**Files:**
- Create: `include/Astra/Registry/ComponentStorage.hpp`
- Modify: `include/Astra/Astra.hpp:48`. Include the new header before `Registry/Query.hpp`.
- Test: `tests/Registry/StorageDeclarationTest.cpp` (append)

**Interfaces:**
- Consumes: Task 3. `Storage`, `StorageKind`, `MAX_SPARSE_COMPONENTS`, `ComponentDescriptor::storage`.
- Produces:
  ```cpp
  namespace Astra {
    using UniversalInitFn = void (*)(Entity entity, void* component, void* user);
    struct UniversalSpec { ComponentID id = INVALID_COMPONENT; UniversalInitFn init = nullptr; void* user = nullptr;
                           void (*registerComponent)(ComponentRegistry& registry) = nullptr; };   // 4th field: see the notes
    class ComponentStorage {
    public:
      ComponentStorage(std::shared_ptr<ComponentRegistry> registry, const std::vector<UniversalSpec>& universal);
      StorageKind KindOf(ComponentID id) const noexcept;        // Universal > descriptor trait; unregistered -> Table
      int         SparseSlotOf(ComponentID id) const noexcept;  // -1 if none
      int         AcquireSparseSlot(ComponentID id);            // lowest free slot; idempotent; -1 at the cap (Debug asserts)
      ComponentID ComponentOfSlot(uint16_t slot) const noexcept;  // INVALID_COMPONENT if free / out of range
      size_t      UsedSparseSlotCount() const noexcept;
      const UniversalSpec* FindUniversalSpec(ComponentID id) const noexcept;
      const std::vector<UniversalSpec>& UniversalSpecs() const noexcept;
      void        Clear() noexcept;                             // keeps slots + declarations; stores added by Chunk B
    };
  }
  ```
  `Sparse`, `Universal`, `ConstructUniversals`, `OnEntityDestroyed`, `ReleaseComponent`, `Serialize` and `Deserialize` are added by Chunks B, C and E onto this class. A released slot is reused, because `AcquireSparseSlot` takes the LOWEST free slot. Chunk E's `ReleaseComponent` frees a slot by resetting `m_slotOf[id] = -1`, `m_componentOfSlot[slot] = INVALID_COMPONENT` and `--m_usedSlots`.

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/StorageDeclarationTest.cpp`:

```cpp
// ---------------- ComponentStorage (spec s4): kinds + sparse slots ----------------

namespace
{
    // Records and CONTINUES, so a deliberately failing ASTRA_ASSERT / ASTRA_ENSURE returns
    // into the refusal path instead of breaking or aborting the test process.
    Astra::AssertAction CountingContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++*static_cast<int*>(user);
        return Astra::AssertAction::Continue;
    }
}

TEST(ComponentStorage, KindOfReadsTheTraitAndTheUniversalList)
{
    using Astra::StorageKind;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Astra::Test::Position, Astra::Test::PosS>();
    const Astra::ComponentID pos  = Astra::TypeID<Astra::Test::Position>::Value();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    const Astra::ComponentID vel  = Astra::TypeID<Astra::Test::Velocity>::Value();   // NOT registered in creg

    Astra::ComponentStorage plain(creg, {});
    EXPECT_EQ(plain.KindOf(pos), StorageKind::Table);
    EXPECT_EQ(plain.KindOf(posS), StorageKind::Sparse);
    EXPECT_EQ(plain.KindOf(vel), StorageKind::Table);                     // unregistered: Table
    EXPECT_EQ(plain.KindOf(Astra::INVALID_COMPONENT), StorageKind::Table);
    EXPECT_TRUE(plain.UniversalSpecs().empty());

    const std::vector<Astra::UniversalSpec> specs{ Astra::UniversalSpec{pos, nullptr, nullptr},
                                                   Astra::UniversalSpec{posS, nullptr, nullptr} };
    Astra::ComponentStorage universal(creg, specs);
    EXPECT_EQ(universal.KindOf(pos), StorageKind::Universal);
    EXPECT_EQ(universal.KindOf(posS), StorageKind::Universal);            // Universal overrides the Sparse trait
    ASSERT_NE(universal.FindUniversalSpec(pos), nullptr);
    EXPECT_EQ(universal.FindUniversalSpec(vel), nullptr);
}

TEST(ComponentStorage, UniversalSpecRegistersItsTypeAndLastDeclarationWins)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const Astra::ComponentID name = Astra::TypeID<Astra::Test::Name>::Value();
    ASSERT_EQ(creg->GetComponentDescriptor(name), nullptr);
    int first = 0, second = 0;
    const auto registerName = +[](Astra::ComponentRegistry& r) { r.RegisterComponent<Astra::Test::Name>(); };
    const std::vector<Astra::UniversalSpec> specs{ Astra::UniversalSpec{name, nullptr, &first, registerName},
                                                   Astra::UniversalSpec{name, nullptr, &second, registerName} };
    Astra::ComponentStorage storage(creg, specs);
    EXPECT_NE(creg->GetComponentDescriptor(name), nullptr);      // registered through the spec's thunk
    EXPECT_EQ(storage.KindOf(name), Astra::StorageKind::Universal);
    ASSERT_EQ(storage.UniversalSpecs().size(), 1u);              // deduplicated by id
    EXPECT_EQ(storage.FindUniversalSpec(name)->user, static_cast<void*>(&second));   // last declaration wins
}

TEST(ComponentStorage, InvalidUniversalIdIsIgnored)
{
    int calls = 0;
    Astra::Testing::ScopedAssertHandler guard(&CountingContinue, &calls);
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentStorage storage(creg, { Astra::UniversalSpec{Astra::INVALID_COMPONENT, nullptr, nullptr} });
    EXPECT_TRUE(storage.UniversalSpecs().empty());
    EXPECT_EQ(storage.KindOf(Astra::INVALID_COMPONENT), Astra::StorageKind::Table);
}

TEST(ComponentStorage, SparseSlotsAreDenseIdempotentAndPerStorage)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    const Astra::ComponentID pos  = Astra::TypeID<Astra::Test::Position>::Value();
    Astra::ComponentStorage a(creg, {});
    Astra::ComponentStorage b(creg, {});

    EXPECT_EQ(a.SparseSlotOf(posS), -1);
    EXPECT_EQ(a.AcquireSparseSlot(posS), 0);
    EXPECT_EQ(a.AcquireSparseSlot(posS), 0);                      // idempotent
    EXPECT_EQ(a.AcquireSparseSlot(pos), 1);                       // kind not checked: enableable Universal ids take slots too (s5.1)
    EXPECT_EQ(a.SparseSlotOf(posS), 0);
    EXPECT_EQ(a.ComponentOfSlot(0), posS);
    EXPECT_EQ(a.ComponentOfSlot(1), pos);
    EXPECT_EQ(a.ComponentOfSlot(2), Astra::INVALID_COMPONENT);
    EXPECT_EQ(a.ComponentOfSlot(static_cast<uint16_t>(Astra::MAX_SPARSE_COMPONENTS)), Astra::INVALID_COMPONENT);
    EXPECT_EQ(a.UsedSparseSlotCount(), 2u);

    // A shared ComponentRegistry does not couple slot spaces (spec s3.2).
    EXPECT_EQ(b.AcquireSparseSlot(pos), 0);
    EXPECT_EQ(b.AcquireSparseSlot(posS), 1);
    EXPECT_EQ(a.SparseSlotOf(posS), 0);

    a.Clear();                                                    // slot assignments survive Clear (s7.3)
    EXPECT_EQ(a.SparseSlotOf(posS), 0);
    EXPECT_EQ(a.SparseSlotOf(pos), 1);
}

TEST(ComponentStorage, SparseSlotCapRefusesBeyondMaxSparseComponents)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentStorage storage(creg, {});
    // Raw ids: the slot table never consults a descriptor, so no TypeID is minted.
    for (size_t i = 0; i < Astra::MAX_SPARSE_COMPONENTS; ++i)
        ASSERT_EQ(storage.AcquireSparseSlot(static_cast<Astra::ComponentID>(i)), static_cast<int>(i));
    ASSERT_EQ(storage.UsedSparseSlotCount(), Astra::MAX_SPARSE_COMPONENTS);

    const auto overflow = static_cast<Astra::ComponentID>(Astra::MAX_SPARSE_COMPONENTS);   // 32 < MAX_COMPONENTS
    int calls = 0;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountingContinue, &calls);
        EXPECT_EQ(storage.AcquireSparseSlot(overflow), -1);
    }
#if MOSAIC_ASSERTS_ACTIVE
    EXPECT_EQ(calls, 1);                                          // Debug: assert, then the refusal
#endif
    EXPECT_EQ(storage.SparseSlotOf(overflow), -1);
    EXPECT_EQ(storage.UsedSparseSlotCount(), Astra::MAX_SPARSE_COMPONENTS);
    EXPECT_EQ(storage.AcquireSparseSlot(5), 5);                   // existing assignments still answer
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS with `'ComponentStorage': is not a member of 'Astra'` and `'UniversalSpec': is not a member of 'Astra'`.

- [ ] **Step 3: Implement**

Create `include/Astra/Registry/ComponentStorage.hpp`:

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Core/Assert.hpp"
#include "../Core/Base.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    // ============ Storage policies: the per-registry owner (spec 2026-10-01 s4) ============
    //
    // ComponentStorage sits beside ArchetypeManager inside a Registry. It answers "where does
    // component `id` live in THIS registry" (KindOf), owns the per-registry sparse-slot table
    // (s3.2: a Sparse component -- or an enableable Universal one, s5.1 -- gets a dense slot in
    // [0, MAX_SPARSE_COMPONENTS) the first time this registry stores it; slot spaces are per
    // registry, so a shared ComponentRegistry couples nothing), and keeps the registry's
    // universal declarations. The sparse sets and universal columns are added on top of this
    // class (Registry/SparseSet.hpp, Registry/UniversalColumn.hpp).

    // Universal-component initializer (spec s3.2, s4.2): runs once per entity AFTER its slot
    // was default-constructed, and ONLY when the creator supplied no value.
    using UniversalInitFn = void (*)(Entity entity, void* component, void* user);

    // One universal declaration (Registry::Config::universal). `registerComponent` is filled by
    // Config::AddUniversal<T>: a Config has no ComponentRegistry to register T into, so the
    // owning registry's ComponentStorage calls it at construction (and again on Load). A
    // hand-built spec may leave it null; T must then already be registered.
    struct UniversalSpec
    {
        ComponentID id = INVALID_COMPONENT;
        UniversalInitFn init = nullptr;
        void* user = nullptr;
        void (*registerComponent)(ComponentRegistry& registry) = nullptr;
    };

    class ComponentStorage
    {
    public:
        ComponentStorage(std::shared_ptr<ComponentRegistry> registry, const std::vector<UniversalSpec>& universal) :
            m_registry(std::move(registry))
        {
            m_slotOf.fill(-1);
            m_componentOfSlot.fill(INVALID_COMPONENT);
            for (const UniversalSpec& spec : universal)
            {
                if (!ASTRA_ENSURE(spec.id < MAX_COMPONENTS,
                        "UniversalSpec: invalid ComponentID (unregistrable or id-refused type) -- declaration ignored")) ASTRA_UNLIKELY
                    continue;
                if (spec.registerComponent && m_registry)
                    spec.registerComponent(*m_registry);
                if (UniversalSpec* existing = FindUniversalSpecMutable(spec.id))
                    *existing = spec;   // last declaration wins (Config::AddUniversal's rule)
                else
                    m_universal.push_back(spec);
                m_universalMask.Set(spec.id);
            }
        }

        ComponentStorage(const ComponentStorage&) = delete;
        ComponentStorage& operator=(const ComponentStorage&) = delete;

        // Effective layout of `id` in this registry: Universal wins over the trait, otherwise
        // the registered descriptor's Storage trait decides. An unregistered or out-of-range id
        // answers Table, so the archetype path refuses it exactly as today. Never cached: a
        // module unload or rebind may swap the descriptor.
        ASTRA_NODISCARD StorageKind KindOf(ComponentID id) const noexcept
        {
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return StorageKind::Table;
            if (m_universalMask.Test(id))
                return StorageKind::Universal;
            const ComponentDescriptor* desc = m_registry ? m_registry->GetComponentDescriptor(id) : nullptr;
            return (desc && desc->storage == Storage::Sparse) ? StorageKind::Sparse : StorageKind::Table;
        }

        // Dense slot of `id`, or -1 while this registry has not stored it.
        ASTRA_NODISCARD int SparseSlotOf(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS ? m_slotOf[id] : -1;
        }

        // Assigns `id` the LOWEST free slot on first use; idempotent afterwards. Callers pass a
        // Sparse component, or an enableable Universal one (its disabled mirror, spec s5.1) --
        // the kind is deliberately not checked here. Exhausting the cap is a registration-time
        // assert + refusal (spec s3.2): Debug asserts, every config returns -1 and the caller's
        // add then fails.
        int AcquireSparseSlot(ComponentID id)
        {
            if (!ASTRA_ENSURE(id < MAX_COMPONENTS, "AcquireSparseSlot: invalid ComponentID")) ASTRA_UNLIKELY
                return -1;
            if (m_slotOf[id] >= 0)
                return m_slotOf[id];
            if (m_usedSlots >= MAX_SPARSE_COMPONENTS) ASTRA_UNLIKELY
            {
                ASTRA_ASSERT(m_usedSlots < MAX_SPARSE_COMPONENTS,
                    "Sparse slot cap exhausted: raise ASTRA_MAX_SPARSE_COMPONENTS (spec 2026-10-01 s3.2)");
                return -1;
            }
            for (size_t s = 0; s < MAX_SPARSE_COMPONENTS; ++s)
            {
                if (m_componentOfSlot[s] == INVALID_COMPONENT)
                {
                    m_componentOfSlot[s] = id;
                    m_slotOf[id] = static_cast<int16_t>(s);
                    ++m_usedSlots;
                    return static_cast<int>(s);
                }
            }
            return -1;   // not reached: m_usedSlots < cap guarantees a free slot
        }

        ASTRA_NODISCARD ComponentID ComponentOfSlot(uint16_t slot) const noexcept
        {
            return slot < MAX_SPARSE_COMPONENTS ? m_componentOfSlot[slot] : INVALID_COMPONENT;
        }

        ASTRA_NODISCARD size_t UsedSparseSlotCount() const noexcept { return m_usedSlots; }

        ASTRA_NODISCARD const UniversalSpec* FindUniversalSpec(ComponentID id) const noexcept
        {
            for (const UniversalSpec& spec : m_universal)
                if (spec.id == id)
                    return &spec;
            return nullptr;
        }

        ASTRA_NODISCARD const std::vector<UniversalSpec>& UniversalSpecs() const noexcept { return m_universal; }

        // Registry::Clear(): destroys every stored element IN PLACE (cached Views stay valid).
        // Slot assignments and universal declarations survive (spec s7.3). There are no stores
        // yet, so there is nothing to destroy; the SparseSet / UniversalColumn tasks extend it.
        void Clear() noexcept {}

    private:
        UniversalSpec* FindUniversalSpecMutable(ComponentID id) noexcept
        {
            for (UniversalSpec& spec : m_universal)
                if (spec.id == id)
                    return &spec;
            return nullptr;
        }

        std::shared_ptr<ComponentRegistry> m_registry;
        std::vector<UniversalSpec> m_universal;                              // deduplicated by id, declaration order
        ComponentMask m_universalMask{};                                     // id -> declared universal here
        std::array<int16_t, MAX_COMPONENTS> m_slotOf{};                      // id -> sparse slot, -1 = none
        std::array<ComponentID, MAX_SPARSE_COMPONENTS> m_componentOfSlot{};  // slot -> id, INVALID_COMPONENT = free
        size_t m_usedSlots = 0;
    };
}
```

In `include/Astra/Astra.hpp`, replace

```cpp
// Registry and queries
#include "Registry/Query.hpp"
```

with

```cpp
// Registry and queries
#include "Registry/ComponentStorage.hpp"
#include "Registry/Query.hpp"
```

- [ ] **Step 4: Run the tests**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ComponentStorage.*:StorageTrait.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: 5 `ComponentStorage` tests PASS, and the full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Registry/ComponentStorage.hpp include/Astra/Astra.hpp tests/Registry/StorageDeclarationTest.cpp
git commit -m @'
feat(storage): ComponentStorage skeleton -- UniversalSpec/UniversalInitFn, KindOf (universal overrides the trait), the per-registry sparse-slot table with the 32-slot cap (spec s3.2, s4)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

### Task 5: `Registry::Config::AddUniversal<T>` and Registry ownership of `ComponentStorage`

Spec s3.2 (the Universal bullet and D1), s4 (owned by `Registry` beside `ArchetypeManager`, rebuilt fresh on `Load`), s7.3 (`Clear` keeps slot assignments).

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp`:
  - `:13-33`: the includes;
  - `:40-50`: `Config`;
  - `:52-81`: the three constructors;
  - `:1281-1286`: `Clear`;
  - `:1314-1315`: the accessors;
  - `:1875`: `LoadInternal`, the rebuild;
  - `:1910`: the member.
- Test: `tests/Registry/StorageDeclarationTest.cpp` (append)

**Interfaces:**
- Consumes: Task 4 (`ComponentStorage`, `UniversalSpec`, `UniversalInitFn`).
- Produces:
  ```cpp
  struct Registry::Config {
    // ...existing members unchanged...
    using UniversalInitFn = Astra::UniversalInitFn;
    using UniversalSpec   = Astra::UniversalSpec;
    std::vector<UniversalSpec> universal;
    template<Component T> Config& AddUniversal(UniversalInitFn init = nullptr, void* user = nullptr);   // last declaration wins
  };
  ComponentStorage*       Registry::GetComponentStorage() noexcept;
  const ComponentStorage* Registry::GetComponentStorage() const noexcept;
  // private: std::shared_ptr<ComponentStorage> m_componentStorage;   (declared right after m_archetypeManager)
  ```
  Later chunks reach the store as `m_componentStorage->...` inside `Registry`, and as `GetComponentStorage()->...` from tests and tools (it returns a pointer). It is a `shared_ptr`, matching `m_archetypeManager`. (`View` and `Relations` reach it non-owningly through `ArchetypeManager::GetComponentStorage()`, Task 22, and re-resolve store pointers at every entry point.)

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/StorageDeclarationTest.cpp`:

```cpp
// ---------------- Registry::Config::AddUniversal + Registry ownership (spec s3.2, s4) ----------------

TEST(StorageConfig, AddUniversalRecordsTheSpecAndChains)
{
    using Astra::Test::Name;
    Astra::Registry::Config config;
    int token = 0;
    const Astra::Registry::Config::UniversalInitFn init = +[](Astra::Entity, void*, void*) {};
    Astra::Registry::Config& chained = config.AddUniversal<Name>(init, &token);
    EXPECT_EQ(&chained, &config);
    ASSERT_EQ(config.universal.size(), 1u);
    EXPECT_EQ(config.universal[0].id, Astra::TypeID<Name>::Value());
    EXPECT_TRUE(config.universal[0].init == init);
    EXPECT_EQ(config.universal[0].user, static_cast<void*>(&token));
    EXPECT_TRUE(config.universal[0].registerComponent != nullptr);

    config.AddUniversal<Name>();                                  // re-declare: replaces, never duplicates
    ASSERT_EQ(config.universal.size(), 1u);
    EXPECT_TRUE(config.universal[0].init == nullptr);
    EXPECT_EQ(config.universal[0].user, nullptr);
}

TEST(StorageConfig, RegistryRegistersAndClassifiesDeclaredUniversals)
{
    using Astra::StorageKind;
    Astra::Registry::Config config;
    config.AddUniversal<Astra::Test::Name>();
    config.AddUniversal<Astra::Test::PosS>();                     // a Sparse-trait type declared universal
    Astra::Registry reg(config);
    const Astra::ComponentID name = Astra::TypeID<Astra::Test::Name>::Value();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    const Astra::ComponentID pos  = Astra::TypeID<Astra::Test::Position>::Value();

    ASSERT_NE(reg.GetComponentStorage(), nullptr);
    EXPECT_NE(reg.GetComponentRegistry()->GetComponentDescriptor(name), nullptr);   // registered by the Registry
    EXPECT_NE(reg.GetComponentRegistry()->GetComponentDescriptor(posS), nullptr);
    EXPECT_EQ(reg.GetComponentStorage()->KindOf(name), StorageKind::Universal);
    EXPECT_EQ(reg.GetComponentStorage()->KindOf(posS), StorageKind::Universal);    // universal overrides the trait
    EXPECT_EQ(reg.GetComponentStorage()->KindOf(pos), StorageKind::Table);

    Astra::Registry plain;                                        // no declaration: the trait decides
    plain.GetComponentRegistry()->RegisterComponent<Astra::Test::PosS>();
    EXPECT_EQ(plain.GetComponentStorage()->KindOf(name), StorageKind::Table);
    EXPECT_EQ(plain.GetComponentStorage()->KindOf(posS), StorageKind::Sparse);

    Astra::Registry legacy(Astra::EntityManager::Config{}, Astra::ArchetypeChunkPool::Config{});   // the two-config ctor
    ASSERT_NE(legacy.GetComponentStorage(), nullptr);
    EXPECT_TRUE(legacy.GetComponentStorage()->UniversalSpecs().empty());
}

TEST(StorageConfig, SharedComponentRegistryDoesNotShareUniversalityOrSlots)
{
    using Astra::StorageKind;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Astra::Test::PosS, Astra::Test::Velocity>();
    Astra::Registry::Config withName;
    withName.AddUniversal<Astra::Test::Name>();
    Astra::Registry a(creg, withName);
    Astra::Registry b(creg);
    const Astra::ComponentID name = Astra::TypeID<Astra::Test::Name>::Value();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    const Astra::ComponentID vel  = Astra::TypeID<Astra::Test::Velocity>::Value();

    EXPECT_EQ(a.GetComponentStorage()->KindOf(name), StorageKind::Universal);
    EXPECT_EQ(b.GetComponentStorage()->KindOf(name), StorageKind::Table);   // D1: universality is per registry
    EXPECT_EQ(a.GetComponentStorage()->AcquireSparseSlot(posS), 0);
    EXPECT_EQ(b.GetComponentStorage()->AcquireSparseSlot(vel), 0);
    EXPECT_EQ(b.GetComponentStorage()->AcquireSparseSlot(posS), 1);
    EXPECT_EQ(a.GetComponentStorage()->SparseSlotOf(posS), 0);
    EXPECT_EQ(a.GetComponentStorage()->SparseSlotOf(vel), -1);
}

TEST(StorageConfig, ClearKeepsDeclarationsAndSlotsInPlace)
{
    Astra::Registry::Config config;
    config.AddUniversal<Astra::Test::Name>();
    Astra::Registry reg(config);
    reg.GetComponentRegistry()->RegisterComponent<Astra::Test::PosS>();
    const Astra::ComponentID name = Astra::TypeID<Astra::Test::Name>::Value();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    ASSERT_EQ(reg.GetComponentStorage()->AcquireSparseSlot(posS), 0);
    const Astra::ComponentStorage* before = reg.GetComponentStorage();
    reg.CreateEntity<Astra::Test::Position>();

    reg.Clear();
    EXPECT_EQ(reg.GetComponentStorage(), before);                 // cleared in place, never replaced
    EXPECT_EQ(reg.GetComponentStorage()->KindOf(name), Astra::StorageKind::Universal);
    EXPECT_EQ(reg.GetComponentStorage()->SparseSlotOf(posS), 0);
    EXPECT_EQ(reg.Size(), 0u);
}

TEST(StorageConfig, LoadRebuildsStorageFromTheCallersConfig)
{
    using Astra::StorageKind;
    Astra::Registry::Config config;
    config.AddUniversal<Astra::Test::Name>();
    Astra::Registry src(config);
    src.CreateEntity<Astra::Test::Position>();
    auto saved = src.Save();
    ASSERT_TRUE(saved.IsOk());
    const Astra::ComponentID name = Astra::TypeID<Astra::Test::Name>::Value();

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Astra::Test::Position>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg, config);
    ASSERT_TRUE(loaded.IsOk());
    Astra::Registry& reg = **loaded.GetValue();
    ASSERT_NE(reg.GetComponentStorage(), nullptr);
    EXPECT_EQ(reg.GetComponentStorage()->KindOf(name), StorageKind::Universal);
    EXPECT_NE(creg->GetComponentDescriptor(name), nullptr);       // the Config registered Name in the LOADING registry
    EXPECT_EQ(reg.Size(), 1u);

    auto creg2 = std::make_shared<Astra::ComponentRegistry>();
    creg2->RegisterComponents<Astra::Test::Position, Astra::Test::Name>();
    auto plain = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg2);   // default Config
    ASSERT_TRUE(plain.IsOk());
    EXPECT_EQ((*plain.GetValue())->GetComponentStorage()->KindOf(name), StorageKind::Table);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS. The errors are `'AddUniversal': is not a member of 'Astra::Registry::Config'`, `'universal': is not a member of ...`, `'UniversalInitFn': is not a member of 'Astra::Registry::Config'` and `'GetComponentStorage': is not a member of 'Astra::Registry'`.

- [ ] **Step 3: Implement**

All edits are in `include/Astra/Registry/Registry.hpp`.

**(a) Include.** After `#include "../Serialization/SerializationError.hpp"`, add:

```cpp
#include "ComponentStorage.hpp"
```

**(b) Config.** Replace the `Config` struct:

```cpp
        struct Config
        {
            EntityManager::Config entityManagerConfig;
            ArchetypeChunkPool::Config chunkPoolConfig;
            ResourceStorage::Config resourceStorageConfig;
            // Scheduler used by parallel iteration/execution. Astra creates no threads:
            // null (the default) means every Parallel* API runs sequentially inline.
            // Hosts inject one shared instance (e.g. an enkiTS adapter) -- and in
            // multi-module (DLL) setups, the SAME instance into every module.
            std::shared_ptr<IWorkScheduler> workScheduler;

            // Universal components (spec 2026-10-01 s3.2, D1): every entity of a registry built
            // from this Config holds them, in a slot-indexed column rather than an archetype
            // column. Retained in m_config, so it survives Clear(); Load builds the new registry
            // with the caller's Config, so Load honours it too. No NSDMI here on purpose: the
            // `const Config& = {}` default arguments below need Config free of NSDMIs on gcc/clang.
            using UniversalInitFn = Astra::UniversalInitFn;
            using UniversalSpec = Astra::UniversalSpec;
            std::vector<UniversalSpec> universal;

            // Declares T universal. `init` (optional) runs, with `user`, for each new entity that
            // was NOT given a T value (spec s4.2). Re-declaring T replaces init/user (last wins).
            // Registers nothing itself: the Registry registers T in ITS ComponentRegistry when
            // it is constructed (UniversalSpec::registerComponent).
            template<Component T>
            Config& AddUniversal(UniversalInitFn init = nullptr, void* user = nullptr)
            {
                const ComponentID id = TypeID<T>::Value();
                if (!ASTRA_ENSURE(id < MAX_COMPONENTS, "AddUniversal: the type has no valid ComponentID")) ASTRA_UNLIKELY
                    return *this;
                for (UniversalSpec& spec : universal)
                {
                    if (spec.id == id)
                    {
                        spec.init = init;
                        spec.user = user;
                        return *this;
                    }
                }
                universal.push_back(UniversalSpec{id, init, user,
                    +[](ComponentRegistry& registry) { registry.RegisterComponent<T>(); }});
                return *this;
            }
        };
```

**(c) Constructors.** In each of the three constructors, add the `m_componentStorage` initializer directly after the `m_archetypeManager(...)` initializer. The member is declared right after `m_archetypeManager` (step f), so the initializer order matches the declaration order (`-Wreorder` is an error on the Linux lane).

```cpp
        explicit Registry(const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig, &m_entityManager.GetRecordTable())),
            m_componentStorage(std::make_shared<ComponentStorage>(m_componentRegistry, config.universal)),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_workScheduler(config.workScheduler),
            m_config(config),
            m_instanceId(s_nextInstanceId.fetch_add(1, std::memory_order_relaxed))
        {}

        Registry(const EntityManager::Config& entityConfig, const ArchetypeChunkPool::Config& chunkConfig) :
            m_entityManager(entityConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, chunkConfig, &m_entityManager.GetRecordTable())),
            m_componentStorage(std::make_shared<ComponentStorage>(m_componentRegistry, std::vector<UniversalSpec>{})),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry),
            m_workScheduler(nullptr),
            m_config{entityConfig, chunkConfig, {}, nullptr},
            m_instanceId(s_nextInstanceId.fetch_add(1, std::memory_order_relaxed))
        {}

        Registry(std::shared_ptr<ComponentRegistry> componentRegistry, const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(std::move(componentRegistry)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig, &m_entityManager.GetRecordTable())),
            m_componentStorage(std::make_shared<ComponentStorage>(m_componentRegistry, config.universal)),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_workScheduler(config.workScheduler),
            m_config(config),
            m_instanceId(s_nextInstanceId.fetch_add(1, std::memory_order_relaxed))
        {}
```

`m_config{entityConfig, chunkConfig, {}, nullptr}` stays valid: `universal` is the trailing aggregate member and is value-initialized.

**(d) `Clear`.** Replace it with:

```cpp
        void Clear()
        {
            m_componentStorage->Clear();   // in place -- slot assignments + universal declarations survive (spec s7.3)
            m_archetypeManager->Clear();   // in place -- keeps cached Views/Relations valid
            m_relationshipGraph->Clear();
            m_entityManager.Clear();
        }
```

**(e) Accessors.** Directly after `ASTRA_NODISCARD const ArchetypeManager* GetArchetypeManager() const noexcept { return m_archetypeManager.get(); }`, add:

```cpp
        // Storage policies (spec 2026-10-01 s4): component kinds, sparse slots, and the
        // sparse / universal stores of THIS registry.
        ASTRA_NODISCARD ComponentStorage* GetComponentStorage() noexcept { return m_componentStorage.get(); }
        ASTRA_NODISCARD const ComponentStorage* GetComponentStorage() const noexcept { return m_componentStorage.get(); }
```

**(f) Member.** Replace

```cpp
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
```

with

```cpp
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        // Storage policies (spec 2026-10-01 s4): built in every ctor from Config::universal,
        // cleared in place by Clear(), rebuilt fresh by LoadInternal. Declared right after
        // m_archetypeManager: every ctor initializes it in that order.
        std::shared_ptr<ComponentStorage> m_componentStorage;
```

**(g) `LoadInternal`.** Directly after

```cpp
            registry->m_archetypeManager = std::make_shared<ArchetypeManager>(componentRegistry, config.chunkPoolConfig, &registry->m_entityManager.GetRecordTable());
```

add:

```cpp
            // Storage policies (spec 2026-10-01 s4): rebuilt fresh beside the archetype manager,
            // from the CALLER's Config, so the loaded registry's universal set is the loading
            // build's (storage-agnostic load, s8.3), never the archive's.
            registry->m_componentStorage = std::make_shared<ComponentStorage>(componentRegistry, config.universal);
```

**Ordering requirement (binding for Tasks 9, 22, 31-34).** This rebuild MUST stay BEFORE `if (!registry->m_archetypeManager->Deserialize(reader))`. Task 33 diverts Table-in-archive values of now-Sparse/Universal components into this store DURING that call; a store rebuilt afterwards would discard them. Later tasks append, in this order, directly after this line: Task 9 (c) `SetTickSource`, then Task 22 `SetComponentStorage`. The resulting sequence is: rebuild ArchetypeManager -> rebuild ComponentStorage -> `SetTickSource` -> `SetComponentStorage` -> `m_archetypeManager->Deserialize(reader, &hooks)` (Task 33) -> `m_componentStorage->Deserialize` (Task 31/32) -> relationship graph -> resources -> checksum -> `FinishStorageLoad` (Task 32).

- [ ] **Step 4: Run the tests**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageConfig.*:ComponentStorage.*:ConfigPreservation*:RegistryLoadConfig*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- 5 `StorageConfig` tests PASS;
- the existing config-preservation and load-config suites are unchanged and green;
- the full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Registry/StorageDeclarationTest.cpp
git commit -m @'
feat(storage): Registry::Config::AddUniversal<T> and Registry-owned ComponentStorage -- registered at construction, cleared in place, rebuilt from the caller's Config on Load (spec s3.2, s4)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

### Task 6: Enableable Table tags via bit-only columns

Spec s3.2, second bullet ("Every tag may be enableable, Table tags included"), and s12 item 5. An enableable empty type gets an `ArchetypeColumnMeta` entry with `stride 0`, no data region, and a disabled-word region. `idToColumn >= 0` for it, so `SetEnabledByID`, `IsEnabledByID`, every view filter path and every bit carry work unchanged. Non-enableable tags keep having no column. The tag/enableable `static_assert` is deleted. Serialization is Task 7.

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp:355-362`. Delete the `static_assert`.
- Modify: `include/Astra/Component/Component.hpp:176-192` and `:214-224`. Make `ConstructWith` and `MoveConstruct` no-ops for `size == 0`.
- Modify: `include/Astra/Archetype/Archetype.hpp`:
  - `:66`, `:75`: `BuildColumnMeta`;
  - `:172-194`: `ComputeLayoutBytesForCapacity`;
  - `:326-329`: the `Initialize` overhead.
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp`:
  - `:27-30`, `:36`, `:41`: `ArchetypeColumnMeta` comments;
  - `:313-321`: `BatchConstructComponent`;
  - `:452-471`: `GetComponent<T>`;
  - `:734-746`: `InitializeColumns`.
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp`:
  - `:600-613`: `MarkWritten`;
  - `:1326-1329`: `MoveAndAdd`;
  - `:1599-1602`: `MoveAndAddByID`.

  Anchors are at `975cdb7`; Task 2 shifted the later ones, so locate them by content.
- Modify: `include/Astra/Registry/Registry.hpp`:
  - `:598` and `:616`: `IsChanged` / `IsAdded`;
  - `:699`: the `SetEnabledByID` signal payload.
- Modify: `tests/TestComponents.hpp`: add `EnTag` after `PosS`.
- Create: `tests/Registry/EnableableTagTest.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 3-5. It is independent of the storage declaration.
- Produces:
  - `Astra::Test::EnTag`: an empty type with `AstraEnableable = true`. ONE new TypeID.
  - For an archetype holding an enableable tag `T`:
    - `GetColumnMeta().idToColumn[id(T)] >= 0` and `columns[col].stride == 0`;
    - `chunk->GetDisabledWords(col) != nullptr`;
    - `chunk->GetComponentArray<T>() == nullptr`;
    - `chunk->GetComponentArrayByID(id)` is NON-null: an in-arena address that is never dereferenced.
  - `IsChanged` / `IsAdded` / `Modified` stay `false` for every tag.
  - By-ID enable signals carry `EmptyComponentSentinel()` for a tag.

- [ ] **Step 1: Write the failing test**

Add `EnTag` to `tests/TestComponents.hpp`, directly after `static_assert(Component<PosS>);`:

```cpp
    // Enableable Table tag (spec 2026-10-01 s3.2, second bullet): an empty type that opts
    // into AstraEnableable gets a BIT-ONLY archetype column. ONE TypeID (budget).
    struct EnTag
    {
        static constexpr bool AstraEnableable = true;
    };
    static_assert(Component<EnTag> && std::is_empty_v<EnTag>);
```

Create `tests/Registry/EnableableTagTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>

#include <bit>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "../TestComponents.hpp"

// Enableable Table tags (spec 2026-10-01 s3.2, second bullet): an enableable EMPTY type
// gets a bit-only column -- stride 0, no data region, a disabled-word region -- so
// SetEnabled / IsEnabled / view filtering / every bit carry behave exactly as for an
// enableable data component (parity). EnTag is the suite's one enableable tag.

using Astra::Test::EnTag;
using Astra::Test::Player;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace
{
    // disabledCount == popcount(words) and no bit at a slot >= count, on every enableable
    // column of every chunk (RegistryTest.cpp's invariant seam, restated for this suite).
    void ExpectBitInvariant(Astra::Registry& reg)
    {
        for (Astra::Archetype* arch : reg.GetArchetypeManager()->GetArchetypes())
        {
            const Astra::ArchetypeColumnMeta& meta = arch->GetColumnMeta();
            for (const auto& chunkPtr : arch->GetChunks())
            {
                Astra::ArchetypeChunk& chunk = *chunkPtr;
                const size_t words = (chunk.GetCapacity() + 63) / 64;
                for (uint16_t e = 0; e < meta.enableableColumnCount; ++e)
                {
                    const int col = static_cast<int>(meta.enableableColumns[e]);
                    const uint64_t* w = chunk.GetDisabledWords(col);
                    ASSERT_NE(w, nullptr) << "enableable column " << col << " must carry disabled words";
                    uint32_t pop = 0;
                    for (size_t i = 0; i < words; ++i)
                        pop += static_cast<uint32_t>(std::popcount(w[i]));
                    EXPECT_EQ(pop, chunk.GetDisabledCount(col));
                    for (size_t i = chunk.GetCount(); i < words * 64; ++i)
                        EXPECT_FALSE(chunk.IsDisabled(col, i)) << "tail bit at slot " << i;
                }
            }
        }
    }

    const Astra::EntityRecord* RecordOf(Astra::Registry& reg, Astra::Entity e)
    {
        return reg.GetArchetypeManager()->GetEntityRecord(e);
    }
}

```cpp
TEST(EnableableTag, RegistersAndGetsABitOnlyColumn)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity<Position, EnTag, Player>();
    const Astra::ComponentID tagId = Astra::TypeID<EnTag>::Value();
    const auto* desc = reg.GetComponentRegistry()->GetComponentDescriptor(tagId);
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(desc->is_empty);
    EXPECT_EQ(desc->size, 0u);
    EXPECT_TRUE(desc->isEnableable);

    const Astra::EntityRecord* rec = RecordOf(reg, e);
    ASSERT_NE(rec, nullptr);
    const Astra::ArchetypeColumnMeta& meta = rec->archetype->GetColumnMeta();
    const int col = meta.idToColumn[tagId];
    ASSERT_GE(col, 0);                                            // the tag HAS a column...
    EXPECT_EQ(meta.columns[col].stride, 0u);                      // ...that carries no data
    EXPECT_EQ(meta.enableableColumnCount, 1u);
    EXPECT_EQ(meta.idToColumn[Astra::TypeID<Player>::Value()], -1);   // a non-enableable tag still has none
    EXPECT_EQ(rec->chunk->GetComponentArray<EnTag>(), nullptr);   // no data region
    EXPECT_NE(rec->chunk->GetDisabledWords(col), nullptr);        // but a disabled-word region
}

TEST(EnableableTag, ToggleBehavesLikeAnEnableableDataComponent)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentEnabled);
    reg.EnableSignals(Astra::Signal::ComponentDisabled);
    int enabled = 0, disabled = 0;
    void* lastPayload = nullptr;
    reg.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const auto& ev) { ++disabled; lastPayload = ev.component; });
    reg.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const auto& ev) { ++enabled; lastPayload = ev.component; });

    const Astra::Entity e = reg.CreateEntity<Position, EnTag>();
    EXPECT_TRUE(reg.IsEnabled<EnTag>(e));                         // born enabled
    EXPECT_TRUE(reg.SetEnabled<EnTag>(e, false));
    EXPECT_FALSE(reg.IsEnabled<EnTag>(e));
    EXPECT_TRUE(reg.SetEnabled<EnTag>(e, false));                 // idempotent: true (present), silent
    EXPECT_TRUE(reg.HasComponent<EnTag>(e));                      // existence never lies
    EXPECT_NE(reg.GetComponent<EnTag>(e), nullptr);
    EXPECT_TRUE(reg.SetEnabled<EnTag>(e, true));
    EXPECT_EQ(disabled, 1);
    EXPECT_EQ(enabled, 1);
    EXPECT_EQ(lastPayload, Astra::EmptyComponentSentinel());      // by-ID tag payload convention

    const Astra::Entity bare = reg.CreateEntity<Position>();
    EXPECT_FALSE(reg.SetEnabled<EnTag>(bare, false));             // absent
    EXPECT_FALSE(reg.IsEnabled<EnTag>(bare));
    reg.DestroyEntity(e);
    EXPECT_FALSE(reg.SetEnabled<EnTag>(e, true));                 // stale handle
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BitSurvivesSwapRemoveIncludingTheTailItself)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i)
        es.push_back(reg.CreateEntity<Position, EnTag>());
    reg.SetEnabled<EnTag>(es[7], false);                          // the tail slot
    reg.DestroyEntity(es[2]);                                     // the disabled tail swaps into slot 2
    EXPECT_FALSE(reg.IsEnabled<EnTag>(es[7]));
    reg.SetEnabled<EnTag>(es[6], false);
    reg.DestroyEntity(es[6]);                                     // the removed slot IS the tail now
    EXPECT_FALSE(reg.IsEnabled<EnTag>(es[7]));
    for (int i : {0, 1, 3, 4, 5})
        EXPECT_TRUE(reg.IsEnabled<EnTag>(es[i])) << i;
    const Astra::EntityRecord* rec = RecordOf(reg, es[7]);
    const int col = rec->archetype->GetColumnMeta().idToColumn[Astra::TypeID<EnTag>::Value()];
    EXPECT_EQ(rec->chunk->GetDisabledCount(col), 1u);
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BitSurvivesArchetypeTransitionsAndAddsBornEnabled)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity<Position, EnTag>();
    reg.SetEnabled<EnTag>(e, false);
    reg.AddComponent(e, Velocity{1.0f, 2.0f, 3.0f});              // MoveAndAdd: shared bit-only column carries
    EXPECT_FALSE(reg.IsEnabled<EnTag>(e));
    reg.RemoveComponent<Velocity>(e);                             // MoveEntityFrom: carries back
    EXPECT_FALSE(reg.IsEnabled<EnTag>(e));
    EXPECT_TRUE(reg.RemoveComponent<EnTag>(e));
    EXPECT_FALSE(reg.HasComponent<EnTag>(e));
    EXPECT_TRUE(reg.AddComponent(e, EnTag{}));                    // MoveAndAdd<EnTag>: the new column, born enabled
    EXPECT_TRUE(reg.IsEnabled<EnTag>(e));

    const Astra::Entity f = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.AddComponentByID(f, Astra::TypeID<EnTag>::Value(), nullptr, 0));   // MoveAndAddByID, null data
    EXPECT_TRUE(reg.HasComponent<EnTag>(f));
    EXPECT_TRUE(reg.IsEnabled<EnTag>(f));
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BatchAddLeavesNeighbourDataIntact)
{
    Astra::Registry reg;
    constexpr size_t N = 200;
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ(reg.CreateEntitiesWith<Position>(N, std::span{es}, [](size_t i)
        { return std::tuple{Position{static_cast<float>(i), static_cast<float>(i) + 0.5f, -static_cast<float>(i)}}; }), N);
    std::vector<Astra::Entity> odd;
    for (size_t i = 1; i < N; i += 2)
        odd.push_back(es[i]);
    // Archetype::SetComponents -> ArchetypeChunk::BatchConstructComponent<EnTag>: must not
    // write sizeof(EnTag) bytes through the bit-only column's base.
    reg.AddComponents<EnTag>(std::span<Astra::Entity>(odd.data(), odd.size()), EnTag{});
    for (size_t i = 0; i < N; ++i)
    {
        const Position* p = reg.GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->x, static_cast<float>(i));
        EXPECT_EQ(p->y, static_cast<float>(i) + 0.5f);
        EXPECT_EQ(p->z, -static_cast<float>(i));
        EXPECT_EQ(reg.HasComponent<EnTag>(es[i]), (i % 2) == 1) << i;
        if ((i % 2) == 1)
            EXPECT_TRUE(reg.IsEnabled<EnTag>(es[i])) << i;
    }
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, ViewsFilterDisabledTagsInMixedAndTagOnlyArchetypes)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 10; ++i)
        es.push_back(reg.CreateEntity<Position, EnTag>());
    for (int i : {0, 3, 9})
        reg.SetEnabled<EnTag>(es[i], false);

    size_t n = 0;
    reg.CreateView<Position, EnTag>().ForEach([&](Astra::Entity e, Position&, EnTag&)
    {
        ++n;
        EXPECT_TRUE(reg.IsEnabled<EnTag>(e));
    });
    EXPECT_EQ(n, 7u);
    EXPECT_EQ(reg.CreateView<Position, EnTag>().Size(), 7u);

    size_t all = 0;
    reg.CreateView<Position, Astra::IncludeDisabled<EnTag>>().ForEach([&](Astra::Entity, Position&, EnTag&) { ++all; });
    EXPECT_EQ(all, 10u);

    // Tag-only archetype (zero per-entity size: entities live in the chunk's side vector,
    // capacity chunkBytes/64) -- the bit-only column's words must still be carved.
    std::vector<Astra::Entity> tagOnly;
    for (int i = 0; i < 70; ++i)
        tagOnly.push_back(reg.CreateEntity<EnTag>());
    reg.SetEnabled<EnTag>(tagOnly[64], false);                    // second word
    size_t m = 0;
    reg.CreateView<EnTag>().ForEach([&](Astra::Entity, EnTag&) { ++m; });
    EXPECT_EQ(m, 7u + 69u);                                       // both archetypes, disabled excluded
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BitSurvivesChurnAndDefragment)
{
    Astra::Registry reg;
    constexpr size_t N = 3000;                                    // spans chunks
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ(reg.CreateEntitiesWith<Position, EnTag>(N, std::span{es},
        [](size_t) { return std::tuple{Position{}, EnTag{}}; }), N);
    std::vector<bool> expected(N, true);
    for (size_t i = 0; i < N; i += 3)
    {
        reg.SetEnabled<EnTag>(es[i], false);
        expected[i] = false;
    }
    for (size_t i = 1; i < N; i += 4)
        reg.DestroyEntity(es[i]);                                 // fragment: swap-removes across every chunk
    for (size_t i = 2; i < N; i += 5)
    {
        reg.AddComponent(es[i], Velocity{});                      // MoveAndAdd + MoveEntityFrom round trip
        reg.RemoveComponent<Velocity>(es[i]);
    }

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;                        // CompactChunks on every multi-chunk archetype
    reg.Defragment(opts);

    for (size_t i = 0; i < N; ++i)
    {
        if ((i % 4) == 1)
            continue;                                             // destroyed
        EXPECT_EQ(reg.IsEnabled<EnTag>(es[i]), expected[i]) << i;
    }
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, HasNoValueForChangeDetection)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity<Position, EnTag>();
    EXPECT_FALSE(reg.IsChanged<EnTag>(e, 0));                     // "false for ... a tag" (unchanged contract)
    EXPECT_FALSE(reg.IsAdded<EnTag>(e, 0));
    EXPECT_FALSE(reg.Modified<EnTag>(e));
    EXPECT_FALSE(reg.Modified(e, Astra::TypeID<EnTag>::Value()));
    EXPECT_TRUE(reg.Modified<Position>(e));                       // the data column beside it still answers
}

TEST(EnableableTag, InspectorReadsTheBitOnlyColumnAsZeroBytes)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i)
        reg.CreateEntity<Position, EnTag>();
    const Astra::Debug::InspectorSnapshot snap = Astra::Debug::Capture(reg);
    bool sawBitOnly = false;
    for (const Astra::Debug::ArchetypeInfo& arch : snap.archetypes)
    {
        for (size_t c = 0; c < arch.columns.size(); ++c)
        {
            if (arch.columns[c].stride != 0)
                continue;
            sawBitOnly = true;
            for (const Astra::Debug::ChunkInfo& ch : arch.chunks)
            {
                EXPECT_EQ(ch.columns[c].bytes, 0u);
                EXPECT_NE(ch.columns[c].disabledOffset, SIZE_MAX);   // its words region is reported
            }
        }
        for (const Astra::Debug::ChunkInfo& ch : arch.chunks)
            EXPECT_LE(ch.padBytes, ch.chunkBytes);                // no unsigned underflow in the layout walk
    }
    EXPECT_TRUE(sawBitOnly);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS in `ComponentRegistry.hpp` (`MakeDescriptor<Astra::Test::EnTag>`) with `static_assert failed: 'An enableable component must have storage: an empty/tag type has no column to carry a disabled bit. ...'`.

- [ ] **Step 3: Implement**

**(a) Lift the refusal.** In `include/Astra/Component/ComponentRegistry.hpp`, `MakeDescriptor`, replace

```cpp
            // An enableable component must carry storage: the disabled bit lives
            // per-entity in the chunk's column data, and an empty/tag type has no
            // column (idToColumn == -1) to hang that bit off of. A runtime guard
            // would make such a type silently unfilterable instead of catching the
            // mistake at the call site, so refuse it at compile time.
            static_assert(!(std::is_empty_v<T> && IsEnableableV<T>),
                "An enableable component must have storage: an empty/tag type has no column to carry "
                "a disabled bit. Use a non-empty component, or a separate enableable marker with a byte of state.");
```

with

```cpp
            // An enableable TAG is legal (spec 2026-10-01 s3.2, parity): its archetype gives
            // it a BIT-ONLY column -- stride 0, a disabled-word region, no data region (see
            // Archetype::BuildColumnMeta). A non-enableable tag still gets no column at all.
```

**(b) Type-erased construct/move are no-ops for an empty type.** These mirror the existing `DefaultConstruct`/`Destruct` guards: an enableable tag's column base is a real in-arena address that must never be written. In `include/Astra/Component/Component.hpp`, `struct ComponentDescriptor`, replace `ConstructWith` and `MoveConstruct` with:

```cpp
        inline void ConstructWith(void* ptr, const void* value) const
        {
            if (size == 0)
            {
                return;  // empty (tag) component: nothing to construct (mirrors DefaultConstruct;
                         // an enableable tag's bit-only column base must never be written)
            }
            if (constructWith)
            {
                constructWith(ptr, value);
            }
            else if (copyConstruct)
            {
                // Fallback to copy construct if no specialized constructWith
                copyConstruct(ptr, value);
            }
            else
            {
                // Should not happen for properly registered components
                defaultConstruct(ptr);
            }
        }
```

```cpp
        inline void MoveConstruct(void* dst, void* src) const
        {
            if (is_trivially_copyable)
            {
                std::memcpy(dst, src, size);
            }
            else if (size != 0)   // empty (tag) component: nothing to move (an enableable tag's bit-only column has no data)
            {
                moveConstruct(dst, src);
            }
        }
```

**(c) Column metadata comments.** In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, replace the `ArchetypeColumnMeta` header comment's last two lines

```cpp
    // list) instead of by-value in every chunk slot. Columns are storage-bearing components only
    // (tags excluded), sorted ascending by id so cross-archetype moves can merge-join.
```

with

```cpp
    // list) instead of by-value in every chunk slot. Columns are storage-bearing components plus
    // one BIT-ONLY column (stride 0: disabled words, no data) per ENABLEABLE tag; non-enableable
    // tags have no column. Sorted ascending by id so cross-archetype moves can merge-join.
```

Replace `uint32_t stride{0};                          // == descriptor->size` with `uint32_t stride{0};                          // == descriptor->size (0 for an enableable tag's bit-only column)`. Replace `int16_t   idToColumn[MAX_COMPONENTS];            // id -> column index, or -1 (absent OR tag)` with `int16_t   idToColumn[MAX_COMPONENTS];            // id -> column index, or -1 (absent OR non-enableable tag)`.

**(d) `BuildColumnMeta`.** In `include/Astra/Archetype/Archetype.hpp`, change the first line of its doc comment from `// Builds m_columnMeta from m_componentDescriptors: excludes tags (desc.size == 0) from` to `// Builds m_columnMeta from m_componentDescriptors: excludes NON-enableable tags (desc.size == 0) from`. Then replace

```cpp
                if (desc.size == 0) continue;       // tag: no storage column (idToColumn stays -1)
```

with

```cpp
                // A non-enableable tag has no column (idToColumn stays -1). An ENABLEABLE tag
                // gets a BIT-ONLY column (spec 2026-10-01 s3.2): stride 0, no data region, a
                // disabled-word region like any enableable column -- so SetEnabledByID, every
                // view filter and every bit carry work unchanged through idToColumn.
                if (desc.size == 0 && !desc.isEnableable) continue;
```

**(e) Capacity math.** In `Archetype::ComputeLayoutBytesForCapacity`, replace the column loop

```cpp
            for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
            {
                offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                offset += static_cast<size_t>(m_columnMeta.columns[c].stride) * cap;
            }
```

with

```cpp
            for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
            {
                if (m_columnMeta.columns[c].stride == 0)
                    continue;   // bit-only column (enableable tag): no data, no alignment -- mirrors InitializeColumns
                offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                offset += static_cast<size_t>(m_columnMeta.columns[c].stride) * cap;
            }
```

Its version-region line (`offset += static_cast<size_t>(m_columnMeta.columnCount) * sizeof(Tick);`) already counts the bit-only column's Tick. Its word loop already counts the bit-only column's words, because the column is in `enableableColumns`. `ChunkBytesToHold` counts it through `enableableColumnCount` too. `m_perEntitySize` stays the sum of non-empty sizes, so a tag-only archetype keeps the zero-size side-vector capacity (`chunkBytes >> 6`). That chunk's carve is at most `columnCount * 4 + 8 + words * 8` bytes, which always fits.

In `Archetype::Initialize`, replace

```cpp
            // Change-detection version region (ArchetypeChunk::InitializeColumns):
            // 8-byte align slack + one Tick per storage column. Folded into the
            // overhead so ComputeCapacityForBytes' estimate stays a guaranteed fit.
            alignmentOverhead += 8 + nonEmptyComponents * sizeof(Tick);
```

with

```cpp
            // Change-detection version region (ArchetypeChunk::InitializeColumns):
            // 8-byte align slack + one Tick per COLUMN -- an enableable tag's bit-only
            // column included (columnCount equals nonEmptyComponents unless one is
            // present, so every other archetype's layout is byte-identical). Folded into
            // the overhead so ComputeCapacityForBytes' estimate stays a guaranteed fit.
            alignmentOverhead += 8 + static_cast<size_t>(m_columnMeta.columnCount) * sizeof(Tick);
```

**(f) The carve.** In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, `ArchetypeChunk::InitializeColumns`, replace the comment and the first loop

```cpp
        // Assign each storage column a cache-line-aligned base within the chunk arena.
        // Column physical order follows m_meta->columns (ascending by id); tags carry
        // no column, so every column here has real storage.
        void InitializeColumns()
        {
            size_t offset = 0;
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                // Align offset to a cache line to avoid false sharing between columns.
                offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                m_columns[c].base   = static_cast<std::byte*>(m_memory) + offset;
                m_columns[c].stride = m_meta->columns[c].stride;
                offset += static_cast<size_t>(m_meta->columns[c].stride) * m_capacity;
            }
```

with

```cpp
        // Assign each storage column a cache-line-aligned base within the chunk arena.
        // Column physical order follows m_meta->columns (ascending by id). Non-enableable
        // tags carry no column; an enableable tag's column is BIT-ONLY (stride 0).
        void InitializeColumns()
        {
            size_t offset = 0;
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                m_columns[c].stride = m_meta->columns[c].stride;
                if (m_columns[c].stride == 0)
                {
                    // Bit-only column (enableable tag, spec 2026-10-01 s3.2): no data region and
                    // no alignment. The base is a NON-NULL in-arena address at the running offset,
                    // so the type-erased move/copy paths (memcpy of 0 bytes) never see nullptr.
                    // It is never dereferenced: desc.size == 0 makes every construct/move/destruct
                    // a no-op, and the Inspector's layout walk reads it as a zero-byte column.
                    m_columns[c].base = static_cast<std::byte*>(m_memory) + offset;
                    continue;
                }
                // Align offset to a cache line to avoid false sharing between columns.
                offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                m_columns[c].base = static_cast<std::byte*>(m_memory) + offset;
                offset += static_cast<size_t>(m_meta->columns[c].stride) * m_capacity;
            }
```

The version-region, disabled-word and tick loops after it are unchanged. The bit-only column is in `enableableColumns`, so it gets its words.

**(g) Typed tag access never hands out the bit-only base.** In the same file, replace `ArchetypeChunk::GetComponent<T>`:

```cpp
        template<Component T>
        T* GetComponent(size_t index)
        {
            ASTRA_ASSERT(index < m_count, "Index out of count");
            // Empty (tag) component: no data, so a shared static instance -- also for an
            // enableable tag, whose bit-only column has a (never-dereferenced) base. Callers
            // resolve presence from the archetype mask first (Archetype::GetComponent).
            if constexpr (std::is_empty_v<T>)
            {
                static T emptyInstance{};
                return &emptyInstance;
            }
            else
            {
                return static_cast<T*>(GetComponentPointer(TypeID<T>::Value(), index));
            }
        }
```

**(h) Batch construct.** In `ArchetypeChunk::BatchConstructComponent<T>`, insert as the first statement of the body, before `ComponentID id = TypeID<T>::Value();`:

```cpp
            // Empty (tag) component: nothing to construct. Must precede the column lookup: an
            // enableable tag HAS a (bit-only, stride-0) column, and the trivially-copyable path
            // below would memcpy sizeof(T) == 1 byte through its base.
            if constexpr (std::is_empty_v<T>)
                return;
```

**(i) `MoveAndAdd<T>`.** In `include/Astra/Archetype/ArchetypeManager.hpp`, replace

```cpp
                if (id == newComponentId) ASTRA_UNLIKELY
                {
                    new (dstPtr) T(std::forward<Args>(args)...);
                }
```

with

```cpp
                if (id == newComponentId) ASTRA_UNLIKELY
                {
                    // An enableable tag's bit-only column (stride 0) has no data: never
                    // placement-new into its base. Born enabled (fresh slot bits are zero).
                    if constexpr (std::is_empty_v<T>)
                        ((void)args, ...);
                    else
                        new (dstPtr) T(std::forward<Args>(args)...);
                }
```

**(j) `MoveAndAddByID`.** Replace

```cpp
                    // Copy new component data - use memcpy for trivially copyable types
                    if (newDesc.is_trivially_copyable)
                    {
                        std::memcpy(dstPtr, componentData, newDesc.size);
                    }
```

with

```cpp
                    // Copy new component data - use memcpy for trivially copyable types. An
                    // enableable tag's bit-only column (size 0) has no data; the by-id add
                    // path may pass componentData == nullptr for a tag, so write nothing.
                    if (newDesc.size == 0) ASTRA_UNLIKELY
                    {
                    }
                    else if (newDesc.is_trivially_copyable)
                    {
                        std::memcpy(dstPtr, componentData, newDesc.size);
                    }
```

**(k) `MarkWritten`: a tag has no value to write.** Replace

```cpp
            const int col = rec->archetype->GetColumnMeta().idToColumn[id];
            if (col < 0) ASTRA_UNLIKELY
                return false;
            rec->chunk->StampColumn(col, m_tick);
```

with

```cpp
            const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
            const int col = cm.idToColumn[id];
            if (col < 0 || cm.columns[col].stride == 0) ASTRA_UNLIKELY
                return false;   // absent, or a tag (an enableable tag's bit-only column holds no value)
            rec->chunk->StampColumn(col, m_tick);
```

**(l) `IsChanged` / `IsAdded`.** In `include/Astra/Registry/Registry.hpp`, replace BOTH occurrences of

```cpp
            if (col < 0) return false;
```

(the line after `const int col = rec->archetype->GetColumnMeta().idToColumn[id];` in `IsChanged` and in `IsAdded`) with

```cpp
            if (col < 0 || rec->archetype->GetColumnMeta().columns[col].stride == 0) return false;   // absent, or a tag (no value)
```

**(m) By-ID tag payload.** In `SetEnabledByID`, replace

```cpp
                    void* comp = chunk->GetComponentPointer(componentId, idx);
```

with

```cpp
                    // A present tag's payload is EmptyComponentSentinel() on every by-ID path
                    // (Component.hpp); an enableable tag's bit-only column base is never handed out.
                    void* comp = rec->archetype->GetColumnMeta().columns[col].stride == 0
                        ? EmptyComponentSentinel()
                        : chunk->GetComponentPointer(componentId, idx);
```

- [ ] **Step 4: Run the tests**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=EnableableTag.*:EmptyTag.*:RegistryTest.*Enable*:RegistryTest.*Disabled*:ViewTest.*:InspectorTest*:ChunkPool*:ArchetypeTest.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- 9 `EnableableTag` tests PASS;
- the existing tag, enableable, view, inspector, chunk-pool and archetype suites are unchanged and green. Their archetypes hold no enableable tag, so `columnCount == nonEmptyComponents` and every layout is byte-identical;
- the full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Component/ComponentRegistry.hpp include/Astra/Component/Component.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp tests/TestComponents.hpp tests/Registry/EnableableTagTest.cpp
git commit -m @'
feat(storage): enableable Table tags through a bit-only column (stride 0, disabled words, no data) -- the tag/enableable static_assert is gone; enable/disable, view filtering, swap-remove/transition/compaction carries and the Inspector walk all hold (spec s3.2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

### Task 7: Serialize enableable tags (zero data bytes + the disabled section)

Spec s3.2, second bullet (`SerializeColumn` "writes zero data bytes plus the existing disabled section"), plus the IM-9 rule: section presence is a property of the ARCHIVE. After Task 6, the writer would emit per-element `serializeVersioned` bytes for the tag's bit-only column. Also, the reader decides "a column block follows" from THIS build's column, so it desyncs on an archive whose writer disagreed about a tag's enableability.

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp`:
  - `:959-963`: the `Serialize` column loop;
  - `:1260-1266`: the `Deserialize` column loop;
  - `:1530-1606`: `SerializeColumn`;
  - `:1608-1660`: `DeserializeColumn`.

  Anchors are at `975cdb7`; locate them by content.
- Test: `tests/Registry/EnableableTagTest.cpp` (append)

**Interfaces:**
- Consumes: Task 6 (bit-only columns, `EnTag`).
- Produces:
  - **Writer.** A column block is written for every descriptor with `idToColumn[id] >= 0`: every data component plus every enableable tag. For a tag (`desc.size == 0`) the block is ZERO data bytes followed by `uint32 disabledCount + ceil(max(1,n)/64) uint64 words`. With LZ4, the block is its own compressed block (`WriteCompressedBlock`), as for any column. A non-enableable tag writes nothing, as today, so tag-free and plain-tag archives are byte-identical to before.
  - **Reader.** A column block is consumed for descriptor `di` iff `desc.size != 0 || diskHasDisabledSection[di]`. That condition is a property of the archive, not of this build.
    - A section for a tag this build does not mark enableable is consumed, validated and dropped.
    - An archive whose writer had a plain tag (flag 0) loads with every entity enabled.
  - The header stays `BINARY_FORMAT_VERSION` 5. Chunk E bumps it to 6.

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/EnableableTagTest.cpp`:

```cpp
// ---------------- Serialization (spec s3.2: zero data bytes + the disabled section) ----------------

TEST(EnableableTagSerialization, RoundTripsInLz4AndNoneIncludingTagOnlyArchetypes)
{
    for (const Astra::CompressionMode mode : {Astra::CompressionMode::LZ4, Astra::CompressionMode::None})
    {
        SCOPED_TRACE(mode == Astra::CompressionMode::LZ4 ? "LZ4" : "None");
        Astra::Registry reg;
        constexpr size_t N = 3000;                                // spans chunks
        std::vector<Astra::Entity> es(N);
        ASSERT_EQ(reg.CreateEntitiesWith<Position, EnTag>(N, std::span{es},
            [](size_t i) { return std::tuple{Position{static_cast<float>(i), 0.0f, 0.0f}, EnTag{}}; }), N);
        std::vector<bool> expected(N, true);
        for (size_t i = 0; i < N; i += 3)
        {
            reg.SetEnabled<EnTag>(es[i], false);
            expected[i] = false;
        }
        std::vector<Astra::Entity> tagOnly;
        for (int i = 0; i < 70; ++i)
            tagOnly.push_back(reg.CreateEntity<EnTag>());
        reg.SetEnabled<EnTag>(tagOnly[5], false);
        reg.SetEnabled<EnTag>(tagOnly[64], false);

        Astra::Registry::SaveConfig cfg;
        cfg.compressionMode = mode;
        auto saved = reg.Save(cfg);
        ASSERT_TRUE(saved.IsOk());

        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position, EnTag>();
        auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
        ASSERT_TRUE(loaded.IsOk());
        Astra::Registry& out = **loaded.GetValue();

        for (size_t i = 0; i < N; ++i)
        {
            ASSERT_TRUE(out.HasComponent<EnTag>(es[i])) << i;
            EXPECT_EQ(out.IsEnabled<EnTag>(es[i]), expected[i]) << i;
            const Position* p = out.GetComponent<Position>(es[i]);
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(p->x, static_cast<float>(i));
        }
        for (int i = 0; i < 70; ++i)
            EXPECT_EQ(out.IsEnabled<EnTag>(tagOnly[i]), i != 5 && i != 64) << i;
        ExpectBitInvariant(out);
    }
}

TEST(EnableableTagSerialization, BitOnlyColumnWritesZeroDataBytes)
{
    // Two registries with the same shape, one with the enableable tag and one with a plain
    // tag. Both archives match except the tag's column block. The plain tag writes nothing.
    // EnTag writes ZERO data bytes, then disabledCount (4) + ceil(100/64) == 2 words (16).
    // 100 entities fit one 4 KB chunk in both layouts.
    Astra::Registry::SaveConfig cfg;
    cfg.compressionMode = Astra::CompressionMode::None;
    Astra::Registry withEnTag;
    Astra::Registry withPlayer;
    for (int i = 0; i < 100; ++i)
    {
        withEnTag.CreateEntity<Position, EnTag>();
        withPlayer.CreateEntity<Position, Player>();
    }
    auto a = withEnTag.Save(cfg);
    auto b = withPlayer.Save(cfg);
    ASSERT_TRUE(a.IsOk());
    ASSERT_TRUE(b.IsOk());
    EXPECT_EQ(a.GetValue()->size(), b.GetValue()->size() + sizeof(uint32_t) + 2 * sizeof(uint64_t));
}

namespace
{
    // Hand-built single-archetype {EnTag, Position} archive at format v5, following
    // Archetype::Serialize's field order (see BinarySerializationTests' legacy-format
    // tests). Version 5 is HARDCODED: it is the last format before the storage-policies
    // bump to 6, and v5 archives stay loadable. Five entities in one chunk, EnTag first
    // in descriptor order.
    //   withSection == false: a writer whose EnTag was a PLAIN tag. Flag 0, no column block.
    //   withSection == true:  a writer that stored EnTag as a bit-only column. Flag 1, zero
    //                         data bytes, then disabledCount 2 + one word with slots 1 and 3 set.
    constexpr uint32_t kTagArchiveCount = 5;

    void BuildTagArchive(std::vector<std::byte>& buf, const Astra::ComponentDescriptor& tag,
                         const Astra::ComponentDescriptor& pos, bool withSection)
    {
        Astra::BinaryHeader header;                               // magic/endianness, compression None
        header.version = 5;
        const auto* raw = reinterpret_cast<const std::byte*>(&header);
        buf.insert(buf.end(), raw, raw + sizeof(header));
        {
            Astra::BinaryWriter writer(buf);                      // memory mode appends after the header
            Astra::ComponentMask mask;
            mask.Set(tag.id);
            mask.Set(pos.id);
            for (size_t i = 0; i < Astra::ComponentMask::WORD_COUNT; ++i)
                writer(mask.Data()[i]);
            writer(static_cast<uint64_t>(kTagArchiveCount));      // archetype entityCount
            writer(static_cast<uint64_t>(8));                     // entitiesPerChunk bound
            writer(static_cast<uint32_t>(1));                     // chunkCount
            writer(static_cast<uint32_t>(2));                     // descriptorCount
            for (const Astra::ComponentDescriptor* d : {&tag, &pos})
            {
                writer(d->hash);
                writer(static_cast<uint64_t>(d->size));
                writer(static_cast<uint64_t>(d->alignment));
                writer(d->version);
                writer(static_cast<uint8_t>((d == &tag && withSection) ? 1 : 0));   // IM-9 has-section flag
            }
            writer(kTagArchiveCount);                             // chunk 0 entity count
            for (uint32_t i = 0; i < kTagArchiveCount; ++i)
                writer(Astra::Entity(i + 1, 1));
            if (withSection)                                      // EnTag block: no data, then the section
            {
                writer(static_cast<uint32_t>(2));
                writer(static_cast<uint64_t>((1ull << 1) | (1ull << 3)));
            }
            for (uint32_t i = 0; i < kTagArchiveCount; ++i)       // Position block
            {
                Position p{static_cast<float>(i), 1.0f, 2.0f};
                writer.WriteVersionedComponent(p);
            }
        }
    }
}

TEST(EnableableTagSerialization, ArchiveFromAPlainTagWriterLoadsAllEnabled)
{
    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<EnTag, Position>();
    const Astra::ComponentDescriptor* tag = cr->GetComponentDescriptor(Astra::TypeID<EnTag>::Value());
    const Astra::ComponentDescriptor* pos = cr->GetComponentDescriptor(Astra::TypeID<Position>::Value());
    ASSERT_NE(tag, nullptr);
    ASSERT_NE(pos, nullptr);
    std::vector<Astra::ComponentDescriptor> descriptors;
    cr->GetAllDescriptors(descriptors);

    std::vector<std::byte> buf;
    BuildTagArchive(buf, *tag, *pos, /*withSection=*/false);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    ASSERT_TRUE(reader.ReadHeader().IsOk());
    Astra::ArchetypeChunkPool pool;
    auto result = Astra::Archetype::Deserialize(reader, descriptors, &pool);
    ASSERT_TRUE(result.IsOk()) << "flag 0 => no EnTag block in the stream; the reader must not expect one";
    auto archetype = std::move(*result.GetValue());

    const int col = archetype->GetColumnMeta().idToColumn[tag->id];
    ASSERT_GE(col, 0);                                            // THIS build stores EnTag as a bit-only column
    Astra::ArchetypeChunk& chunk = *archetype->GetChunks()[0];
    EXPECT_EQ(chunk.GetDisabledCount(col), 0u);
    for (uint32_t i = 0; i < kTagArchiveCount; ++i)
    {
        EXPECT_FALSE(chunk.IsDisabled(col, i)) << i;
        EXPECT_EQ(chunk.GetComponent<Position>(i)->x, static_cast<float>(i)) << "stream desync at " << i;
    }
}

TEST(EnableableTagSerialization, BitOnlySectionAppliesWhenThisBuildHasTheColumn)
{
    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<EnTag, Position>();
    const Astra::ComponentDescriptor* tag = cr->GetComponentDescriptor(Astra::TypeID<EnTag>::Value());
    const Astra::ComponentDescriptor* pos = cr->GetComponentDescriptor(Astra::TypeID<Position>::Value());
    ASSERT_NE(tag, nullptr);
    ASSERT_NE(pos, nullptr);
    std::vector<Astra::ComponentDescriptor> descriptors;
    cr->GetAllDescriptors(descriptors);

    std::vector<std::byte> buf;
    BuildTagArchive(buf, *tag, *pos, /*withSection=*/true);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    ASSERT_TRUE(reader.ReadHeader().IsOk());
    Astra::ArchetypeChunkPool pool;
    auto result = Astra::Archetype::Deserialize(reader, descriptors, &pool);
    ASSERT_TRUE(result.IsOk());
    auto archetype = std::move(*result.GetValue());

    const int col = archetype->GetColumnMeta().idToColumn[tag->id];
    ASSERT_GE(col, 0);
    Astra::ArchetypeChunk& chunk = *archetype->GetChunks()[0];
    EXPECT_EQ(chunk.GetDisabledCount(col), 2u);
    for (uint32_t i = 0; i < kTagArchiveCount; ++i)
    {
        EXPECT_EQ(chunk.IsDisabled(col, i), i == 1 || i == 3) << i;
        EXPECT_EQ(chunk.GetComponent<Position>(i)->x, static_cast<float>(i)) << "stream desync at " << i;
    }
}

TEST(EnableableTagSerialization, BitOnlySectionIsConsumedWhenThisBuildHasNoColumn)
{
    // The archive's writer stored EnTag as a bit-only column (flag 1). Simulate a loading
    // build where EnTag is a PLAIN tag by clearing isEnableable on the descriptor copy
    // handed to Deserialize. The section must be consumed (stream stays in sync) and dropped.
    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<EnTag, Position>();
    const Astra::ComponentDescriptor* tag = cr->GetComponentDescriptor(Astra::TypeID<EnTag>::Value());
    const Astra::ComponentDescriptor* pos = cr->GetComponentDescriptor(Astra::TypeID<Position>::Value());
    ASSERT_NE(tag, nullptr);
    ASSERT_NE(pos, nullptr);
    std::vector<Astra::ComponentDescriptor> descriptors;
    cr->GetAllDescriptors(descriptors);
    for (Astra::ComponentDescriptor& d : descriptors)
        if (d.id == tag->id)
            d.isEnableable = false;

    std::vector<std::byte> buf;
    BuildTagArchive(buf, *tag, *pos, /*withSection=*/true);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    ASSERT_TRUE(reader.ReadHeader().IsOk());
    Astra::ArchetypeChunkPool pool;
    auto result = Astra::Archetype::Deserialize(reader, descriptors, &pool);
    ASSERT_TRUE(result.IsOk()) << "flag 1 => the section must be consumed even with no local column";
    auto archetype = std::move(*result.GetValue());

    EXPECT_EQ(archetype->GetColumnMeta().idToColumn[tag->id], -1);   // plain tag here: no column
    Astra::ArchetypeChunk& chunk = *archetype->GetChunks()[0];
    for (uint32_t i = 0; i < kTagArchiveCount; ++i)
        EXPECT_EQ(chunk.GetComponent<Position>(i)->x, static_cast<float>(i)) << "stream desync at " << i;
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=EnableableTagSerialization.*
```

Expected: FAIL.
- `BitOnlyColumnWritesZeroDataBytes`: the size differs by more than 20 bytes, because the writer emits per-element `serializeVersioned` data for the tag.
- `ArchiveFromAPlainTagWriterLoadsAllEnabled` and `BitOnlySectionAppliesWhenThisBuildHasTheColumn`: the reader tries to read per-element tag data out of the stream, so `Deserialize` returns `Err` or the `Position` values are garbage.
- `BitOnlySectionIsConsumedWhenThisBuildHasNoColumn`: with no local column, the reader skips the section and reads it as `Position` data.
- `RoundTripsInLz4AndNoneIncludingTagOnlyArchetypes` may already pass: writer and reader are symmetric after Task 6. It is the regression net for this task.

- [ ] **Step 3: Implement**

All edits are in `include/Astra/Archetype/Archetype.hpp`.

**(a) Writer column loop** (`Archetype::Serialize`). Replace

```cpp
                // Write component arrays (SOA layout)
                for (const auto& desc : m_componentDescriptors)
                {
                    void* componentArray = chunk->GetComponentArrayByID(desc.id);
                    if (!componentArray) continue;

                    // (componentArray null / tag columns already skipped by the
                    //  `if (!componentArray) continue;` above)
                    if (writer.GetCompressionMode() == CompressionMode::LZ4)
```

with

```cpp
                // Write component arrays (SOA layout). One block per COLUMN: every data
                // component, and every enableable tag's bit-only column (zero data bytes +
                // its disabled section, spec 2026-10-01 s3.2). A non-enableable tag has no
                // column and writes nothing -- byte-identical to the earlier format.
                for (const auto& desc : m_componentDescriptors)
                {
                    if (m_columnMeta.idToColumn[desc.id] < 0) continue;

                    if (writer.GetCompressionMode() == CompressionMode::LZ4)
```

**(b) `SerializeColumn`.** Replace the last sentences of its doc comment

```cpp
        // compressed-column path can reuse it. Caller guarantees
        // chunk->GetComponentArrayByID(desc.id) is non-null (null/tag columns
        // are skipped before calling).
        void SerializeColumn(BinaryWriter& w, ArchetypeChunk* chunk, const ComponentDescriptor& desc, size_t chunkEntityCount) const
```

with

```cpp
        // compressed-column path can reuse it. Caller guarantees desc has a column
        // here (idToColumn >= 0): a data column, or an enableable tag's bit-only
        // column, which writes ZERO data bytes and then its disabled section.
        void SerializeColumn(BinaryWriter& w, ArchetypeChunk* chunk, const ComponentDescriptor& desc, size_t chunkEntityCount) const
```

In its body, replace

```cpp
            if (desc.serializeVersioned || desc.serialize)
            {
                // For custom serialization, we can't compress the whole array
```

with

```cpp
            // desc.size == 0: an enableable tag's bit-only column -- no data, zero bytes.
            if (desc.size != 0 && (desc.serializeVersioned || desc.serialize))
            {
                // For custom serialization, we can't compress the whole array
```

Then replace the comment inside `if (desc.isEnableable)`

```cpp
                // Tag components (desc.size == 0) never reach here -- their
                // componentArray is null and the caller's null-skip already
                // skipped them -- so this ordinal is always a real storage column.
```

with

```cpp
                // An enableable tag reaches here through its bit-only column
                // (zero data bytes above); every admitted desc has idToColumn >= 0.
```

**(c) Reader column loop** (`Archetype::Deserialize`). Replace

```cpp
                for (size_t di = 0; di < descriptors.size(); ++di)
                {
                    const ComponentDescriptor& desc = descriptors[di];
                    void* componentArray = chunk->GetComponentArrayByID(desc.id);
                    if (!componentArray) continue;
```

with

```cpp
                for (size_t di = 0; di < descriptors.size(); ++di)
                {
                    const ComponentDescriptor& desc = descriptors[di];
                    // A block follows iff the WRITER had a column: every data component, and a
                    // tag only when the writer stored it as an enableable bit-only column --
                    // which the archive records as the has-disabled-section flag (IM-9). Keyed
                    // on the ARCHIVE, never on this build's column, so a tag whose
                    // enableability differs between builds keeps the stream in sync.
                    const bool archiveHasColumn = desc.size != 0 || diskHasDisabledSection[di] != 0;
                    if (!archiveHasColumn) continue;
```

**(d) `DeserializeColumn`.** Replace the last sentences of its doc comment

```cpp
        // compressed-column path can reuse it. Caller guarantees
        // chunk->GetComponentArrayByID(desc.id) is non-null (null/tag columns
        // are skipped before calling); hasDisabledSection is the per-descriptor
```

with

```cpp
        // compressed-column path can reuse it. Called iff the ARCHIVE holds a block
        // for desc (desc.size != 0 || hasDisabledSection). For a tag that block is zero
        // data bytes plus the section; when THIS build has no column for the tag (not
        // enableable here) the section is consumed, validated and dropped below.
        // hasDisabledSection is the per-descriptor
```

In its body, replace

```cpp
            if (desc.deserializeVersioned || desc.deserialize)
            {
                // For custom deserialization, components are not compressed
```

with

```cpp
            // desc.size == 0: a tag's block carries no data (bit-only column) -- read nothing.
            if (desc.size != 0 && (desc.deserializeVersioned || desc.deserialize))
            {
                // For custom deserialization, components are not compressed
```

The section-consumption code after it is unchanged. It reads iff `hasDisabledSection`, and applies the bits iff `desc.isEnableable`. An enableable descriptor always has a column in this build after Task 6, so `idToColumn[desc.id] >= 0` holds there.

- [ ] **Step 4: Run the tests**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=EnableableTagSerialization.*:EnableableTag.*:BinarySerializationTests.*:RegistrySerialization*:FormatV2*:LoadRobustness*:BinarySerializationVersioning*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- 5 `EnableableTagSerialization` tests PASS;
- every existing serialization suite is unchanged and green. They carry no enableable tag, so their archives are byte-identical;
- the full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Archetype/Archetype.hpp tests/Registry/EnableableTagTest.cpp
git commit -m @'
feat(storage): serialize enableable Table tags -- zero data bytes plus the disabled section; the reader keys block presence on the archive (size or IM-9 flag), so plain-tag and bit-only-tag archives load in either build (spec s3.2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---


## Chunk B: stores + creation (Tasks 8-14)

**Covers:** spec s4.1 and s4.2 (the two stores), s7.1 (the creation funnel and the CommandBuffer reroute), and s3.3 for Universal: Get/Has routing, Add as a no-op, the Remove refusal, enable, and change ticks. Sparse Get/Has routing lands in Task 11 because creation is the first path that puts values into a sparse set.

**What this chunk takes from Chunk A (Tasks 1-7)**, by the canonical names:
- `Storage`, `StorageOfV<T>`, `StorageKind`, `ComponentDescriptor::storage`, and the `ASTRA_SPARSE_PAGE_SIZE` / `ASTRA_UNIVERSAL_PAGE_SLOTS` macros.
- `Registry::Config::AddUniversal<T>(init, user)`. `UniversalSpec` and `UniversalInitFn` live at NAMESPACE scope in `Registry/ComponentStorage.hpp`, and `Registry::Config` aliases them. `UniversalSpec` has a 4th field, `registerComponent`. This chunk reads only `id`, `init` and `user`.
- `ComponentStorage` with `KindOf`, `SparseSlotOf`, `AcquireSparseSlot`, `FindUniversalSpec`, `UniversalSpecs()`, `UsedSparseSlotCount()`, and a `Clear()` that is a no-op until this chunk fills it.
- Two `ComponentStorage` members, by Chunk A's (Task 4) names: `m_registry` (the `std::shared_ptr<ComponentRegistry>`) and `m_universal` (the stored, deduplicated `std::vector<UniversalSpec>`). Chunk A's slot table is `m_slotOf` / `m_componentOfSlot` / `m_usedSlots`. (`m_componentRegistry` is the `Registry`'s own member, not `ComponentStorage`'s.)
- The `Registry` member `std::shared_ptr<ComponentStorage> m_componentStorage`. It is built in all three constructors, cleared in place by `Registry::Clear()`, rebuilt from the caller's `Config` in `LoadInternal`, and exposed through `GetComponentStorage()`.

**Chunk C extends or replaces three of this chunk's arms.** They are deliberately minimal here, and each Chunk C task names exactly what it replaces:
- **Sparse change ticks:** Task 14's `MarkStoreWritten`/`StoreTickNewer` carry a `default: return false;` where the Sparse arm goes. Task 20 REPLACES each `default:` with a `case StorageKind::Sparse:` (the typed and by-id entry points already route Sparse through these helpers, via Task 11's `RoutesToStore`).
- **The universal enable mirror:** Task 13's `IsUniversalId` arms in `SetEnabledByID`/`IsEnabledByID` and its `SetEnabledUniversal` helper update only the column flag. Task 19 DELETES all three and routes every non-Table id through `SetEnabledInStore`, which also maintains the chunk `disabled` mirror (s5.1).
- **The destroy walk:** Task 9 wires `OnEntityDestroyed` into `DestroyEntity`/`DestroyEntities` and Task 11 adds an interim O(sets) sparse-erase loop inside it. Task 21 REPLACES that body with the presence-block walk and REPLACES Task 9's two call sites (adding the sparse structural-counter bump).

**What this chunk adds beyond the canonical interfaces.** These additions are listed in the chunk notes:
- **Both stores:** `SetTickSource`, `MarkWritten`, `AddedTick`, `ChangedTick` and `PageCount`.
- **`ComponentStorage`:** `SetTickSource`, `HasUniversals`, `HasAnyStore`, `FindUniversal` and `FindSparse`.
- **`ArchetypeManager`:** `TickSource()`.
- **The header `Registry/StorageSplit.hpp`:** compile-time helpers that split a component pack by its storage trait.

---

### Task 8: `UniversalColumn` -- the paged, slot-indexed universal store

Spec s4.2. A standalone store, unit-tested through `ComponentRegistry::MakeDescriptor<T>` (a public static that mints no new `ComponentID`). Nothing in the registry uses it yet.

**Files:**
- Create: `include/Astra/Registry/UniversalColumn.hpp`
- Create: `tests/Registry/UniversalColumnTest.cpp`

**Interfaces:**
- Consumes: `ComponentDescriptor` (`Component/Component.hpp`), `EmptyComponentSentinel()`, `ASTRA_UNIVERSAL_PAGE_SLOTS` (Chunk A), `Tick` / `EntityTicks` (`Core/Tick.hpp`), `Entity`.
- Produces (canonical, plus the additions marked `+`):
  ```cpp
  class UniversalColumn {
  public:
    static constexpr std::size_t PAGE_SLOTS = ASTRA_UNIVERSAL_PAGE_SLOTS;   // 1024
    explicit UniversalColumn(const ComponentDescriptor& desc);
    void*        Construct(Entity e);   void* ConstructMove(Entity e, void* src);
    void         Destroy(Entity e);                     // no-op for an absent or stale handle
    void*        Get(Entity e);         const void* Get(Entity e) const;   // nullptr unless live with matching version; a tag yields EmptyComponentSentinel()
    bool         IsLive(Entity e) const;
    bool         IsDisabled(Entity e) const;  bool SetDisabled(Entity e, bool disabled);  // true on change
    EntityTicks* Ticks(Entity e);                       // nullptr unless live and change-tracked
    size_t       LiveCount() const;
    template<class Fn> void ForEachLive(Fn&& fn) const; // fn(Entity, const void*), ascending index
    void         Clear();
    const ComponentDescriptor& Descriptor() const;
    + void   SetTickSource(const Tick* tick) noexcept;   // ArchetypeManager::TickSource()
    + bool   MarkWritten(Entity e, Tick now) noexcept;   // false: absent, stale or tag
    + Tick   AddedTick(Entity e) const noexcept;         // tracked: per slot; untracked: page version; 0 if not live
    + Tick   ChangedTick(Entity e) const noexcept;
    + size_t PageCount() const noexcept;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/UniversalColumnTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <vector>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/UniversalColumn.hpp>
#include "../TestComponents.hpp"

using Astra::Entity;
using Astra::Tick;
using Astra::UniversalColumn;
using Astra::Test::Position;

namespace
{
    // The registry's own descriptor factory, without registering anything: MakeDescriptor
    // reads TypeID<T>::Hash() (compile-time), never TypeID<T>::Value(), so these unit
    // tests mint no ComponentID. The id argument is irrelevant to the store.
    template<typename T>
    Astra::ComponentDescriptor Desc()
    {
        return Astra::ComponentRegistry::MakeDescriptor<T>(0, nullptr);
    }
}

TEST(UniversalColumn, ConstructGetDestroyRoundTrip)
{
    UniversalColumn col(Desc<Position>());
    const Entity e(5, 1);
    EXPECT_EQ(col.Get(e), nullptr);
    EXPECT_FALSE(col.IsLive(e));

    auto* p = static_cast<Position*>(col.Construct(e));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, Position{});                 // DefaultConstruct: value-initialised
    p->x = 3.0f;
    EXPECT_EQ(col.Get(e), p);
    EXPECT_TRUE(col.IsLive(e));
    EXPECT_EQ(col.LiveCount(), 1u);
    EXPECT_EQ(col.PageCount(), 1u);

    col.Destroy(e);
    EXPECT_EQ(col.Get(e), nullptr);
    EXPECT_EQ(col.LiveCount(), 0u);
    EXPECT_EQ(col.PageCount(), 0u);            // the page's last live slot went: page released
}

TEST(UniversalColumn, ConstructMoveAndClearRunEveryLifetimeHook)
{
    using Astra::Test::Tracked;
    const int base = Tracked::s_live;
    {
        UniversalColumn col(Desc<Tracked>());
        Tracked src(41);
        auto* t = static_cast<Tracked*>(col.ConstructMove(Entity(2, 1), &src));
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->value, 41);
        (void)col.Construct(Entity(3, 1));
        (void)col.Construct(Entity(1500, 1));  // a second page
        EXPECT_EQ(Tracked::s_live, base + 4);  // src + three stored
        EXPECT_EQ(col.PageCount(), 2u);

        col.Clear();
        EXPECT_EQ(Tracked::s_live, base + 1);  // only src is left
        EXPECT_EQ(col.LiveCount(), 0u);
        EXPECT_EQ(col.PageCount(), 0u);

        (void)col.Construct(Entity(9, 1));     // the column is reusable after Clear
        EXPECT_EQ(Tracked::s_live, base + 2);
    }                                          // ~UniversalColumn destroys what is left
    EXPECT_EQ(Tracked::s_live, base);
}

TEST(UniversalColumn, ForEachLiveWalksAscendingIndexAcrossPages)
{
    UniversalColumn col(Desc<Position>());
    const std::vector<Entity> ents{Entity(2100, 1), Entity(7, 3), Entity(1024, 1),
                                   Entity(0, 1), Entity(63, 2), Entity(64, 1)};
    for (Entity e : ents)
        static_cast<Position*>(col.Construct(e))->x = static_cast<float>(e.GetID());

    std::vector<Entity> seen;
    col.ForEachLive([&](Entity e, const void* p)
    {
        EXPECT_FLOAT_EQ(static_cast<const Position*>(p)->x, static_cast<float>(e.GetID()));
        seen.push_back(e);
    });
    const std::vector<Entity> expected{Entity(0, 1), Entity(7, 3), Entity(63, 2),
                                       Entity(64, 1), Entity(1024, 1), Entity(2100, 1)};
    EXPECT_EQ(seen, expected);
}

TEST(UniversalColumn, TagHasPresenceButNoData)
{
    UniversalColumn col(Desc<Astra::Test::Player>());
    const Entity e(4, 1);
    EXPECT_EQ(col.Construct(e), Astra::EmptyComponentSentinel());
    EXPECT_TRUE(col.IsLive(e));
    EXPECT_EQ(col.Get(e), Astra::EmptyComponentSentinel());
    EXPECT_FALSE(col.MarkWritten(e, 5));       // a tag has no value to write (Table: no column)
    col.Destroy(e);
    EXPECT_FALSE(col.IsLive(e));
}

TEST(UniversalColumn, TrackedTicksArePerSlotUntrackedIsPageCoarse)
{
    Tick now = 4;
    UniversalColumn tracked(Desc<Astra::Test::TrackedPos>());
    tracked.SetTickSource(&now);
    const Entity a(1, 1), b(2, 1);
    (void)tracked.Construct(a);
    now = 6;
    (void)tracked.Construct(b);
    ASSERT_NE(tracked.Ticks(a), nullptr);
    EXPECT_EQ(tracked.Ticks(a)->added, 4u);
    EXPECT_EQ(tracked.Ticks(b)->added, 6u);
    now = 9;
    EXPECT_TRUE(tracked.MarkWritten(a, now));
    EXPECT_EQ(tracked.ChangedTick(a), 9u);
    EXPECT_EQ(tracked.ChangedTick(b), 6u);     // exact: b was not written
    EXPECT_EQ(tracked.AddedTick(a), 4u);

    UniversalColumn plain(Desc<Position>());
    plain.SetTickSource(&now);
    now = 2;
    (void)plain.Construct(a);
    (void)plain.Construct(b);
    EXPECT_EQ(plain.Ticks(a), nullptr);        // untracked: no per-slot ticks
    EXPECT_EQ(plain.AddedTick(a), 2u);
    now = 5;
    EXPECT_TRUE(plain.MarkWritten(a, now));
    EXPECT_EQ(plain.ChangedTick(b), 5u);       // page-coarse, the chunk-column-version twin
    EXPECT_EQ(plain.ChangedTick(Entity(1, 2)), 0u);   // stale handle: 0 == never
}

TEST(UniversalColumn, DisabledBitsExistOnlyForEnableableTypes)
{
    UniversalColumn en(Desc<Astra::Test::Timer>());
    const Entity e(3, 1);
    (void)en.Construct(e);
    EXPECT_FALSE(en.IsDisabled(e));            // born enabled
    EXPECT_TRUE(en.SetDisabled(e, true));
    EXPECT_FALSE(en.SetDisabled(e, true));     // idempotent: no change
    EXPECT_TRUE(en.IsDisabled(e));
    en.Destroy(e);
    (void)en.Construct(Entity(3, 2));
    EXPECT_FALSE(en.IsDisabled(Entity(3, 2))); // the next occupant is born enabled

    UniversalColumn plain(Desc<Position>());
    (void)plain.Construct(e);
    EXPECT_FALSE(plain.SetDisabled(e, true));  // not enableable: nothing to store
    EXPECT_FALSE(plain.IsDisabled(e));
}

// Store-level half of Review Focus 1 (the registry-level pin is Task 9): an older
// version of a recycled id must miss on every accessor and never touch the occupant.
TEST(UniversalColumn, StaleHandleMissesAfterRecycleAndClear)
{
    using Astra::Test::TrackedVel;   // change-tracked AND enableable: every per-slot array exists
    UniversalColumn col(Desc<TrackedVel>());
    const Entity old(10, 1), fresh(10, 2);
    static_cast<TrackedVel*>(col.Construct(old))->dx = 7.0f;
    col.Destroy(old);
    auto* v = static_cast<TrackedVel*>(col.Construct(fresh));
    v->dx = 9.0f;

    EXPECT_EQ(col.Get(old), nullptr);
    EXPECT_FALSE(col.IsLive(old));
    EXPECT_EQ(col.Ticks(old), nullptr);
    EXPECT_FALSE(col.MarkWritten(old, 3));
    EXPECT_FALSE(col.SetDisabled(old, true));
    EXPECT_FALSE(col.IsDisabled(fresh));       // the stale write never reached the occupant
    col.Destroy(old);                          // stale destroy: a no-op
    ASSERT_TRUE(col.IsLive(fresh));
    EXPECT_FLOAT_EQ(v->dx, 9.0f);

    col.Clear();
    EXPECT_EQ(col.Get(fresh), nullptr);
    EXPECT_EQ(col.Get(Entity(10, 3)), nullptr);
}
```

- [ ] **Step 2: Regenerate projects, build, and confirm the failure**

A new `tests/**.cpp` file was added, so regenerate first (PowerShell, from the worktree root):

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS with `fatal error C1083: Cannot open include file: 'Astra/Registry/UniversalColumn.hpp'`.

- [ ] **Step 3: Implement `UniversalColumn`**

Create `include/Astra/Registry/UniversalColumn.hpp`:

```cpp
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "../Component/Component.hpp"
#include "../Core/Assert.hpp"
#include "../Core/Base.hpp"
#include "../Core/Tick.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    /**
     * Universal-component storage (spec 2026-10-01 s4.2). There is one per component
     * that a registry declares universal.
     *
     * Layout: a paged, slot-indexed column with no indirection. The value of entity e
     * lives on page e.GetID() / PAGE_SLOTS, at slot e.GetID() % PAGE_SLOTS.
     *
     * Each page holds:
     *  - the aligned data block (none for an empty type);
     *  - a live bitmap;
     *  - the full Entity handle per slot, so a stale handle misses on its version;
     *  - a live count;
     *  - EntityTicks, iff the type is change-tracked;
     *  - a disabled bitmap, iff the type is enableable;
     *  - one coarse version Tick: the untracked change-detection tier, the twin of a
     *    chunk's column version.
     *
     * Pages are allocated when an id first lands in their range, and released when their
     * live count reaches zero. The store is type-erased through a descriptor held BY
     * VALUE (the ResourceStorage lifetime pattern). Values never move.
     */
    class UniversalColumn
    {
    public:
        static constexpr std::size_t PAGE_SLOTS = static_cast<std::size_t>(ASTRA_UNIVERSAL_PAGE_SLOTS);
        static constexpr std::size_t PAGE_WORDS = PAGE_SLOTS / 64;
        static_assert(PAGE_SLOTS >= 64 && std::has_single_bit(PAGE_SLOTS),
                      "ASTRA_UNIVERSAL_PAGE_SLOTS must be a power of two >= 64");

        explicit UniversalColumn(const ComponentDescriptor& desc) : m_desc(desc) {}
        ~UniversalColumn() { Clear(); }

        UniversalColumn(const UniversalColumn&) = delete;
        UniversalColumn& operator=(const UniversalColumn&) = delete;
        UniversalColumn(UniversalColumn&&) = delete;
        UniversalColumn& operator=(UniversalColumn&&) = delete;

        // The change-detection counter that construction stamps from
        // (ArchetypeManager::TickSource()). Null reads as tick 0 ("never").
        void SetTickSource(const Tick* tick) noexcept { m_tick = tick; }

        void* Construct(Entity e) { return ConstructImpl(e, nullptr); }
        void* ConstructMove(Entity e, void* src) { return ConstructImpl(e, src); }

        // Destroys e's value. An absent or stale handle is a no-op: an older version of a
        // recycled id never reaches the current occupant.
        void Destroy(Entity e)
        {
            Page* page = FindLive(e);
            if (!page) ASTRA_UNLIKELY
                return;
            const std::size_t slot = SlotOf(e);
            if (page->data)
                m_desc.Destruct(page->data + slot * m_desc.size);
            ClearBit(page->live, slot);
            if (page->disabled)
                ClearBit(page->disabled.get(), slot);
            page->entities[slot] = Entity::Invalid();
            --page->liveCount;
            --m_liveCount;
            if (page->liveCount == 0)
                m_pages[PageOf(e)].reset();
        }

        ASTRA_NODISCARD void* Get(Entity e) noexcept
        {
            Page* page = FindLive(e);
            return page ? SlotPtr(*page, SlotOf(e)) : nullptr;
        }

        ASTRA_NODISCARD const void* Get(Entity e) const noexcept
        {
            Page* page = FindLive(e);
            return page ? SlotPtr(*page, SlotOf(e)) : nullptr;
        }

        ASTRA_NODISCARD bool IsLive(Entity e) const noexcept { return FindLive(e) != nullptr; }

        ASTRA_NODISCARD bool IsDisabled(Entity e) const noexcept
        {
            const Page* page = FindLive(e);
            return page && page->disabled && TestBit(page->disabled.get(), SlotOf(e));
        }

        // SET == DISABLED, matching the chunk bits. Returns true IFF the state changed.
        // Returns false for an absent or stale handle, or a non-enableable type.
        bool SetDisabled(Entity e, bool disabled) noexcept
        {
            Page* page = FindLive(e);
            if (!page || !page->disabled) ASTRA_UNLIKELY
                return false;
            const std::size_t slot = SlotOf(e);
            if (TestBit(page->disabled.get(), slot) == disabled)
                return false;
            if (disabled) SetBit(page->disabled.get(), slot);
            else          ClearBit(page->disabled.get(), slot);
            return true;
        }

        ASTRA_NODISCARD EntityTicks* Ticks(Entity e) noexcept
        {
            Page* page = FindLive(e);
            return (page && page->ticks) ? &page->ticks[SlotOf(e)] : nullptr;
        }

        // Write-side stamp (Registry::GetComponent / Modified / SetIfNeq): bump the page
        // version and, for a tracked type, the slot's `changed`. Returns false for an
        // absent or stale handle, or for a tag, which has no value (the Table rule for a
        // column-less tag).
        bool MarkWritten(Entity e, Tick now) noexcept
        {
            if (m_desc.size == 0) ASTRA_UNLIKELY
                return false;
            Page* page = FindLive(e);
            if (!page) ASTRA_UNLIKELY
                return false;
            page->version = now;
            if (page->ticks)
                page->ticks[SlotOf(e)].changed = now;
            return true;
        }

        // Exact for a tracked type. For an untracked type, the page version is returned:
        // every live slot of a stamped page answers it, the same trade a chunk's column
        // version makes. Returns 0 ("never") when e is not live.
        ASTRA_NODISCARD Tick AddedTick(Entity e) const noexcept
        {
            const Page* page = FindLive(e);
            if (!page) return Tick{0};
            return page->ticks ? page->ticks[SlotOf(e)].added : page->version;
        }

        ASTRA_NODISCARD Tick ChangedTick(Entity e) const noexcept
        {
            const Page* page = FindLive(e);
            if (!page) return Tick{0};
            return page->ticks ? page->ticks[SlotOf(e)].changed : page->version;
        }

        ASTRA_NODISCARD std::size_t LiveCount() const noexcept { return m_liveCount; }

        ASTRA_NODISCARD std::size_t PageCount() const noexcept
        {
            std::size_t n = 0;
            for (const auto& page : m_pages)
                n += page ? 1u : 0u;
            return n;
        }

        // fn(Entity, const void* component) for every live slot, in ascending index
        // order. Walks the live bitmaps, not the EntityManager (Clear, destroy and
        // serialization use it).
        template<class Fn>
        void ForEachLive(Fn&& fn) const
        {
            for (const auto& owned : m_pages)
            {
                const Page* page = owned.get();
                if (!page)
                    continue;
                for (std::size_t w = 0; w < PAGE_WORDS; ++w)
                {
                    std::uint64_t bits = page->live[w];
                    while (bits != 0)
                    {
                        const std::size_t slot = (w << 6) + static_cast<std::size_t>(std::countr_zero(bits));
                        bits &= bits - 1;
                        fn(page->entities[slot], static_cast<const void*>(SlotPtr(*page, slot)));
                    }
                }
            }
        }

        // Destroys every live value and releases every page. The column itself, with its
        // descriptor and tick source, stays usable.
        void Clear()
        {
            if (m_desc.size > 0 && !m_desc.is_trivially_destructible)
                ForEachLive([this](Entity, const void* p) { m_desc.Destruct(const_cast<void*>(p)); });
            m_pages.clear();
            m_liveCount = 0;
        }

        ASTRA_NODISCARD const ComponentDescriptor& Descriptor() const noexcept { return m_desc; }

    private:
        struct Page
        {
            std::byte* data = nullptr;                    // PAGE_SLOTS * size, desc.alignment-aligned; null for a tag
            std::unique_ptr<Entity[]> entities;           // value-initialised: Entity::Invalid()
            std::unique_ptr<EntityTicks[]> ticks;         // iff change-tracked; zeroed (tick 0 == never)
            std::unique_ptr<std::uint64_t[]> disabled;    // iff enableable; zeroed (born enabled)
            std::uint64_t live[PAGE_WORDS] = {};
            std::uint32_t liveCount = 0;
            Tick version = 0;
            std::size_t alignment = 1;

            explicit Page(const ComponentDescriptor& desc) :
                entities(std::make_unique<Entity[]>(PAGE_SLOTS)),
                alignment(desc.alignment)
            {
                if (desc.size > 0)
                    data = static_cast<std::byte*>(::operator new(desc.size * PAGE_SLOTS, std::align_val_t{desc.alignment}));
                if (desc.isChangeTracked)
                    ticks = std::make_unique<EntityTicks[]>(PAGE_SLOTS);
                if (desc.isEnableable)
                    disabled = std::make_unique<std::uint64_t[]>(PAGE_WORDS);
            }

            ~Page()
            {
                if (data)
                    ::operator delete(data, std::align_val_t{alignment});
            }

            Page(const Page&) = delete;
            Page& operator=(const Page&) = delete;
        };

        static std::size_t PageOf(Entity e) noexcept { return static_cast<std::size_t>(e.GetID()) / PAGE_SLOTS; }
        static std::size_t SlotOf(Entity e) noexcept { return static_cast<std::size_t>(e.GetID()) % PAGE_SLOTS; }

        static bool TestBit(const std::uint64_t* words, std::size_t i) noexcept { return ((words[i >> 6] >> (i & 63)) & 1ull) != 0; }
        static void SetBit(std::uint64_t* words, std::size_t i) noexcept { words[i >> 6] |= (1ull << (i & 63)); }
        static void ClearBit(std::uint64_t* words, std::size_t i) noexcept { words[i >> 6] &= ~(1ull << (i & 63)); }

        // The page holding e's value IFF that slot is live AND holds exactly this handle
        // (version included). Every accessor funnels through here, which is what makes a
        // stale handle miss.
        Page* FindLive(Entity e) const noexcept
        {
            if (!e.IsValid()) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t pageIndex = PageOf(e);
            if (pageIndex >= m_pages.size()) ASTRA_UNLIKELY
                return nullptr;
            Page* page = m_pages[pageIndex].get();
            if (!page) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t slot = SlotOf(e);
            if (!TestBit(page->live, slot) || page->entities[slot] != e) ASTRA_UNLIKELY
                return nullptr;
            return page;
        }

        void* SlotPtr(const Page& page, std::size_t slot) const noexcept
        {
            if (m_desc.size == 0)
                return EmptyComponentSentinel();   // present tag: non-null, never dereferenced
            return page.data + slot * m_desc.size;
        }

        // s4.2 step 1. `src` == nullptr: default-construct; otherwise move-construct from it.
        void* ConstructImpl(Entity e, void* src)
        {
            ASTRA_ASSERT(e.IsValid(), "UniversalColumn::Construct: invalid entity");
            if (!e.IsValid()) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t pageIndex = PageOf(e);
            if (pageIndex >= m_pages.size())
                m_pages.resize(pageIndex + 1);
            std::unique_ptr<Page>& owned = m_pages[pageIndex];
            if (!owned)
                owned = std::make_unique<Page>(m_desc);
            Page& page = *owned;
            const std::size_t slot = SlotOf(e);
            if (TestBit(page.live, slot)) ASTRA_UNLIKELY
            {
                // Precondition breach: the previous occupant was never destroyed (a destroy
                // path missed the store). In Release, destroy it rather than leak it.
                ASTRA_ASSERT(false, "UniversalColumn::Construct over a live slot: a destroy path missed the store");
                if (page.data)
                    m_desc.Destruct(page.data + slot * m_desc.size);
                --page.liveCount;
                --m_liveCount;
            }
            if (page.data)
            {
                void* ptr = page.data + slot * m_desc.size;
                if (src) m_desc.MoveConstruct(ptr, src);
                else     m_desc.DefaultConstruct(ptr);
            }
            SetBit(page.live, slot);
            if (page.disabled)
                ClearBit(page.disabled.get(), slot);   // born enabled
            page.entities[slot] = e;
            ++page.liveCount;
            ++m_liveCount;
            const Tick now = m_tick ? *m_tick : Tick{0};
            page.version = now;                        // the AddEntity StampAllColumns twin
            if (page.ticks)
                page.ticks[slot] = EntityTicks{now, now};
            return SlotPtr(page, slot);
        }

        ComponentDescriptor m_desc;
        std::vector<std::unique_ptr<Page>> m_pages;
        std::size_t m_liveCount = 0;
        const Tick* m_tick = nullptr;
    };
}
```

- [ ] **Step 4: Build and run the suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalColumn.*
```

Expected: `[  PASSED  ] 7 tests.`

- [ ] **Step 5: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the `Randomizing tests' orders with a seed of N` line in the task report.

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Registry/UniversalColumn.hpp tests/Registry/UniversalColumnTest.cpp
git commit -m "feat(storage): UniversalColumn -- paged slot-indexed universal store (1024-slot pages, live bitmap, per-slot Entity for stale-handle misses, ticks iff tracked, disabled bits iff enableable, page version for the untracked tier)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Universal end to end -- `ComponentStorage` owns the columns, every create path constructs them, Get/Has route; the Universal stale-handle pin

Spec s4.2, s7.1 and s3.3 (the Get/Has rows), plus s7.3 for Universal destruction.

This task:
- has `ComponentStorage` own one lazily created `UniversalColumn` per universal id;
- introduces the private creation funnel `Registry::CreateEntityInternal`;
- reroutes `CommandBuffer::ExecuteCreateEntity`/`ExecuteCreateEntities` through that funnel;
- adds universal construction (step 1 default, step 2 init, step 3 `ComponentAdded`) to every typed creator;
- destroys universal values on `DestroyEntity`, `DestroyEntities`, `Clear` and registry destruction;
- routes typed `GetComponent`/`HasComponent` for a universal `T` to the column.

Packs that NAME a universal or Sparse type are split in Task 11. Until then the typed creators keep their archetype placement and only gain the universal construction.

**Review Focus 1 (Universal) is pinned here.** The `UniversalStorageStaleHandle` suite proves that a destroy-recycled or `Clear()`-recycled handle misses on `Get`, const `Get` and `Has`, and never reads the new occupant. Tasks 12 and 13 extend the same suite with `Add`, `Remove` and `SetEnabled`.

**Files:**
- Modify: `include/Astra/Registry/ComponentStorage.hpp` (Chunk A's skeleton)
- Modify: `include/Astra/Registry/Registry.hpp`
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (add `TickSource()`)
- Modify: `include/Astra/Commands/CommandBuffer.hpp:1441-1496` (the create executors)
- Create: `tests/Registry/UniversalStorageTest.cpp`

**Interfaces:**
- Consumes: `UniversalColumn` (Task 8); `ComponentStorage::KindOf`, `UniversalSpecs()`, `m_registry` and `m_universal` (Chunk A, Task 4); `Registry::Config::AddUniversal<T>` (Chunk A); the `Registry::m_componentStorage` shared_ptr (Chunk A).
- Produces:
  ```cpp
  // ComponentStorage (canonical: Universal, ConstructUniversals, OnEntityDestroyed, Clear; + = additive)
  UniversalColumn* Universal(ComponentID id);                      // lazily created for Universal kinds, else nullptr
  void ConstructUniversals(Entity e, const ComponentID* supplied, size_t suppliedCount);  // step 1 default (skip supplied), step 2 init (skip supplied)
  void OnEntityDestroyed(Entity e, ArchetypeChunk& chunk, size_t index);                  // destroys e's universal values
  void Clear();                                                    // fills Chunk A's no-op: destroys every value; columns, specs and slots survive
  + void SetTickSource(const Tick* tick) noexcept;
  + bool HasUniversals() const noexcept;                           // !m_universal.empty()
  + bool HasAnyStore() const noexcept;                             // any column or sparse set exists (destroy fast path)
  + UniversalColumn* FindUniversal(ComponentID id) const noexcept; // never creates
  // ArchetypeManager
  + const Tick* TickSource() const noexcept;                       // &m_tick
  // Registry (private)
  Entity CreateEntityInternal(Entity preallocated);                // root placement + universals + EntityCreated + universal ComponentAdded
  friend class CommandBuffer;
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/UniversalStorageTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

using Astra::Entity;
using Astra::Registry;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace
{
    struct InitLog { int calls = 0; };

    // A registry init (s4.2 step 2): stamps a recognisable per-entity value so a test can
    // tell "init ran" from "default-constructed" from "moved in".
    void InitPosition(Entity e, void* component, void* user)
    {
        static_cast<Position*>(component)->x = 100.0f + static_cast<float>(e.GetID());
        ++static_cast<InitLog*>(user)->calls;
    }

    Registry::Config UniversalPositionConfig(InitLog* log)
    {
        Registry::Config cfg;
        if (log) cfg.AddUniversal<Position>(&InitPosition, log);
        else     cfg.AddUniversal<Position>();
        return cfg;
    }

    bool MaskHas(const Registry& reg, Entity e, Astra::ComponentID id)
    {
        const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        return rec && rec->archetype->GetMask().Test(id);
    }
}

TEST(UniversalStorage, CreateEntityConstructsEveryUniversalThenRunsInit)
{
    InitLog log;
    Registry reg(UniversalPositionConfig(&log));
    const Entity e = reg.CreateEntity();

    ASSERT_TRUE(reg.HasComponent<Position>(e));
    const Position* p = std::as_const(reg).GetComponent<Position>(e);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 100.0f + static_cast<float>(e.GetID()));   // init ran...
    EXPECT_FLOAT_EQ(p->y, 0.0f);                                      // ...over a value-initialised slot
    EXPECT_EQ(log.calls, 1);
    EXPECT_FALSE(MaskHas(reg, e, Astra::TypeID<Position>::Value())); // never an archetype column
}

TEST(UniversalStorage, EveryCreatePathConstructsUniversals)
{
    InitLog log;
    Registry reg(UniversalPositionConfig(&log));
    std::vector<Entity> made;
    made.push_back(reg.CreateEntity<Velocity>());
    made.push_back(reg.CreateEntityWith(Velocity{1.0f, 2.0f, 3.0f}));

    std::vector<Entity> batch(4);
    ASSERT_EQ(reg.CreateEntities<Velocity>(4, std::span{batch}), 4u);
    made.insert(made.end(), batch.begin(), batch.end());
    ASSERT_EQ((reg.CreateEntitiesWith<Velocity>(4, std::span{batch},
        [](size_t i) { return std::tuple{Velocity{static_cast<float>(i), 0.0f, 0.0f}}; })), 4u);
    made.insert(made.end(), batch.begin(), batch.end());

    {
        Astra::CommandBuffer cb(&reg);   // declared after the Registry (CommandBlockArena order)
        const Entity viaCmd = cb.CreateEntity();
        Entity cmdBatch[3];
        cb.CreateEntities(3, cmdBatch);
        EXPECT_FALSE(cb.Execute().IsErr());
        made.push_back(viaCmd);
        made.insert(made.end(), std::begin(cmdBatch), std::end(cmdBatch));
    }

    ASSERT_EQ(made.size(), 14u);
    for (Entity e : made)
    {
        ASSERT_TRUE(reg.IsValid(e));
        const Position* p = std::as_const(reg).GetComponent<Position>(e);
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, 100.0f + static_cast<float>(e.GetID()));
    }
    EXPECT_EQ(log.calls, 14);
}

TEST(UniversalStorage, ComponentAddedFiresOncePerUniversalAfterEntityCreated)
{
    Registry reg(UniversalPositionConfig(nullptr));
    reg.EnableSignals(Astra::Signal::EntityCreated | Astra::Signal::ComponentAdded);
    std::vector<std::string> order;
    auto* signals = reg.GetSignalManager();
    auto created = signals->On<Astra::Events::EntityCreated>().Register(
        [&](const Astra::Events::EntityCreated&) { order.push_back("created"); });
    auto added = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev)
        {
            if (ev.componentId == Astra::TypeID<Position>::Value())
            {
                EXPECT_EQ(static_cast<const void*>(ev.component),
                          static_cast<const void*>(std::as_const(reg).GetComponent<Position>(ev.entity)));
                order.push_back("position");
            }
            else
            {
                order.push_back("other");
            }
        });

    (void)reg.CreateEntityWith(Velocity{});
    (void)reg.CreateEntity();
    EXPECT_EQ(order, (std::vector<std::string>{"created", "other", "position", "created", "position"}));

    signals->On<Astra::Events::EntityCreated>().Unregister(created);
    signals->On<Astra::Events::ComponentAdded>().Unregister(added);
}

TEST(UniversalStorage, DestroyClearAndRegistryDestructionDestroyUniversalValues)
{
    using Astra::Test::Tracked;   // move-only, lifetime-counted
    const int base = Tracked::s_live;
    {
        Registry::Config cfg;
        cfg.AddUniversal<Tracked>();
        Registry reg(cfg);
        std::vector<Entity> ents(4);
        ASSERT_EQ(reg.CreateEntities<Velocity>(4, std::span{ents}), 4u);
        EXPECT_EQ(Tracked::s_live, base + 4);

        reg.DestroyEntity(ents[0]);
        EXPECT_EQ(Tracked::s_live, base + 3);
        reg.DestroyEntities(std::span{ents}.subspan(1, 2));
        EXPECT_EQ(Tracked::s_live, base + 1);
        reg.Clear();
        EXPECT_EQ(Tracked::s_live, base);

        (void)reg.CreateEntity();
        (void)reg.CreateEntity();
        EXPECT_EQ(Tracked::s_live, base + 2);
    }   // ~Registry destroys the rest
    EXPECT_EQ(Tracked::s_live, base);
}

// ---- Review Focus 1 (Universal): stale handles after destroy-recycle and Clear ----
// T1-C2: ids come back at version+1. A handle to the previous occupant must miss on
// every surface and must never read or write the new occupant's slot. Tasks 12/13
// extend this suite with Add, Remove and SetEnabled.

TEST(UniversalStorageStaleHandle, DestroyRecycleMissesOnGetAndHas)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity stale = reg.CreateEntity();
    Position* before = reg.GetComponent<Position>(stale);
    ASSERT_NE(before, nullptr);
    before->x = 7.0f;
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<Position>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<Position>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<Position>(stale));

    const Position* now = std::as_const(reg).GetComponent<Position>(fresh);
    ASSERT_NE(now, nullptr);
    EXPECT_FLOAT_EQ(now->x, 0.0f);             // reconstructed, not the stale value
}

TEST(UniversalStorageStaleHandle, BatchDestroyRecycleMisses)
{
    Registry reg(UniversalPositionConfig(nullptr));
    std::vector<Entity> ents(3);
    ASSERT_EQ(reg.CreateEntities<>(3, std::span{ents}), 3u);
    Position* before = reg.GetComponent<Position>(ents[1]);
    ASSERT_NE(before, nullptr);
    before->x = 5.0f;
    reg.DestroyEntities(std::span{ents});
    std::vector<Entity> again(3);
    ASSERT_EQ(reg.CreateEntities<>(3, std::span{again}), 3u);
    for (Entity stale : ents)
    {
        EXPECT_EQ(reg.GetComponent<Position>(stale), nullptr);
        EXPECT_FALSE(reg.HasComponent<Position>(stale));
    }
    for (Entity e : again)
    {
        const Position* p = std::as_const(reg).GetComponent<Position>(e);
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, 0.0f);
    }
}

TEST(UniversalStorageStaleHandle, ClearRecycleMissesOnGetAndHas)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity stale = reg.CreateEntity();
    Position* before = reg.GetComponent<Position>(stale);
    ASSERT_NE(before, nullptr);
    before->x = 7.0f;
    reg.Clear();
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<Position>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<Position>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<Position>(stale));
    const Position* now = std::as_const(reg).GetComponent<Position>(fresh);
    ASSERT_NE(now, nullptr);
    EXPECT_FLOAT_EQ(now->x, 0.0f);
}
```

- [ ] **Step 2: Regenerate, build, and confirm the failure**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*
```

Expected: the build succeeds, because every API used exists after Chunk A. The suites then FAIL:
- the creation tests at `ASSERT_TRUE(reg.HasComponent<Position>(e))`, because no create path constructs a universal yet and the root archetype has no Position;
- the lifetime test at `EXPECT_EQ(Tracked::s_live, base + 4)`, with `base` observed instead;
- the stale-handle tests at `ASSERT_NE(before, nullptr)`, because the universal value was never constructed.

- [ ] **Step 3: `ArchetypeManager::TickSource()`**

In `include/Astra/Archetype/ArchetypeManager.hpp`, directly below `AdvanceTick()` (around line 692-697), add:

```cpp
        // Address of the change-detection counter, for stores that stamp outside a chunk
        // (ComponentStorage, spec 2026-10-01 s4). Stable for this manager's lifetime; a
        // Load builds a new manager, so the registry re-points its stores (LoadInternal).
        ASTRA_NODISCARD const Tick* TickSource() const noexcept { return &m_tick; }
```

- [ ] **Step 4: `ComponentStorage` owns the universal columns**

In `include/Astra/Registry/ComponentStorage.hpp`, add `#include <array>`, `#include "../Core/Tick.hpp"`, `#include "../Archetype/ArchetypeChunkPool.hpp"` (`OnEntityDestroyed` names `ArchetypeChunk`; Task 21's body needs the complete type) and `#include "UniversalColumn.hpp"` beside the existing includes.

Add the members below to the public section. Chunk A's `Clear()` (Task 4) is a no-op placeholder: REPLACE it with the one below. Chunk A declares no `Universal`, `ConstructUniversals` or `OnEntityDestroyed`; this step introduces them. `UniversalSpecs()`, `FindUniversalSpec()` and `UsedSparseSlotCount()` already exist (Chunk A), so do not add them again. The sparse-slot assignments must survive `Clear` (spec s7.3).

```cpp
        // ================= Universal columns (spec 2026-10-01 s4.2) =================

        // Every store stamps change ticks from the owning ArchetypeManager's counter.
        // The Registry sets this at construction and again in LoadInternal (a Load
        // builds a new ArchetypeManager).
        void SetTickSource(const Tick* tick) noexcept
        {
            m_tickSource = tick;
            for (const UniversalSpec& spec : m_universal)
                if (UniversalColumn* column = FindUniversal(spec.id))
                    column->SetTickSource(tick);
        }

        ASTRA_NODISCARD bool HasUniversals() const noexcept { return !m_universal.empty(); }

        // True once any universal column or sparse set exists. The Registry's destroy paths
        // skip the storage walk entirely while this is false (G4: Table-only registries pay
        // one predictable branch).
        ASTRA_NODISCARD bool HasAnyStore() const noexcept { return m_storeCount != 0; }

        // Lookup without creation. Read paths, which are const at the Registry, use this.
        ASTRA_NODISCARD UniversalColumn* FindUniversal(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS ? m_universalColumns[id].get() : nullptr;
        }

        // The column for a Universal-kind id, created on first use from the registered
        // descriptor, which the column holds BY VALUE. Returns nullptr for any other kind,
        // and for an id whose descriptor is not registered.
        UniversalColumn* Universal(ComponentID id)
        {
            if (id >= MAX_COMPONENTS || KindOf(id) != StorageKind::Universal) ASTRA_UNLIKELY
                return nullptr;
            std::unique_ptr<UniversalColumn>& owned = m_universalColumns[id];
            if (!owned) ASTRA_UNLIKELY
            {
                const ComponentDescriptor* desc = m_registry ? m_registry->GetComponentDescriptor(id) : nullptr;
                if (!desc) ASTRA_UNLIKELY
                    return nullptr;
                owned = std::make_unique<UniversalColumn>(*desc);
                owned->SetTickSource(m_tickSource);
                ++m_storeCount;
            }
            return owned.get();
        }

        // Spec s4.2. Construction order for one freshly placed entity:
        //   (1) every universal the caller did NOT supply is default-constructed;
        //   (2) the registry's init runs for exactly those, never over a supplied value.
        // A supplied value was already move-constructed by the caller (ConstructMove)
        // before this runs. Two passes, so that an init may read any other universal of
        // the same entity.
        void ConstructUniversals(Entity e, const ComponentID* supplied, size_t suppliedCount)
        {
            const auto isSupplied = [supplied, suppliedCount](ComponentID id) noexcept
            {
                for (size_t i = 0; i < suppliedCount; ++i)
                    if (supplied[i] == id)
                        return true;
                return false;
            };
            for (const UniversalSpec& spec : m_universal)
            {
                if (isSupplied(spec.id))
                    continue;
                if (UniversalColumn* column = Universal(spec.id)) ASTRA_LIKELY
                    (void)column->Construct(e);
            }
            for (const UniversalSpec& spec : m_universal)
            {
                if (!spec.init || isSupplied(spec.id))
                    continue;
                if (UniversalColumn* column = FindUniversal(spec.id))
                    if (void* component = column->Get(e))
                        spec.init(e, component, spec.user);
            }
        }

        // Destroy hook (spec s7.3). Called after EntityDestroyed and before the archetype
        // swap-remove. Destroys e's slot in every universal column. Task 11 adds the
        // sparse erase. Chunk C replaces that erase with the presence-block walk, which
        // reads `chunk`/`index`.
        void OnEntityDestroyed(Entity e, ArchetypeChunk& chunk, size_t index)
        {
            (void)chunk;
            (void)index;
            for (const UniversalSpec& spec : m_universal)
                if (UniversalColumn* column = FindUniversal(spec.id))
                    column->Destroy(e);
        }

        // Registry::Clear (spec s7.3): destroys every element. The columns, their
        // descriptors and the specs survive, and so do Chunk A's sparse-slot assignments.
        void Clear()
        {
            for (const UniversalSpec& spec : m_universal)
                if (UniversalColumn* column = FindUniversal(spec.id))
                    column->Clear();
        }
```

Add to the private section:

```cpp
        std::array<std::unique_ptr<UniversalColumn>, MAX_COMPONENTS> m_universalColumns{};
        const Tick* m_tickSource = nullptr;
        std::uint32_t m_storeCount = 0;   // columns + sparse sets created so far (HasAnyStore)
```

`ComponentStorage` now holds `std::unique_ptr` members, so it is not copyable. The Registry holds it through a `std::shared_ptr`, so nothing copies it. Task 4 already deletes the copy constructor and copy assignment; keep them deleted.

- [ ] **Step 5: Registry -- tick source, the creation funnel, destroy/Clear, Get/Has routing**

All edits are in `include/Astra/Registry/Registry.hpp`. Locate each site by the quoted content, because Chunk A has shifted the line numbers.

(a) Directly above `class Registry` (inside `namespace Astra`), forward-declare the friend:

```cpp
    class CommandBuffer;   // friend: its create executors use the private creation funnel
```

(b) In each of the three constructors, Chunk A builds `m_componentStorage` in the member-initialiser list. Add this statement to the constructor body (append it to whatever the body already holds):

```cpp
            m_componentStorage->SetTickSource(m_archetypeManager->TickSource());
```

(c) In `LoadInternal`, Task 5 (g) rebuilds `registry->m_archetypeManager` and then, on the next line, `registry->m_componentStorage` (from the caller's `Config`), both before `if (!registry->m_archetypeManager->Deserialize(reader))`. Directly after the ComponentStorage rebuild line, so after BOTH rebuilds and before any deserialisation that could construct a store, add:

```cpp
            // The stores stamp from the archetype manager's counter. Both were just
            // rebuilt, so re-point them; a stale pointer would be a use-after-free.
            registry->m_componentStorage->SetTickSource(registry->m_archetypeManager->TickSource());
```

(d) Replace the whole body of `CreateEntity<Components...>()` with:

```cpp
        template<Component... Components>
        Entity CreateEntity()
        {
            Entity entity = m_entityManager.Create();
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();

            if constexpr (sizeof...(Components) == 0)
            {
                return CreateEntityInternal(entity);
            }
            else
            {
                m_archetypeManager->AddEntity<Components...>(entity);
                ConstructUniversalsFor(entity, nullptr, 0);

                m_signalManager.Emit<Events::EntityCreated>(entity);

                if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
                {
                    // Get the entity location to retrieve components for signals
                    auto* record = m_archetypeManager->GetEntityRecord(entity);
                    if (record)
                    {
                        ((m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<Components>::Value(), record->archetype->GetComponent<Components>(record->location))), ...);
                    }
                }
                EmitUniversalAdded(entity);
                return entity;
            }
        }
```

(e) In `CreateEntityWith`:
- directly after `m_archetypeManager->AddEntityWith(entity, std::forward<Components>(components)...);`, add `ConstructUniversalsFor(entity, nullptr, 0);`;
- directly before its final `return entity;`, add `EmitUniversalAdded(entity);`.

(f) In `CreateEntities`:
- directly after `m_archetypeManager->AddEntities<Components...>(outEntities.subspan(0, created));`, add:

```cpp
            if (m_componentStorage->HasUniversals()) ASTRA_UNLIKELY
            {
                for (size_t i = 0; i < created; ++i)
                    m_componentStorage->ConstructUniversals(outEntities[i], nullptr, 0);
            }
```

- directly before its final `return created;`, add:

```cpp
            for (size_t i = 0; i < created; ++i)
                EmitUniversalAdded(outEntities[i]);
```

(g) In `CreateEntitiesWith`, make the same two insertions: after `m_archetypeManager->AddEntitiesWith<Components...>(...)`, and before `return created;`.

(h) In `DestroyEntity`, between the `EntityDestroyed` emit block and `m_archetypeManager->RemoveEntity(entity, rec);`, insert:

```cpp
            // Spec s7.3: storage teardown after EntityDestroyed (handlers can still read
            // every component) and before the archetype swap-remove. Per-component
            // ComponentRemoved is NOT emitted on destroy (today's behaviour).
            if (m_componentStorage->HasAnyStore()) ASTRA_UNLIKELY
            {
                ASTRA_ASSERT(rec->chunk, "DestroyEntity: located record without a chunk");
                if (rec->chunk)
                    m_componentStorage->OnEntityDestroyed(entity, *rec->chunk, rec->location.GetEntityIndex());
            }
```

(i) In `DestroyEntities`, between the `EntityDestroyed` emit block and `m_archetypeManager->RemoveEntities(validEntities);`, insert:

```cpp
            if (m_componentStorage->HasAnyStore()) ASTRA_UNLIKELY
            {
                for (Entity entity : validEntities)
                {
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    if (rec && rec->chunk)
                        m_componentStorage->OnEntityDestroyed(entity, *rec->chunk, rec->location.GetEntityIndex());
                }
            }
```

`rec->chunk` is an `ArchetypeChunk*` member (`Entity/EntityRecord.hpp:28`). The record's constness is top-level only, so `*rec->chunk` binds to `ArchetypeChunk&` without a cast.

(j) Chunk A's `Registry::Clear()` already calls `m_componentStorage->Clear()`, which was a no-op until Step 4. Make sure it is called exactly once and FIRST, before the entity manager recycles the ids, so `Clear()` reads:

```cpp
        void Clear()
        {
            m_componentStorage->Clear();   // destroy every store element first (spec s7.3)
            m_archetypeManager->Clear();   // in place -- keeps cached Views/Relations valid
            m_relationshipGraph->Clear();
            m_entityManager.Clear();
        }
```

(k) Route the typed reads. Replace the two `GetComponent<T>` overloads and `HasComponent<T>` with:

```cpp
        // Non-const access counts as a write for change detection (spec §3.3):
        // T's chunk column is stamped with the current tick. Use the const
        // overload (e.g. via std::as_const) for a read that must not stamp.
        template<Component T>
        ASTRA_NODISCARD T* GetComponent(Entity entity)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity))
                return nullptr;
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
                return StoreGet<T>(entity);
            return m_archetypeManager->GetComponentMut<T>(entity);
        }

        template<Component T>
        ASTRA_NODISCARD const T* GetComponent(Entity entity) const
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity))
                return nullptr;
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
                return StoreGet<T>(entity);
            return m_archetypeManager->GetComponent<T>(entity);
        }

        template<Component T>
        ASTRA_NODISCARD bool HasComponent(Entity entity) const
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity))
                return false;
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
                return StoreLookup(entity, TypeID<T>::Value()) != nullptr;
            return m_archetypeManager->HasComponent<T>(entity);
        }
```

(l) In the `private:` section (directly above `static Result<...> LoadInternal`), add the routing helpers and the creation funnel:

```cpp
        friend class CommandBuffer;   // ExecuteCreateEntity/ExecuteCreateEntities -> CreateEntityInternal

        // ---- Storage routing (spec 2026-10-01 s3.3) ----
        // A component's effective kind in THIS registry. Out-of-range ids (a refused
        // registration) are reported as Table, so the archetype path refuses them as it
        // does today.
        template<Component T>
        ASTRA_NODISCARD StorageKind KindOfT() const noexcept
        {
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return StorageKind::Table;
            return m_componentStorage->KindOf(id);
        }

        // True when T's value lives in a ComponentStorage store rather than a chunk
        // column. A Table-trait T is a store kind only when this registry declares it
        // universal: one predictable bool load in a registry with no universal (G4).
        // Task 11 adds the compile-time Sparse arm.
        template<Component T>
        ASTRA_NODISCARD bool RoutesToStore() const noexcept
        {
            return m_componentStorage->HasUniversals() && KindOfT<T>() == StorageKind::Universal;
        }

        // Non-stamping, type-erased store lookup: the element pointer (or
        // EmptyComponentSentinel() for a present tag), or nullptr when absent or stale.
        // Task 11 adds the Sparse arm.
        ASTRA_NODISCARD void* StoreLookup(Entity entity, ComponentID id) const noexcept
        {
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Universal:
                {
                    UniversalColumn* column = m_componentStorage->FindUniversal(id);
                    return column ? column->Get(entity) : nullptr;
                }
                default:
                    return nullptr;
            }
        }

        // Typed twin of StoreLookup. A present tag yields a per-type static, the same
        // convention ArchetypeChunk::GetComponent<T> uses for a column-less tag.
        template<Component T>
        ASTRA_NODISCARD T* StoreGet(Entity entity) const noexcept
        {
            void* p = StoreLookup(entity, TypeID<T>::Value());
            if (!p)
                return nullptr;
            if constexpr (std::is_empty_v<T>)
            {
                static T emptyInstance{};
                return &emptyInstance;
            }
            else
            {
                return static_cast<T*>(p);
            }
        }

        // ---- Creation funnel (spec 2026-10-01 s4.2, s7.1) ----
        // Universal construction for a freshly placed entity (steps 1-2). `supplied` lists
        // the universal ids whose value the caller already moved in; those skip default
        // and init. Costs one branch in a registry with no universal.
        void ConstructUniversalsFor(Entity entity, const ComponentID* supplied, size_t suppliedCount)
        {
            if (m_componentStorage->HasUniversals()) ASTRA_UNLIKELY
                m_componentStorage->ConstructUniversals(entity, supplied, suppliedCount);
        }

        // Step 3: ComponentAdded once for every universal of `entity`, after EntityCreated
        // and after the pack's own ComponentAdded. Supplied or not, a universal is "added"
        // at create (s3.3 signals row). By-id payload: a present tag yields
        // EmptyComponentSentinel().
        void EmitUniversalAdded(Entity entity)
        {
            if (!m_componentStorage->HasUniversals()) ASTRA_LIKELY
                return;
            if (!m_signalManager.IsSignalEnabled(Signal::ComponentAdded)) ASTRA_LIKELY
                return;
            for (const UniversalSpec& spec : m_componentStorage->UniversalSpecs())
            {
                UniversalColumn* column = m_componentStorage->FindUniversal(spec.id);
                if (!column)
                    continue;
                if (void* component = column->Get(entity))
                    m_signalManager.Emit<Events::ComponentAdded>(entity, spec.id, component);
            }
        }

        // THE single-entity creation funnel. Every create path without a component pack
        // ends here, including the CommandBuffer executors, so universal construction and
        // init cannot be skipped. `preallocated` is a live id that has no location yet.
        Entity CreateEntityInternal(Entity preallocated)
        {
            if (!preallocated.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();
            m_archetypeManager->AddEntity(preallocated);   // root archetype
            ConstructUniversalsFor(preallocated, nullptr, 0);
            m_signalManager.Emit<Events::EntityCreated>(preallocated);
            EmitUniversalAdded(preallocated);
            return preallocated;
        }
```

`UniversalSpec` is the namespace-scope `Astra::UniversalSpec` from `ComponentStorage.hpp` (Chunk A), so the unqualified spelling resolves inside `Registry`.

- [ ] **Step 6: Reroute the CommandBuffer create executors through the funnel**

In `include/Astra/Commands/CommandBuffer.hpp`:

- In `ExecuteCreateEntity`, replace
  ```cpp
              m_registry->GetArchetypeManager()->AddEntity(cmd->entity);
              m_registry->GetSignalManager()->Emit<Events::EntityCreated>(cmd->entity);
  ```
  with
  ```cpp
              // The Registry's creation funnel (spec 2026-10-01 s7.1): root placement,
              // universal construction + init, EntityCreated, universal ComponentAdded.
              m_registry->CreateEntityInternal(cmd->entity);
  ```

- In `ExecuteCreateEntities`, delete the two locals `archetypeManager` and `signalManager`. In the loop, replace
  ```cpp
                  archetypeManager->AddEntity(entities[i]);
                  signalManager->Emit<Events::EntityCreated>(entities[i]);
  ```
  with
  ```cpp
                  m_registry->CreateEntityInternal(entities[i]);
  ```

The comments and the `m_committedCount` bookkeeping stay exactly as they are.

- [ ] **Step 7: Build and run the suites**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*:CommandBuffer*:RegistryTest.*:ChangeDetection*
```

Expected: every test passes, including the 7 new ones (`UniversalStorage` 4, `UniversalStorageStaleHandle` 3) and every existing CommandBuffer, Registry and change-detection test (no Table behaviour changed).

- [ ] **Step 8: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 9: Commit**

```powershell
git add include/Astra/Registry/ComponentStorage.hpp include/Astra/Registry/Registry.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Commands/CommandBuffer.hpp tests/Registry/UniversalStorageTest.cpp
git commit -m "feat(storage): universal components end to end -- ComponentStorage owns lazily created UniversalColumns; every create path (incl. the CommandBuffer executors via the private Registry::CreateEntityInternal funnel) constructs them (default, then init, then ComponentAdded); destroy/Clear/registry destruction tear them down; typed Get/Has route to the column; stale handles after destroy-recycle and Clear miss (Review Focus 1, Universal)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: `SparseSet` -- the paged sparse / dense store, with its stale-handle pin

Spec s4.1. A standalone store, unit-tested through `MakeDescriptor<T>`:
- **Sparse half:** pages of `ASTRA_SPARSE_PAGE_SIZE` (4096) `uint32_t` dense positions, allocated on first insert into their range and released when they empty.
- **Dense half:** full `Entity` handles (a stale handle misses on its version), a stride-packed, aligned data buffer grown geometrically (none for a tag), `EntityTicks` iff change-tracked, and a `uint8_t` disabled flag iff enableable.
- **Remove:** swap-and-pop.

Untracked types get NO store-level coarse tick. Integration ruling (one coarse scheme per kind): the untracked-Sparse coarse tier is `ComponentStorage::CoarseVersion(id)` (Task 20); the untracked-Universal tier is `UniversalColumn`'s per-page version (Task 8). So `SparseSet::AddedTick`/`ChangedTick` answer `0` for an untracked element, and `MarkWritten` stamps only a tracked element's `changed`.

**Review Focus 1 (Sparse), store level, is pinned here** (`SparseSetStaleHandle`). The registry-level half follows in Task 11. Sparse `Add`/`Remove`/`SetEnabled` through the Registry are Chunk C's, which must extend the registry-level pin (see the notes).

**Files:**
- Create: `include/Astra/Registry/SparseSet.hpp`
- Create: `tests/Registry/SparseSetTest.cpp`

**Interfaces:**
- Consumes: `ComponentDescriptor`, `EmptyComponentSentinel()`, `ASTRA_SPARSE_PAGE_SIZE` (Chunk A), `Tick`, `EntityTicks`, `Entity`.
- Produces (canonical, plus the additions marked `+`):
  ```cpp
  class SparseSet {
  public:
    static constexpr std::size_t   PAGE_ENTRIES = ASTRA_SPARSE_PAGE_SIZE;   // 4096
    static constexpr std::uint32_t INVALID_INDEX = UINT32_MAX;
    explicit SparseSet(const ComponentDescriptor& desc);
    bool          Contains(Entity e) const;
    void*         Get(Entity e);              const void* Get(Entity e) const;   // nullptr if absent or stale; a tag yields EmptyComponentSentinel()
    void*         Insert(Entity e);           // default-construct; precondition !Contains(e)
    void*         InsertMove(Entity e, void* src);
    bool          Erase(Entity e);            // swap-and-pop; false if absent or stale
    size_t        Size() const;
    const Entity* Entities() const;           void* DataAt(size_t dense);   EntityTicks* TicksAt(size_t dense);   // DataAt: nullptr for a tag; TicksAt: nullptr unless tracked
    bool          IsDisabled(Entity e) const; bool SetDisabled(Entity e, bool disabled);   // true on change
    void          Clear();
    const ComponentDescriptor& Descriptor() const;
    + void   SetTickSource(const Tick* tick) noexcept;
    + bool   MarkWritten(Entity e, Tick now) noexcept;   // false: absent, stale or tag
    + Tick   AddedTick(Entity e) const noexcept;         // tracked: per element; untracked or absent: 0 (coarse tier = ComponentStorage::CoarseVersion)
    + Tick   ChangedTick(Entity e) const noexcept;
    + size_t PageCount() const noexcept;                 // live sparse pages
    + size_t Capacity() const noexcept;                  // dense data capacity (elements)
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/SparseSetTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <vector>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/SparseSet.hpp>
#include "../TestComponents.hpp"

using Astra::Entity;
using Astra::SparseSet;
using Astra::Tick;
using Astra::Test::Position;

namespace
{
    // See UniversalColumnTest.cpp: MakeDescriptor mints no ComponentID.
    template<typename T>
    Astra::ComponentDescriptor Desc()
    {
        return Astra::ComponentRegistry::MakeDescriptor<T>(0, nullptr);
    }

    Position* PosOf(SparseSet& set, Entity e) { return static_cast<Position*>(set.Get(e)); }
}

TEST(SparseSet, InsertGetContainsErase)
{
    SparseSet set(Desc<Position>());
    const Entity e(12, 1);
    EXPECT_FALSE(set.Contains(e));
    EXPECT_EQ(set.Get(e), nullptr);

    auto* p = static_cast<Position*>(set.Insert(e));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, Position{});                 // DefaultConstruct: value-initialised
    p->y = 4.0f;
    EXPECT_TRUE(set.Contains(e));
    EXPECT_EQ(set.Get(e), p);
    EXPECT_EQ(set.Size(), 1u);
    EXPECT_EQ(set.Entities()[0], e);
    EXPECT_EQ(set.DataAt(0), p);

    EXPECT_TRUE(set.Erase(e));
    EXPECT_FALSE(set.Erase(e));                // already gone
    EXPECT_FALSE(set.Contains(e));
    EXPECT_EQ(set.Size(), 0u);
}

TEST(SparseSet, SwapAndPopMovesTheTailIntoTheHole)
{
    SparseSet set(Desc<Position>());
    for (uint32_t id = 1; id <= 5; ++id)
    {
        Position src(static_cast<float>(id), 0.0f, 0.0f);
        (void)set.InsertMove(Entity(id, 1), &src);
    }
    ASSERT_TRUE(set.Erase(Entity(2, 1)));      // the hole at dense 1 takes the tail (id 5)

    const std::vector<Entity> dense(set.Entities(), set.Entities() + set.Size());
    EXPECT_EQ(dense, (std::vector<Entity>{Entity(1, 1), Entity(5, 1), Entity(3, 1), Entity(4, 1)}));
    for (uint32_t id : {1u, 3u, 4u, 5u})
    {
        Position* p = PosOf(set, Entity(id, 1));
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, static_cast<float>(id));   // the moved element's sparse entry was rewritten
    }
    ASSERT_TRUE(set.Erase(Entity(4, 1)));      // erasing the tail itself: a plain pop
    EXPECT_EQ(set.Size(), 3u);
    EXPECT_FLOAT_EQ(PosOf(set, Entity(3, 1))->x, 3.0f);
}

TEST(SparseSet, PagesAllocateOnFirstTouchAndReleaseWhenEmpty)
{
    SparseSet set(Desc<Position>());
    (void)set.Insert(Entity(3, 1));
    (void)set.Insert(Entity(5000, 1));
    (void)set.Insert(Entity(9000, 1));
    EXPECT_EQ(set.PageCount(), 3u);            // 4096-entry pages 0, 1 and 2
    ASSERT_TRUE(set.Erase(Entity(5000, 1)));
    EXPECT_EQ(set.PageCount(), 2u);
    EXPECT_TRUE(set.Contains(Entity(9000, 1)));
    set.Clear();
    EXPECT_EQ(set.PageCount(), 0u);
    EXPECT_EQ(set.Size(), 0u);
}

TEST(SparseSet, GrowthAndEraseRunEveryLifetimeHookExactlyOnce)
{
    using Astra::Test::Tracked;   // move-only, lifetime-counted: a skipped move ctor or dtor shows
    const int base = Tracked::s_live;
    {
        SparseSet set(Desc<Tracked>());
        for (int i = 0; i < 100; ++i)
        {
            Tracked src(i);
            (void)set.InsertMove(Entity(static_cast<uint32_t>(i), 1), &src);
        }
        EXPECT_GE(set.Capacity(), 100u);
        EXPECT_EQ(Tracked::s_live, base + 100);
        for (int i = 0; i < 100; ++i)
            EXPECT_EQ(static_cast<Tracked*>(set.Get(Entity(static_cast<uint32_t>(i), 1)))->value, i);
        for (int i = 0; i < 100; i += 2)
            ASSERT_TRUE(set.Erase(Entity(static_cast<uint32_t>(i), 1)));
        EXPECT_EQ(Tracked::s_live, base + 50);
        for (int i = 1; i < 100; i += 2)
            EXPECT_EQ(static_cast<Tracked*>(set.Get(Entity(static_cast<uint32_t>(i), 1)))->value, i);
        set.Clear();
        EXPECT_EQ(Tracked::s_live, base);
        (void)set.Insert(Entity(7, 1));
        EXPECT_EQ(Tracked::s_live, base + 1);
    }   // ~SparseSet
    EXPECT_EQ(Tracked::s_live, base);
}

TEST(SparseSet, TagStoresPresenceOnly)
{
    SparseSet set(Desc<Astra::Test::Player>());
    const Entity e(8, 1);
    EXPECT_EQ(set.Insert(e), Astra::EmptyComponentSentinel());
    EXPECT_TRUE(set.Contains(e));
    EXPECT_EQ(set.Get(e), Astra::EmptyComponentSentinel());
    EXPECT_EQ(set.DataAt(0), nullptr);         // no data buffer for an empty type
    EXPECT_EQ(set.Capacity(), 0u);
    EXPECT_FALSE(set.MarkWritten(e, 3));       // a tag has no value to write
    EXPECT_TRUE(set.Erase(e));
}

TEST(SparseSet, TicksArePerElementForTrackedAndAbsentOtherwise)
{
    Tick now = 3;
    SparseSet tracked(Desc<Astra::Test::TrackedPos>());
    tracked.SetTickSource(&now);
    const Entity a(1, 1), b(2, 1), c(3, 1);
    (void)tracked.Insert(a);
    now = 5;
    (void)tracked.Insert(b);
    (void)tracked.Insert(c);
    ASSERT_NE(tracked.TicksAt(0), nullptr);
    EXPECT_EQ(tracked.TicksAt(0)->added, 3u);
    now = 8;
    EXPECT_TRUE(tracked.MarkWritten(c, now));
    EXPECT_EQ(tracked.ChangedTick(c), 8u);
    EXPECT_EQ(tracked.ChangedTick(b), 5u);     // exact: b untouched
    ASSERT_TRUE(tracked.Erase(a));             // c (the tail) moves to dense 0 with its ticks
    EXPECT_EQ(tracked.Entities()[0], c);
    EXPECT_EQ(tracked.TicksAt(0)->changed, 8u);
    EXPECT_EQ(tracked.AddedTick(c), 5u);

    SparseSet plain(Desc<Position>());
    plain.SetTickSource(&now);
    now = 2;
    (void)plain.Insert(a);
    (void)plain.Insert(b);
    EXPECT_EQ(plain.TicksAt(0), nullptr);      // untracked: no per-element ticks
    now = 6;
    EXPECT_TRUE(plain.MarkWritten(a, now));    // present, non-tag: accepted; nothing per-element to stamp
    EXPECT_EQ(plain.ChangedTick(a), 0u);       // untracked: no store-level tier (ComponentStorage::CoarseVersion, Task 20)
    EXPECT_EQ(plain.AddedTick(b), 0u);
    EXPECT_EQ(plain.ChangedTick(Entity(1, 2)), 0u);   // stale handle: never
}

TEST(SparseSet, DisabledFlagsFollowSwapAndPop)
{
    using Astra::Test::TrackedVel;   // enableable
    SparseSet set(Desc<TrackedVel>());
    const Entity a(1, 1), b(2, 1), c(3, 1);
    (void)set.Insert(a);
    (void)set.Insert(b);
    (void)set.Insert(c);
    EXPECT_FALSE(set.IsDisabled(c));           // born enabled
    EXPECT_TRUE(set.SetDisabled(c, true));
    EXPECT_FALSE(set.SetDisabled(c, true));    // idempotent: no change
    ASSERT_TRUE(set.Erase(a));                 // c moves into dense 0...
    EXPECT_TRUE(set.IsDisabled(c));            // ...and keeps its flag
    EXPECT_FALSE(set.IsDisabled(b));
    EXPECT_TRUE(set.SetDisabled(c, false));

    SparseSet plain(Desc<Position>());
    (void)plain.Insert(a);
    EXPECT_FALSE(plain.SetDisabled(a, true));  // not enableable: nothing to store
    EXPECT_FALSE(plain.IsDisabled(a));
}

// ---- Review Focus 1 (Sparse), store level ----
// T1-C2 recycles ids at version+1. A handle to an erased or cleared occupant must
// miss on every accessor, and must never reach the element of the id's next owner.
TEST(SparseSetStaleHandle, EraseThenReinsertAtNextVersionMisses)
{
    using Astra::Test::TrackedVel;   // tracked + enableable: every per-element array exists
    SparseSet set(Desc<TrackedVel>());
    const Entity old(5, 1), fresh(5, 2);
    static_cast<TrackedVel*>(set.Insert(old))->dx = 1.0f;
    ASSERT_TRUE(set.Erase(old));
    static_cast<TrackedVel*>(set.Insert(fresh))->dx = 2.0f;

    EXPECT_FALSE(set.Contains(old));
    EXPECT_EQ(set.Get(old), nullptr);
    EXPECT_FALSE(set.Erase(old));              // must not erase the occupant
    EXPECT_FALSE(set.SetDisabled(old, true));  // must not disable the occupant
    EXPECT_FALSE(set.IsDisabled(old));
    EXPECT_FALSE(set.MarkWritten(old, 9));
    EXPECT_EQ(set.ChangedTick(old), 0u);

    ASSERT_TRUE(set.Contains(fresh));
    EXPECT_FALSE(set.IsDisabled(fresh));
    EXPECT_FLOAT_EQ(static_cast<TrackedVel*>(set.Get(fresh))->dx, 2.0f);
    EXPECT_EQ(set.Size(), 1u);
}

TEST(SparseSetStaleHandle, ClearThenReinsertAtNextVersionMisses)
{
    SparseSet set(Desc<Position>());
    const Entity old(5, 1), fresh(5, 2);
    static_cast<Position*>(set.Insert(old))->x = 1.0f;
    set.Clear();
    EXPECT_EQ(set.Get(old), nullptr);          // Clear released every page
    static_cast<Position*>(set.Insert(fresh))->x = 2.0f;
    EXPECT_EQ(set.Get(old), nullptr);
    EXPECT_FALSE(set.Contains(old));
    EXPECT_FALSE(set.Erase(old));
    EXPECT_FLOAT_EQ(PosOf(set, fresh)->x, 2.0f);
}
```

- [ ] **Step 2: Regenerate, build, and confirm the failure**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS with `fatal error C1083: Cannot open include file: 'Astra/Registry/SparseSet.hpp'`.

- [ ] **Step 3: Implement `SparseSet`**

Create `include/Astra/Registry/SparseSet.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "../Component/Component.hpp"
#include "../Core/Assert.hpp"
#include "../Core/Base.hpp"
#include "../Core/Tick.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    /**
     * Sparse-component storage (spec 2026-10-01 s4.1). There is one per Sparse
     * component per registry.
     *
     * Sparse half: pages of PAGE_ENTRIES uint32_t dense positions, indexed by
     * entity.GetID(). A page is allocated on the first insert into its range and released
     * when it empties (the EntityTable segment pattern).
     *
     * Dense half:
     *  - full Entity handles, so a stale handle misses on its version;
     *  - a stride-packed, desc.alignment-aligned data buffer, grown geometrically (none for
     *    an empty type);
     *  - EntityTicks per element, iff the type is change-tracked;
     *  - a uint8_t disabled flag per element, iff the type is enableable.
     * No coarse tick lives here: the untracked tier is ComponentStorage::CoarseVersion.
     *
     * Remove is swap-and-pop, so the set never needs compaction. The store is type-erased
     * through a descriptor held BY VALUE. Element pointers are valid until the next
     * insert or erase on this set.
     */
    class SparseSet
    {
    public:
        static constexpr std::size_t PAGE_ENTRIES = static_cast<std::size_t>(ASTRA_SPARSE_PAGE_SIZE);
        static constexpr std::uint32_t INVALID_INDEX = UINT32_MAX;
        static_assert(PAGE_ENTRIES > 0, "ASTRA_SPARSE_PAGE_SIZE must be positive");

        explicit SparseSet(const ComponentDescriptor& desc) : m_desc(desc) {}
        ~SparseSet() { Clear(); }

        SparseSet(const SparseSet&) = delete;
        SparseSet& operator=(const SparseSet&) = delete;
        SparseSet(SparseSet&&) = delete;
        SparseSet& operator=(SparseSet&&) = delete;

        void SetTickSource(const Tick* tick) noexcept { m_tick = tick; }

        ASTRA_NODISCARD bool Contains(Entity e) const noexcept { return Find(e) != INVALID_INDEX; }

        ASTRA_NODISCARD void* Get(Entity e) noexcept
        {
            const std::uint32_t d = Find(e);
            return d == INVALID_INDEX ? nullptr : ElementPtr(d);
        }

        ASTRA_NODISCARD const void* Get(Entity e) const noexcept
        {
            const std::uint32_t d = Find(e);
            return d == INVALID_INDEX ? nullptr : ElementPtr(d);
        }

        void* Insert(Entity e) { return InsertImpl(e, nullptr); }
        void* InsertMove(Entity e, void* src) { return InsertImpl(e, src); }

        // Swap-and-pop. Returns false for an absent OR stale handle, so an older version
        // of a recycled id can never erase the current occupant.
        bool Erase(Entity e)
        {
            const std::uint32_t d = Find(e);
            if (d == INVALID_INDEX) ASTRA_UNLIKELY
                return false;
            const std::size_t pageIndex = PageOf(e);
            EraseAt(d);
            if (m_sparse[pageIndex]->used == 0)
                m_sparse[pageIndex].reset();
            return true;
        }

        ASTRA_NODISCARD std::size_t Size() const noexcept { return m_dense.size(); }
        ASTRA_NODISCARD const Entity* Entities() const noexcept { return m_dense.data(); }

        // Dense-order raw access, for the sparse-driven query driver. A tag has no data:
        // nullptr, mirroring ArchetypeChunk::GetComponentArray for a column-less tag.
        ASTRA_NODISCARD void* DataAt(std::size_t dense) noexcept
        {
            ASTRA_ASSERT(dense < m_dense.size(), "SparseSet::DataAt: dense index out of range");
            return m_desc.size == 0 ? nullptr : m_data + dense * m_desc.size;
        }

        ASTRA_NODISCARD EntityTicks* TicksAt(std::size_t dense) noexcept
        {
            ASTRA_ASSERT(dense < m_dense.size(), "SparseSet::TicksAt: dense index out of range");
            return m_desc.isChangeTracked ? &m_ticks[dense] : nullptr;
        }

        ASTRA_NODISCARD bool IsDisabled(Entity e) const noexcept
        {
            if (!m_desc.isEnableable)
                return false;
            const std::uint32_t d = Find(e);
            return d != INVALID_INDEX && m_disabled[d] != 0;
        }

        // SET == DISABLED. Returns true IFF the state changed. Returns false for an
        // absent or stale handle, or a non-enableable type.
        bool SetDisabled(Entity e, bool disabled) noexcept
        {
            if (!m_desc.isEnableable) ASTRA_UNLIKELY
                return false;
            const std::uint32_t d = Find(e);
            if (d == INVALID_INDEX) ASTRA_UNLIKELY
                return false;
            const std::uint8_t want = static_cast<std::uint8_t>(disabled ? 1 : 0);
            if (m_disabled[d] == want)
                return false;
            m_disabled[d] = want;
            return true;
        }

        // Write-side stamp: a tracked element's `changed`. An untracked element is
        // accepted (true) with nothing to stamp -- its coarse tier lives in
        // ComponentStorage. Returns false for an absent or stale handle, or for a tag.
        bool MarkWritten(Entity e, Tick now) noexcept
        {
            if (m_desc.size == 0) ASTRA_UNLIKELY
                return false;
            const std::uint32_t d = Find(e);
            if (d == INVALID_INDEX) ASTRA_UNLIKELY
                return false;
            if (m_desc.isChangeTracked)
                m_ticks[d].changed = now;
            return true;
        }

        ASTRA_NODISCARD Tick AddedTick(Entity e) const noexcept
        {
            const std::uint32_t d = Find(e);
            if (d == INVALID_INDEX) return Tick{0};
            return m_desc.isChangeTracked ? m_ticks[d].added : Tick{0};
        }

        ASTRA_NODISCARD Tick ChangedTick(Entity e) const noexcept
        {
            const std::uint32_t d = Find(e);
            if (d == INVALID_INDEX) return Tick{0};
            return m_desc.isChangeTracked ? m_ticks[d].changed : Tick{0};
        }

        ASTRA_NODISCARD std::size_t PageCount() const noexcept
        {
            std::size_t n = 0;
            for (const auto& page : m_sparse)
                n += page ? 1u : 0u;
            return n;
        }

        ASTRA_NODISCARD std::size_t Capacity() const noexcept { return m_capacity; }

        // Destroys every element and releases every page and the data buffer. The set,
        // with its descriptor and tick source, stays usable (Registry::Clear keeps its
        // sparse-slot assignment, spec s7.3).
        void Clear()
        {
            if (m_desc.size > 0)
                for (std::size_t i = 0; i < m_dense.size(); ++i)
                    m_desc.Destruct(m_data + i * m_desc.size);
            m_dense.clear();
            m_ticks.clear();
            m_disabled.clear();
            m_sparse.clear();
            FreeData();
        }

        ASTRA_NODISCARD const ComponentDescriptor& Descriptor() const noexcept { return m_desc; }

    private:
        struct SparsePage
        {
            std::uint32_t index[PAGE_ENTRIES];
            std::uint32_t used = 0;
            SparsePage() { std::fill_n(index, PAGE_ENTRIES, INVALID_INDEX); }
        };

        static std::size_t PageOf(Entity e) noexcept { return static_cast<std::size_t>(e.GetID()) / PAGE_ENTRIES; }
        static std::size_t LocalOf(Entity e) noexcept { return static_cast<std::size_t>(e.GetID()) % PAGE_ENTRIES; }

        // The dense position of exactly this handle (version included), or INVALID_INDEX.
        // Every accessor funnels through here: two loads plus a handle compare.
        std::uint32_t Find(Entity e) const noexcept
        {
            if (!e.IsValid()) ASTRA_UNLIKELY
                return INVALID_INDEX;
            const std::size_t pageIndex = PageOf(e);
            if (pageIndex >= m_sparse.size() || !m_sparse[pageIndex]) ASTRA_UNLIKELY
                return INVALID_INDEX;
            const std::uint32_t d = m_sparse[pageIndex]->index[LocalOf(e)];
            return (d != INVALID_INDEX && m_dense[d] == e) ? d : INVALID_INDEX;
        }

        void* ElementPtr(std::uint32_t d) const noexcept
        {
            if (m_desc.size == 0)
                return EmptyComponentSentinel();   // present tag: non-null, never dereferenced
            return m_data + static_cast<std::size_t>(d) * m_desc.size;
        }

        void* InsertImpl(Entity e, void* src)
        {
            ASTRA_ASSERT(e.IsValid(), "SparseSet::Insert: invalid entity");
            if (!e.IsValid()) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t pageIndex = PageOf(e);
            if (pageIndex >= m_sparse.size())
                m_sparse.resize(pageIndex + 1);
            if (!m_sparse[pageIndex])
                m_sparse[pageIndex] = std::make_unique<SparsePage>();
            SparsePage& page = *m_sparse[pageIndex];
            const std::size_t local = LocalOf(e);

            if (page.index[local] != INVALID_INDEX) ASTRA_UNLIKELY
            {
                const std::uint32_t held = page.index[local];
                if (m_dense[held] == e)
                {
                    ASTRA_ASSERT(false, "SparseSet::Insert: entity already present (precondition !Contains)");
                    return ElementPtr(held);
                }
                // An older version of this id was never erased: a destroy path missed the
                // store. In Release, drop the stale element instead of orphaning it (an
                // orphan's later swap-and-pop would rewrite this id's entry).
                ASTRA_ASSERT(false, "SparseSet::Insert over a stale entry: a destroy path missed the store");
                EraseAt(held);
            }

            const std::uint32_t d = static_cast<std::uint32_t>(m_dense.size());
            Reserve(m_dense.size() + 1);
            if (m_desc.size > 0)
            {
                void* slot = m_data + static_cast<std::size_t>(d) * m_desc.size;
                if (src) m_desc.MoveConstruct(slot, src);
                else     m_desc.DefaultConstruct(slot);
            }
            m_dense.push_back(e);
            const Tick now = m_tick ? *m_tick : Tick{0};
            if (m_desc.isChangeTracked)
                m_ticks.push_back(EntityTicks{now, now});
            if (m_desc.isEnableable)
                m_disabled.push_back(std::uint8_t{0});   // born enabled
            page.index[local] = d;
            ++page.used;
            return ElementPtr(d);
        }

        // Swap-and-pop at dense position d. The tail element moves into the hole and its
        // sparse entry is rewritten. The victim's own sparse entry is cleared. Does NOT
        // release the victim's page (Erase does), so InsertImpl may call this on a page it
        // still holds a reference to.
        void EraseAt(std::uint32_t d)
        {
            const Entity victim = m_dense[d];
            const std::uint32_t last = static_cast<std::uint32_t>(m_dense.size() - 1);
            if (m_desc.size > 0)
            {
                std::byte* hole = m_data + static_cast<std::size_t>(d) * m_desc.size;
                m_desc.Destruct(hole);
                if (d != last)
                {
                    std::byte* tail = m_data + static_cast<std::size_t>(last) * m_desc.size;
                    m_desc.MoveConstruct(hole, tail);
                    m_desc.Destruct(tail);
                }
            }
            if (d != last)
            {
                const Entity moved = m_dense[last];
                m_dense[d] = moved;
                m_sparse[PageOf(moved)]->index[LocalOf(moved)] = d;
                if (m_desc.isChangeTracked) m_ticks[d] = m_ticks[last];
                if (m_desc.isEnableable)    m_disabled[d] = m_disabled[last];
            }
            m_dense.pop_back();
            if (m_desc.isChangeTracked) m_ticks.pop_back();
            if (m_desc.isEnableable)    m_disabled.pop_back();
            SparsePage& page = *m_sparse[PageOf(victim)];
            page.index[LocalOf(victim)] = INVALID_INDEX;
            --page.used;
        }

        // Geometric growth of the data buffer (none for a tag). Live elements are
        // relocated through the descriptor's move + destroy, never a blanket memcpy
        // (MoveConstruct memcpys only trivially copyable types).
        void Reserve(std::size_t want)
        {
            if (m_desc.size == 0 || want <= m_capacity)
                return;
            std::size_t cap = m_capacity ? m_capacity : 8;
            while (cap < want)
                cap *= 2;
            auto* fresh = static_cast<std::byte*>(::operator new(cap * m_desc.size, std::align_val_t{m_desc.alignment}));
            for (std::size_t i = 0; i < m_dense.size(); ++i)
            {
                m_desc.MoveConstruct(fresh + i * m_desc.size, m_data + i * m_desc.size);
                m_desc.Destruct(m_data + i * m_desc.size);
            }
            FreeData();
            m_data = fresh;
            m_capacity = cap;
        }

        void FreeData() noexcept
        {
            if (m_data)
                ::operator delete(m_data, std::align_val_t{m_desc.alignment});
            m_data = nullptr;
            m_capacity = 0;
        }

        ComponentDescriptor m_desc;
        std::vector<std::unique_ptr<SparsePage>> m_sparse;
        std::vector<Entity> m_dense;
        std::byte* m_data = nullptr;
        std::size_t m_capacity = 0;
        std::vector<EntityTicks> m_ticks;        // parallel to m_dense iff change-tracked
        std::vector<std::uint8_t> m_disabled;    // parallel to m_dense iff enableable
        const Tick* m_tick = nullptr;
    };
}
```

- [ ] **Step 4: Build and run the suites**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseSet*
```

Expected: `[  PASSED  ] 9 tests.` (`SparseSet` 7, `SparseSetStaleHandle` 2).

- [ ] **Step 5: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Registry/SparseSet.hpp tests/Registry/SparseSetTest.cpp
git commit -m "feat(storage): SparseSet -- paged sparse half (4096-entry pages, released when empty) over a dense half of full Entity handles, aligned geometric data (none for tags), ticks iff tracked, disabled flags iff enableable; swap-and-pop; stale handles miss after erase-recycle and Clear (Review Focus 1, Sparse, store level)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: The creation split -- Sparse and universal-declared pack members leave the archetype path; Sparse Get/Has; no silent drops

Spec s7.1 and s4.2 (the order when a value IS supplied), with s3.3 for the Sparse Get/Has rows and s7.3 for Sparse teardown.

`CreateEntity<Cs...>`, `CreateEntityWith`, `CreateEntities<Cs...>` and `CreateEntitiesWith<Cs...>` split their pack by kind:
- **Table members** choose the archetype. The fast path is unchanged when no member is Table-trait-but-universal.
- **Sparse-trait members** are inserted into their sets after placement. The chunk presence bit is Chunk C's.
- **Universal members:**
  - a SUPPLIED value is move-constructed into the column, and init is skipped for it;
  - a member that is only NAMED (`CreateEntity<Cs...>`) is left to `ConstructUniversals`, which default-constructs it and runs init.

`ComponentStorage` gains lazily created `SparseSet`s, each acquiring its sparse slot (the 32-slot cap). Destroy and `Clear` erase from them. Typed `GetComponent`/`HasComponent` route Sparse-trait types to the set.

`ArchetypeChunk::ConstructComponentAt`'s silent `col < 0` return becomes an assert for non-empty types.

**Review Focus 1 (Sparse), registry level**, is pinned for `Get`/`Has` (`SparseStorageStaleHandle`).

**Files:**
- Create: `include/Astra/Registry/StorageSplit.hpp`
- Modify: `include/Astra/Registry/ComponentStorage.hpp`
- Modify: `include/Astra/Registry/Registry.hpp`
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp:133-143` (`ConstructComponentAt`)
- Create: `tests/Registry/StorageCreationTest.cpp`

**Interfaces:**
- Consumes: `SparseSet` (Task 10); `StorageOfV` and `Storage` (Chunk A); `ComponentStorage::SparseSlotOf` and `AcquireSparseSlot` (Chunk A); everything from Task 9.
- Produces:
  ```cpp
  // Registry/StorageSplit.hpp
  namespace Astra::Detail {
    template<Storage S, typename... Cs> using StorageIndexSeq  = std::index_sequence<...>;   // positions in Cs... whose decayed trait is S, ascending
    template<Storage S, typename... Cs> using TypesWithStorage = std::tuple<...>;            // those decayed types, in pack order
  }
  // ComponentStorage
  SparseSet* Sparse(ComponentID id);                          // canonical: lazily created for Sparse kinds (acquires the slot), else nullptr
  + SparseSet* FindSparse(ComponentID id) const noexcept;     // never creates
  // OnEntityDestroyed / Clear / SetTickSource now also cover sparse sets
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageCreationTest.cpp`. It adds the chunk's only two new component types (TypeID budget: `AstraTest` builds with `ASTRA_MAX_COMPONENTS=192`):
- `SparseProbe` is a lifetime-counted Sparse type, which the destroy/Clear accounting needs.
- `SparseMark` is a Sparse TAG, used for the presence-only path.

Chunk A's `Astra::Test::PosS` (Sparse) and `Astra::Test::EnTag` (an enableable Table tag) provide neither. If `PosS` turns out to be the only Sparse type the parity suites need, these two can still stay local to this file.

```cpp
#include <gtest/gtest.h>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <Astra/Astra.hpp>
#include <Astra/Registry/StorageSplit.hpp>
#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

using Astra::Entity;
using Astra::Registry;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace StorageCreationTypes
{
    // A lifetime-counted Sparse component (the Tracked idiom). It is copyable, so a value
    // can also be supplied by lvalue.
    struct SparseProbe
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static inline int s_live = 0;
        int v = 0;

        SparseProbe() { ++s_live; }
        explicit SparseProbe(int x) : v(x) { ++s_live; }
        SparseProbe(const SparseProbe& o) : v(o.v) { ++s_live; }
        SparseProbe(SparseProbe&& o) noexcept : v(o.v) { ++s_live; }
        SparseProbe& operator=(const SparseProbe& o) { v = o.v; return *this; }
        SparseProbe& operator=(SparseProbe&& o) noexcept { v = o.v; return *this; }
        ~SparseProbe() { --s_live; }

        // Not trivially copyable: registration odr-uses Serialize<T> (Tracked's rationale).
        template<typename Archive>
        void Serialize(Archive& ar) { ar(v); }
    };

    // A Sparse tag: presence only, no data buffer.
    struct SparseMark
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
    };
}
using StorageCreationTypes::SparseMark;
using StorageCreationTypes::SparseProbe;

namespace
{
    int g_initCalls = 0;

    void InitPositionY(Entity, void* component, void*)
    {
        static_cast<Position*>(component)->y = -1.0f;
        ++g_initCalls;
    }

    struct AssertCount { int n = 0; };

    // Records and CONTINUES, so a failing ASTRA_ASSERT is observable in Debug without
    // aborting the test process (the CheckedAssertsTest idiom).
    Astra::AssertAction CountAndContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++static_cast<AssertCount*>(user)->n;
        return Astra::AssertAction::Continue;
    }
}

TEST(StorageSplit, PartitionsAPackByStorageTraitInPackOrder)
{
    using Astra::Storage;
    using Astra::Detail::StorageIndexSeq;
    using Astra::Detail::TypesWithStorage;
    static_assert(std::is_same_v<StorageIndexSeq<Storage::Table, Position, SparseProbe, Velocity, SparseMark>,
                                 std::index_sequence<0, 2>>);
    static_assert(std::is_same_v<StorageIndexSeq<Storage::Sparse, Position, SparseProbe, Velocity, SparseMark>,
                                 std::index_sequence<1, 3>>);
    static_assert(std::is_same_v<TypesWithStorage<Storage::Table, Position&&, const SparseProbe&, Velocity&>,
                                 std::tuple<Position, Velocity>>);
    static_assert(std::is_same_v<TypesWithStorage<Storage::Sparse>, std::tuple<>>);
    SUCCEED();
}

TEST(StorageCreation, SparseMembersGoToTheSetNotTheArchetype)
{
    Registry reg;
    const Entity a = reg.CreateEntityWith(Position{1.0f, 2.0f, 3.0f}, SparseProbe{42});
    const Entity b = reg.CreateEntityWith(Position{4.0f, 5.0f, 6.0f});
    SparseProbe lvalue{7};
    const Entity c = reg.CreateEntityWith(Position{}, lvalue);   // an lvalue is copied, not moved

    ASSERT_TRUE(reg.HasComponent<SparseProbe>(a));
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(b));
    const SparseProbe* s = std::as_const(reg).GetComponent<SparseProbe>(a);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->v, 42);
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(c)->v, 7);
    EXPECT_EQ(lvalue.v, 7);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(a)->x, 1.0f);

    // Non-fragmenting (D3): with and without the sparse member, one {Position} archetype.
    const Astra::EntityRecord* ra = reg.GetArchetypeManager()->GetEntityRecord(a);
    const Astra::EntityRecord* rb = reg.GetArchetypeManager()->GetEntityRecord(b);
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(ra->archetype, rb->archetype);
    EXPECT_FALSE(ra->archetype->GetMask().Test(Astra::TypeID<SparseProbe>::Value()));
}

TEST(StorageCreation, CreateEntityDefaultConstructsSparseMembersAndTags)
{
    Registry reg;
    const Entity e = reg.CreateEntity<Velocity, SparseProbe, SparseMark>();
    EXPECT_TRUE(reg.HasComponent<Velocity>(e));
    ASSERT_TRUE(reg.HasComponent<SparseProbe>(e));
    EXPECT_TRUE(reg.HasComponent<SparseMark>(e));
    EXPECT_NE(std::as_const(reg).GetComponent<SparseMark>(e), nullptr);   // a present tag is non-null
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(e)->v, 0);

    const Entity bare = reg.CreateEntity<Velocity>();
    EXPECT_FALSE(reg.HasComponent<SparseMark>(bare));
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseMark>(bare), nullptr);
}

TEST(StorageCreation, BatchCreatorsSplitToo)
{
    Registry reg;
    std::vector<Entity> ents(8);
    ASSERT_EQ((reg.CreateEntities<Position, SparseProbe>(8, std::span{ents})), 8u);
    for (Entity e : ents)
    {
        EXPECT_TRUE(reg.HasComponent<Position>(e));
        EXPECT_TRUE(reg.HasComponent<SparseProbe>(e));
    }

    std::vector<size_t> calls;
    ASSERT_EQ((reg.CreateEntitiesWith<Position, SparseProbe>(8, std::span{ents}, [&](size_t i)
    {
        calls.push_back(i);
        return std::tuple{Position{static_cast<float>(i), 0.0f, 0.0f}, SparseProbe{static_cast<int>(i)}};
    })), 8u);
    EXPECT_EQ(calls, (std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7}));   // exactly once each, in order
    for (size_t i = 0; i < ents.size(); ++i)
    {
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(ents[i])->x, static_cast<float>(i));
        EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(ents[i])->v, static_cast<int>(i));
    }
}

TEST(StorageCreation, SuppliedUniversalIsMovedInAndNeverInitialised)
{
    g_initCalls = 0;
    Registry::Config cfg;
    cfg.AddUniversal<Position>(&InitPositionY);
    Registry reg(cfg);

    const Entity supplied = reg.CreateEntityWith(Position{9.0f, 9.0f, 9.0f}, Velocity{});
    const Position* p = std::as_const(reg).GetComponent<Position>(supplied);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->y, 9.0f);               // s4.2: a supplied value is never overwritten by init
    EXPECT_EQ(g_initCalls, 0);
    const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(supplied);
    ASSERT_NE(rec, nullptr);
    EXPECT_TRUE(rec->archetype->GetMask().Test(Astra::TypeID<Velocity>::Value()));
    EXPECT_FALSE(rec->archetype->GetMask().Test(Astra::TypeID<Position>::Value()));

    Position lvalue(3.0f, 3.0f, 3.0f);
    const Entity copied = reg.CreateEntityWith(lvalue);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(copied)->y, 3.0f);
    EXPECT_FLOAT_EQ(lvalue.y, 3.0f);
    EXPECT_EQ(g_initCalls, 0);

    const Entity named = reg.CreateEntity<Position, Velocity>();   // named, no value: default + init
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(named)->y, -1.0f);
    EXPECT_EQ(g_initCalls, 1);

    std::vector<Entity> ents(3);
    ASSERT_EQ((reg.CreateEntitiesWith<Position>(3, std::span{ents},
        [](size_t i) { return std::tuple{Position{0.0f, static_cast<float>(i), 0.0f}}; })), 3u);
    for (size_t i = 0; i < 3; ++i)
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(ents[i])->y, static_cast<float>(i));
    EXPECT_EQ(g_initCalls, 1);                 // supplied by the generator: no init
    ASSERT_EQ((reg.CreateEntities<Position>(3, std::span{ents})), 3u);
    EXPECT_EQ(g_initCalls, 4);                 // named only: init for each
}

TEST(StorageCreation, UniversalOverridesTheSparseTrait)
{
    Registry::Config cfg;
    cfg.AddUniversal<SparseProbe>();
    Registry reg(cfg);
    const Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.HasComponent<SparseProbe>(e));        // universal: present on every entity
    const Entity withValue = reg.CreateEntityWith(SparseProbe{5});
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(withValue)->v, 5);
}

TEST(StorageCreation, ComponentAddedCoversSparseMembersInPackOrder)
{
    Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    std::vector<Astra::ComponentID> ids;
    int seenValue = -1;
    auto* signals = reg.GetSignalManager();
    auto handle = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev)
        {
            ids.push_back(ev.componentId);
            if (ev.componentId == Astra::TypeID<SparseProbe>::Value())
                seenValue = static_cast<const SparseProbe*>(ev.component)->v;
        });
    (void)reg.CreateEntityWith(Position{}, SparseProbe{5});
    EXPECT_EQ(ids, (std::vector<Astra::ComponentID>{Astra::TypeID<Position>::Value(),
                                                    Astra::TypeID<SparseProbe>::Value()}));
    EXPECT_EQ(seenValue, 5);
    signals->On<Astra::Events::ComponentAdded>().Unregister(handle);
}

TEST(StorageCreation, DestroyClearAndRegistryDestructionDestroySparseValues)
{
    const int base = SparseProbe::s_live;
    {
        Registry reg;
        std::vector<Entity> ents(4);
        ASSERT_EQ((reg.CreateEntities<Position, SparseProbe>(4, std::span{ents})), 4u);
        EXPECT_EQ(SparseProbe::s_live, base + 4);
        reg.DestroyEntity(ents[0]);
        EXPECT_EQ(SparseProbe::s_live, base + 3);
        reg.DestroyEntities(std::span{ents}.subspan(1, 2));
        EXPECT_EQ(SparseProbe::s_live, base + 1);
        reg.Clear();
        EXPECT_EQ(SparseProbe::s_live, base);
        (void)reg.CreateEntity<SparseProbe>();
        EXPECT_EQ(SparseProbe::s_live, base + 1);
    }   // ~Registry
    EXPECT_EQ(SparseProbe::s_live, base);
}

// s7.1 "no silent drops": a value with no column reaching ConstructComponentAt is a bug
// (the Registry diverts Sparse/Universal values first), so it must be loud.
TEST(StorageCreation, ConstructComponentAtRefusesAColumnlessValueLoudly)
{
    Astra::ComponentRegistry components;
    components.RegisterComponents<Position, Velocity>();
    Astra::ArchetypeChunkPool pool;
    Astra::Archetype archetype(Astra::MakeComponentMask<Position>());
    archetype.SetComponentPool(&pool);
    archetype.Initialize({*components.GetComponentDescriptor(Astra::TypeID<Position>::Value())});

    AssertCount count;
    Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
    const auto location = archetype.AddEntityWith(Entity(1, 1), Position{1.0f, 0.0f, 0.0f}, Velocity{});
    EXPECT_TRUE(location.IsValid());
#if MOSAIC_ASSERTS_ACTIVE
    EXPECT_EQ(count.n, 1);   // Velocity has no column in {Position}: refused loudly
#else
    EXPECT_EQ(count.n, 0);   // Release: compiled out; the value is still written nowhere
#endif
}

// ---- Review Focus 1 (Sparse), registry level: Get/Has ----
// Chunk C extends this suite with Add, Remove and SetEnabled once those route to the set.

TEST(SparseStorageStaleHandle, DestroyRecycleMissesOnGetAndHas)
{
    Registry reg;
    const Entity stale = reg.CreateEntityWith(SparseProbe{7});
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntityWith(SparseProbe{8});
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(stale));
    const SparseProbe* now = std::as_const(reg).GetComponent<SparseProbe>(fresh);
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(now->v, 8);
}

TEST(SparseStorageStaleHandle, RecycledOccupantWithoutTheComponentStillMisses)
{
    // The id's next owner does NOT carry the sparse component: neither handle may see
    // the erased element.
    Registry reg;
    const Entity stale = reg.CreateEntityWith(SparseProbe{7});
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity<Velocity>();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(stale));
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(fresh));
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(fresh), nullptr);
}

TEST(SparseStorageStaleHandle, ClearRecycleMissesOnGetAndHas)
{
    Registry reg;
    const Entity stale = reg.CreateEntityWith(SparseProbe{7});
    reg.Clear();
    const Entity fresh = reg.CreateEntityWith(SparseProbe{8});
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(stale));
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(fresh)->v, 8);
}
```

- [ ] **Step 2: Regenerate, build, and confirm the failure**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS with `fatal error C1083: Cannot open include file: 'Astra/Registry/StorageSplit.hpp'`.

- [ ] **Step 3: The pack-split helpers**

Create `include/Astra/Registry/StorageSplit.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../Component/Component.hpp"

namespace Astra::Detail
{
    // Compile-time partition of a component pack by its declared storage TRAIT (spec
    // 2026-10-01 s7.1). This is pure type-level recursion: no constexpr arrays, so an
    // empty partition needs no zero-length array. Universal is a per-registry runtime
    // fact, and the Registry resolves it per member.

    template<Storage S, std::size_t I, typename Out, typename... Cs>
    struct StorageIndexSeqImpl
    {
        using type = Out;
    };

    template<Storage S, std::size_t I, std::size_t... Out, typename C, typename... Rest>
    struct StorageIndexSeqImpl<S, I, std::index_sequence<Out...>, C, Rest...>
    {
        using type = typename StorageIndexSeqImpl<S, I + 1,
            std::conditional_t<StorageOfV<std::decay_t<C>> == S,
                               std::index_sequence<Out..., I>,
                               std::index_sequence<Out...>>,
            Rest...>::type;
    };

    // Positions (into Cs...) of the members whose decayed trait is S, ascending.
    template<Storage S, typename... Cs>
    using StorageIndexSeq = typename StorageIndexSeqImpl<S, 0, std::index_sequence<>, Cs...>::type;

    template<typename Tuple, typename Seq>
    struct SelectDecayed;

    template<typename Tuple, std::size_t... I>
    struct SelectDecayed<Tuple, std::index_sequence<I...>>
    {
        using type = std::tuple<std::decay_t<std::tuple_element_t<I, Tuple>>...>;
    };

    // The members whose decayed trait is S, decayed, in pack order, as a std::tuple type
    // (consumed through a std::type_identity<std::tuple<Ts...>> template lambda).
    template<Storage S, typename... Cs>
    using TypesWithStorage = typename SelectDecayed<std::tuple<Cs...>, StorageIndexSeq<S, Cs...>>::type;
}
```

`StorageOfV<std::decay_t<C>>` assumes Chunk A's `StorageOfV` already strips cv. Decaying first keeps references out of the trait.

- [ ] **Step 4: `ComponentStorage` owns the sparse sets**

In `include/Astra/Registry/ComponentStorage.hpp`, add `#include "SparseSet.hpp"`. Then add to the public section:

```cpp
        // ================= Sparse sets (spec 2026-10-01 s4.1) =================

        ASTRA_NODISCARD SparseSet* FindSparse(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS ? m_sparseSets[id].get() : nullptr;
        }

        // The set for a Sparse-kind id. It is created on first use, which acquires the
        // id's sparse slot (spec s3.2). Returns nullptr for any other kind, for an
        // unregistered id, and once ASTRA_MAX_SPARSE_COMPONENTS slots are taken
        // (AcquireSparseSlot asserts in Debug, Chunk A). The caller then refuses the
        // component, so Release never silently writes it elsewhere.
        SparseSet* Sparse(ComponentID id)
        {
            if (id >= MAX_COMPONENTS || KindOf(id) != StorageKind::Sparse) ASTRA_UNLIKELY
                return nullptr;
            std::unique_ptr<SparseSet>& owned = m_sparseSets[id];
            if (!owned) ASTRA_UNLIKELY
            {
                const ComponentDescriptor* desc = m_registry ? m_registry->GetComponentDescriptor(id) : nullptr;
                if (!desc) ASTRA_UNLIKELY
                    return nullptr;
                if (SparseSlotOf(id) < 0 && AcquireSparseSlot(id) < 0) ASTRA_UNLIKELY
                    return nullptr;
                owned = std::make_unique<SparseSet>(*desc);
                owned->SetTickSource(m_tickSource);
                m_sparseIds.push_back(id);
                ++m_storeCount;
            }
            return owned.get();
        }
```

Extend the three Task 9 members.

In `SetTickSource`, after the universal loop:

```cpp
            for (ComponentID id : m_sparseIds)
                m_sparseSets[id]->SetTickSource(tick);
```

In `OnEntityDestroyed`, after the universal loop:

```cpp
            // Interim: erase from every existing set (O(sets)). Chunk C's destroy-walk
            // task replaces this loop with the walk over `chunk`'s presence blocks at
            // `index` (spec s7.3).
            for (ComponentID id : m_sparseIds)
                (void)m_sparseSets[id]->Erase(e);
```

In `Clear`, after the universal loop:

```cpp
            for (ComponentID id : m_sparseIds)
                m_sparseSets[id]->Clear();   // the set and its slot survive (spec s7.3)
```

Add to the private section:

```cpp
        std::array<std::unique_ptr<SparseSet>, MAX_COMPONENTS> m_sparseSets{};
        std::vector<ComponentID> m_sparseIds;   // ids with a live set, in creation order
```

- [ ] **Step 5: No silent drops in `ConstructComponentAt`**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, replace the head of `ConstructComponentAt` (around lines 133-143):

```cpp
        template<typename T>
        void ConstructComponentAt(size_t index, T&& value)
        {
            ComponentID id = TypeID<std::decay_t<T>>::Value();
            const int col = m_meta->idToColumn[id];

            if (col < 0) ASTRA_UNLIKELY
            {
                return;  // absent or tag: no storage
            }
```

with:

```cpp
        template<typename T>
        void ConstructComponentAt(size_t index, T&& value)
        {
            if constexpr (std::is_empty_v<std::decay_t<T>>)
            {
                return;   // a tag carries no value (and an enableable Table tag's column is bit-only)
            }
            ComponentID id = TypeID<std::decay_t<T>>::Value();
            const int col = m_meta->idToColumn[id];

            if (col < 0) ASTRA_UNLIKELY
            {
                // Spec 2026-10-01 s7.1, "no silent drops": the Registry diverts every
                // Sparse/Universal value before archetype placement, so a non-empty value
                // without a column here is a routing bug. Make it loud.
                ASTRA_ASSERT(false, "ConstructComponentAt: component has no column in this archetype");
                return;
            }
```

The rest of the function is unchanged. If the `if constexpr` early return triggers an unreachable-code warning (C4702) under `/WX`, move the remaining body into an `else` branch of the `if constexpr`.

- [ ] **Step 6: Registry -- the four creators split by kind; Sparse routing**

In `include/Astra/Registry/Registry.hpp`, add `#include <type_traits>` and `#include "StorageSplit.hpp"` beside the existing includes.

(a) Replace the four creators (`CreateEntity`, `CreateEntityWith`, `CreateEntities` and `CreateEntitiesWith`, as Task 9 left them) with:

```cpp
        template<Component... Components>
        Entity CreateEntity()
        {
            Entity entity = m_entityManager.Create();
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();

            if constexpr (sizeof...(Components) == 0)
            {
                return CreateEntityInternal(entity);
            }
            else
            {
                // Spec s7.1: Table members choose the archetype, Sparse members go to their
                // sets, and a NAMED Universal member is left to ConstructUniversals (no
                // value was passed, so default + init).
                PlaceDefault<Components...>(entity);
                ConstructUniversalsFor(entity, nullptr, 0);
                m_signalManager.Emit<Events::EntityCreated>(entity);
                EmitPackAdded<Components...>(entity);
                EmitUniversalAdded(entity);
                return entity;
            }
        }

        template<Component... Components>
        Entity CreateEntityWith(Components&&... components)
        {
            static_assert(sizeof...(Components) > 0, "CreateEntityWith requires at least one component");
            AssertContextAffinity();
            Entity entity = m_entityManager.Create();
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();

            // Universal members the caller supplied: moved in by PlaceWith, so they skip
            // default + init (spec s4.2: an explicitly supplied value is never overwritten).
            ComponentID supplied[sizeof...(Components)]{};
            size_t suppliedCount = 0;
            PlaceWith(entity, supplied, suppliedCount, std::forward<Components>(components)...);
            ConstructUniversalsFor(entity, supplied, suppliedCount);

            m_signalManager.Emit<Events::EntityCreated>(entity);
            EmitPackAdded<std::decay_t<Components>...>(entity);
            EmitUniversalAdded(entity);
            return entity;
        }

        template<Component... Components>
        size_t CreateEntities(size_t count, std::span<Entity> outEntities)
        {
            if (count == 0 || outEntities.size() < count)
                return 0;

            size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0) ASTRA_UNLIKELY
                return 0;

            const std::span<Entity> made = outEntities.subspan(0, created);
            PlaceDefaultBatch<Components...>(made);
            if (m_componentStorage->HasUniversals()) ASTRA_UNLIKELY
            {
                for (Entity e : made)
                    m_componentStorage->ConstructUniversals(e, nullptr, 0);
            }
            EmitBatchCreated<Components...>(made);
            return created;
        }

        template<Component... Components, std::invocable<size_t> Generator>
        size_t CreateEntitiesWith(size_t count, std::span<Entity> outEntities, Generator&& generator)
        {
            if (count == 0 || outEntities.size() < count)
                return 0;

            size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0) ASTRA_UNLIKELY
                return 0;

            const std::span<Entity> made = outEntities.subspan(0, created);
            constexpr bool allTableTrait = ((StorageOfV<Components> == Storage::Table) && ...);
            if (allTableTrait && !AnyUniversal<Components...>()) ASTRA_LIKELY
            {
                // Today's batched chunk-run path, unchanged (G4).
                m_archetypeManager->AddEntitiesWith<Components...>(made, std::forward<Generator>(generator));
                if (m_componentStorage->HasUniversals()) ASTRA_UNLIKELY
                {
                    for (Entity e : made)
                        m_componentStorage->ConstructUniversals(e, nullptr, 0);
                }
            }
            else
            {
                // Mixed storage: one entity at a time, calling the generator exactly once
                // per index, in order (the Archetype::AddEntitiesWith contract).
                for (size_t i = 0; i < created; ++i)
                {
                    ComponentID supplied[sizeof...(Components) > 0 ? sizeof...(Components) : 1]{};
                    size_t suppliedCount = 0;
                    std::apply([&](auto&&... values)
                    {
                        PlaceWith(made[i], supplied, suppliedCount, std::forward<decltype(values)>(values)...);
                    }, generator(i));
                    ConstructUniversalsFor(made[i], supplied, suppliedCount);
                }
            }
            EmitBatchCreated<Components...>(made);
            return created;
        }
```

(b) Task 9 added two ConstructUniversals loops and the trailing `EmitUniversalAdded` loops to `CreateEntities` and `CreateEntitiesWith`. The replacements above already contain them, through the `HasUniversals` branch and `EmitBatchCreated`. Make sure no Task 9 copy survives.

(c) Replace Task 9's `RoutesToStore` and `StoreLookup` with:

```cpp
        // True when T's value lives in a ComponentStorage store rather than a chunk column.
        // A Sparse-trait T always routes to a store: its set, or its universal column when
        // this registry declares it universal. That is decided at compile time. A
        // Table-trait T routes to a store only when declared universal: one predictable
        // bool load in a registry with no universal (G4).
        template<Component T>
        ASTRA_NODISCARD bool RoutesToStore() const noexcept
        {
            if constexpr (StorageOfV<T> == Storage::Sparse)
                return true;
            else
                return m_componentStorage->HasUniversals() && KindOfT<T>() == StorageKind::Universal;
        }

        ASTRA_NODISCARD void* StoreLookup(Entity entity, ComponentID id) const noexcept
        {
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Universal:
                {
                    UniversalColumn* column = m_componentStorage->FindUniversal(id);
                    return column ? column->Get(entity) : nullptr;
                }
                case StorageKind::Sparse:
                {
                    SparseSet* set = m_componentStorage->FindSparse(id);
                    return set ? set->Get(entity) : nullptr;
                }
                default:
                    return nullptr;
            }
        }
```

(d) Add the placement and signal helpers to the `private:` section, after `CreateEntityInternal`:

```cpp
        // True if any of Ts is declared universal in this registry. Table-trait members
        // only reach the archetype in one piece when this is false.
        template<Component... Ts>
        ASTRA_NODISCARD bool AnyUniversal() const noexcept
        {
            if constexpr (sizeof...(Ts) == 0)
                return false;
            else
                return m_componentStorage->HasUniversals() && ((KindOfT<Ts>() == StorageKind::Universal) || ...);
        }

        // One default-constructed (named, no value) member, by its runtime kind. Used for
        // Sparse-trait members always, and for Table-trait members only on the rare path
        // where a pack names a universal type.
        template<Component C>
        void PlaceDefaultMember(Entity entity)
        {
            m_componentRegistry->RegisterComponent<C>();   // KindOf needs the descriptor (Sparse trait)
            const ComponentID id = TypeID<C>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return;
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Table:
                    (void)m_archetypeManager->AddComponent<C>(entity);
                    break;
                case StorageKind::Sparse:
                    if (SparseSet* set = m_componentStorage->Sparse(id); set && !set->Contains(entity)) ASTRA_LIKELY
                        (void)set->Insert(entity);
                    break;
                case StorageKind::Universal:
                    break;   // ConstructUniversals builds it: named only, so default + init
            }
        }

        template<Component... Components>
        void PlaceDefault(Entity entity)
        {
            [&]<typename... Ts>(std::type_identity<std::tuple<Ts...>>)
            {
                if (!AnyUniversal<Ts...>()) ASTRA_LIKELY
                {
                    m_archetypeManager->AddEntity<Ts...>(entity);   // today's path
                }
                else
                {
                    m_archetypeManager->AddEntity<>(entity);        // root, then one member at a time
                    (PlaceDefaultMember<Ts>(entity), ...);
                }
            }(std::type_identity<Detail::TypesWithStorage<Storage::Table, Components...>>{});

            [&]<typename... Ss>(std::type_identity<std::tuple<Ss...>>)
            {
                (PlaceDefaultMember<Ss>(entity), ...);
            }(std::type_identity<Detail::TypesWithStorage<Storage::Sparse, Components...>>{});
        }

        template<Component... Components>
        void PlaceDefaultBatch(std::span<Entity> entities)
        {
            [&]<typename... Ts>(std::type_identity<std::tuple<Ts...>>)
            {
                if (!AnyUniversal<Ts...>()) ASTRA_LIKELY
                {
                    m_archetypeManager->AddEntities<Ts...>(entities);   // today's batched path
                }
                else
                {
                    m_archetypeManager->AddEntities<>(entities);
                    for (Entity e : entities)
                    {
                        (PlaceDefaultMember<Ts>(e), ...);
                    }
                }
            }(std::type_identity<Detail::TypesWithStorage<Storage::Table, Components...>>{});

            [&]<typename... Ss>(std::type_identity<std::tuple<Ss...>>)
            {
                if constexpr (sizeof...(Ss) > 0)
                {
                    for (Entity e : entities)
                    {
                        (PlaceDefaultMember<Ss>(e), ...);
                    }
                }
            }(std::type_identity<Detail::TypesWithStorage<Storage::Sparse, Components...>>{});
        }
```

```cpp
        // One SUPPLIED member, by its runtime kind. An rvalue is moved straight from the
        // caller's object; an lvalue is copied through a temporary, as ArchetypeChunk's
        // ConstructComponentAt does. Universal members are recorded in `supplied`, so
        // ConstructUniversals skips their default + init.
        template<typename V>
        void PlaceValueMember(Entity entity, ComponentID* supplied, size_t& suppliedCount, V&& value)
        {
            using C = std::decay_t<V>;
            m_componentRegistry->RegisterComponent<C>();
            const ComponentID id = TypeID<C>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return;

            const auto moveInto = [&](auto&& sink)
            {
                if constexpr (std::is_rvalue_reference_v<V&&> && !std::is_const_v<std::remove_reference_t<V>>)
                {
                    (void)sink(static_cast<void*>(std::addressof(value)));
                }
                else
                {
                    C temp(value);
                    (void)sink(static_cast<void*>(std::addressof(temp)));
                }
            };

            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Table:
                    (void)m_archetypeManager->AddComponent<C>(entity, std::forward<V>(value));
                    break;
                case StorageKind::Sparse:
                    if (SparseSet* set = m_componentStorage->Sparse(id); set && !set->Contains(entity)) ASTRA_LIKELY
                        moveInto([&](void* src) { return set->InsertMove(entity, src); });
                    break;
                case StorageKind::Universal:
                    if (UniversalColumn* column = m_componentStorage->Universal(id); column && !column->IsLive(entity)) ASTRA_LIKELY
                    {
                        moveInto([&](void* src) { return column->ConstructMove(entity, src); });
                        supplied[suppliedCount++] = id;
                    }
                    break;
            }
        }

        // Places a value pack (spec s7.1). Table-trait members go to the archetype in ONE
        // placement (today's AddEntityWith) unless one of them is declared universal. In
        // that rare case: the root first, then member by member. Sparse-trait members go to
        // their sets afterwards. The forwarding tuple carries every value exactly once.
        template<typename... Vs>
        void PlaceWith(Entity entity, ComponentID* supplied, size_t& suppliedCount, Vs&&... values)
        {
            auto refs = std::forward_as_tuple(std::forward<Vs>(values)...);
            using Refs = decltype(refs);

            [&]<std::size_t... I>(std::index_sequence<I...>)
            {
                if constexpr (sizeof...(I) == 0)
                {
                    m_archetypeManager->AddEntity<>(entity);
                }
                else
                {
                    if (!AnyUniversal<std::decay_t<std::tuple_element_t<I, Refs>>...>()) ASTRA_LIKELY
                    {
                        m_archetypeManager->AddEntityWith(entity, std::forward<std::tuple_element_t<I, Refs>>(std::get<I>(refs))...);
                    }
                    else
                    {
                        m_archetypeManager->AddEntity<>(entity);
                        (PlaceValueMember(entity, supplied, suppliedCount, std::forward<std::tuple_element_t<I, Refs>>(std::get<I>(refs))), ...);
                    }
                }
            }(Detail::StorageIndexSeq<Storage::Table, Vs...>{});

            [&]<std::size_t... I>(std::index_sequence<I...>)
            {
                (PlaceValueMember(entity, supplied, suppliedCount, std::forward<std::tuple_element_t<I, Refs>>(std::get<I>(refs))), ...);
            }(Detail::StorageIndexSeq<Storage::Sparse, Vs...>{});
        }

        // ComponentAdded for one pack member, by its runtime kind (Table: the chunk slot,
        // today's payload; Sparse: the set element, or the per-type static for a tag).
        // Universal members are skipped here because EmitUniversalAdded covers each
        // universal exactly once.
        template<Component C>
        void EmitAddedFor(Entity entity)
        {
            const ComponentID id = TypeID<C>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return;
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Table:
                    if (const EntityRecord* record = m_archetypeManager->GetEntityRecord(entity))
                        m_signalManager.Emit<Events::ComponentAdded>(entity, id, record->archetype->GetComponent<C>(record->location));
                    break;
                case StorageKind::Sparse:
                    if (C* component = StoreGet<C>(entity))
                        m_signalManager.Emit<Events::ComponentAdded>(entity, id, component);
                    break;
                case StorageKind::Universal:
                    break;
            }
        }

        template<Component... Components>
        void EmitPackAdded(Entity entity)
        {
            if constexpr (sizeof...(Components) > 0)
            {
                if (!m_signalManager.IsSignalEnabled(Signal::ComponentAdded)) ASTRA_LIKELY
                    return;
                (EmitAddedFor<Components>(entity), ...);
            }
        }

        // Batch twin of the single-entity tail: every EntityCreated first, then per entity
        // the pack's ComponentAdded followed by the universals' (today's batch order).
        template<Component... Components>
        void EmitBatchCreated(std::span<Entity> made)
        {
            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (Entity e : made)
                    m_signalManager.Emit<Events::EntityCreated>(e);
            }
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (Entity e : made)
                {
                    EmitPackAdded<Components...>(e);
                    EmitUniversalAdded(e);
                }
            }
        }
```

`StoreGet`, `KindOfT`, `ConstructUniversalsFor`, `EmitUniversalAdded` and `CreateEntityInternal` are Task 9's and stay as they are.

- [ ] **Step 7: Build and run the suites**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageSplit.*:StorageCreation.*:SparseStorageStaleHandle.*:UniversalStorage*:RegistryTest.*:ArchetypeManagerTest.*:CommandBuffer*:EmptyTag*
```

Expected: every test passes. That covers 12 new tests (`StorageSplit` 1, `StorageCreation` 8, `SparseStorageStaleHandle` 3), the Task 9 suites, and the existing Registry/ArchetypeManager/CommandBuffer/tag suites. The all-Table creation paths are unchanged.

- [ ] **Step 8: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 9: Commit**

```powershell
git add include/Astra/Registry/StorageSplit.hpp include/Astra/Registry/ComponentStorage.hpp include/Astra/Registry/Registry.hpp include/Astra/Archetype/ArchetypeChunkPool.hpp tests/Registry/StorageCreationTest.cpp
git commit -m "feat(storage): creation splits packs by kind -- Table members choose the archetype (unchanged fast path), Sparse members go to lazily created SparseSets (slot acquired, 32 cap), supplied Universal values are moved in and skip init, named-only ones get default + init; Sparse Get/Has route to the set; destroy/Clear erase sparse values; ConstructComponentAt asserts on a column-less value (no silent drops); Sparse stale handles miss at the registry (Review Focus 1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: Universal `Add` is a no-op returning `true`; Universal `Remove` is refused

Spec s3.3, s12.2 and s12.3 (D2's single exception).

**Typed `AddComponent`/`EmplaceComponent` on a live entity** returns `true` and does nothing: no value change and no signal. This is today's rule for a present Table component (`ArchetypeManager.hpp:314-315` + `Registry.hpp:337-368`).

**By-id `AddComponentByID`** returns `false`. This mirrors the by-id Table rule for an already-present component (`ArchetypeManager::AddComponentByID`, `if (mask.Test(id)) return false`), so D2 parity is with what each surface does today.

**Batch adds** are no-ops: `AddComponentsByID` returns 0.

**Every remove form** (typed, batch, by-id, by-id batch, and therefore the CommandBuffer executors) on a live entity:
- calls `ASTRA_ASSERT` in Debug, once per call;
- returns `false` / `0`;
- emits no `ComponentRemoved`.

**A stale handle misses quietly:** `false`, no assert (Review Focus 1, `Add`/`Remove` arms).

Tests observe the assert through a counting `ScopedAssertHandler` that returns `Continue`, so they pass in both configurations (`MOSAIC_ASSERTS_ACTIVE`).

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp`
- Modify: `tests/Registry/UniversalStorageTest.cpp` (append)

**Interfaces:**
- Consumes: Task 9's `KindOfT`, `m_componentStorage->KindOf`, `StoreGet`.
- Produces (Registry, private): `static bool RefuseUniversalRemove() noexcept;` (asserts and returns `false`). No public signature changes.

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/UniversalStorageTest.cpp`:

```cpp
namespace
{
    struct AssertCount { int n = 0; };

    Astra::AssertAction CountAndContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++static_cast<AssertCount*>(user)->n;
        return Astra::AssertAction::Continue;
    }

#if MOSAIC_ASSERTS_ACTIVE
    constexpr int kAssertsPerRefusal = 1;   // Debug (or a checked build): the refusal asserts
#else
    constexpr int kAssertsPerRefusal = 0;   // Release: compiled out, returns false
#endif
}

TEST(UniversalStorage, AddOnAPresentUniversalIsANoOpReturningTrue)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity e = reg.CreateEntity();
    reg.GetComponent<Position>(e)->x = 4.0f;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int added = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded&) { ++added; });

    EXPECT_TRUE(reg.AddComponent<Position>(e, Position{9.0f, 9.0f, 9.0f}));
    EXPECT_TRUE(reg.EmplaceComponent<Position>(e, 8.0f, 8.0f, 8.0f));
    std::vector<Entity> one{e};
    reg.AddComponents<Position>(std::span{one}, Position{7.0f, 7.0f, 7.0f});
    reg.EmplaceComponents<Position>(std::span{one}, 6.0f, 6.0f, 6.0f);
    const Position value(5.0f, 5.0f, 5.0f);
    EXPECT_FALSE(reg.AddComponentByID(e, Astra::TypeID<Position>::Value(), &value, sizeof(Position)));   // by-id Table rule for "present"
    EXPECT_EQ(reg.AddComponentsByID(std::span{one}, Astra::TypeID<Position>::Value(), &value, sizeof(Position)), 0u);

    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 4.0f);   // untouched
    EXPECT_EQ(added, 0);                                                     // no signal
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(UniversalStorage, RemoveIsRefusedOnEveryFormWithoutASignal)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity e = reg.CreateEntity();
    reg.GetComponent<Position>(e)->x = 4.0f;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);
    int removed = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved&) { ++removed; });

    AssertCount count;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        std::vector<Entity> one{e};
        EXPECT_FALSE(reg.RemoveComponent<Position>(e));
        EXPECT_EQ(reg.RemoveComponents<Position>(std::span{one}), 0u);
        EXPECT_FALSE(reg.RemoveComponentByID(e, Astra::TypeID<Position>::Value()));
        EXPECT_EQ(reg.RemoveComponentsByID(std::span{one}, Astra::TypeID<Position>::Value()), 0u);
    }
    EXPECT_EQ(count.n, 4 * kAssertsPerRefusal);   // one refusal per call
    EXPECT_EQ(removed, 0);
    ASSERT_TRUE(reg.HasComponent<Position>(e));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 4.0f);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(h);
}

TEST(UniversalStorage, CommandBufferRemoveOfAUniversalIsRefusedAtExecute)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity e = reg.CreateEntity();
    AssertCount count;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        Astra::CommandBuffer cb(&reg);
        cb.RemoveComponent<Position>(e);
        (void)cb.Execute();   // the executor reports the refusal as a failed command
    }
    EXPECT_EQ(count.n, kAssertsPerRefusal);
    EXPECT_TRUE(reg.HasComponent<Position>(e));
}

// ---- Review Focus 1 (Universal), Add/Remove arms ----
TEST(UniversalStorageStaleHandle, AddAndRemoveMissQuietlyAndNeverTouchTheOccupant)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity stale = reg.CreateEntity();
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    reg.GetComponent<Position>(fresh)->x = 3.0f;

    AssertCount count;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        std::vector<Entity> staleOnly{stale};
        const Position value(9.0f, 9.0f, 9.0f);
        EXPECT_FALSE(reg.AddComponent<Position>(stale, value));
        EXPECT_FALSE(reg.EmplaceComponent<Position>(stale, 9.0f, 9.0f, 9.0f));
        EXPECT_FALSE(reg.AddComponentByID(stale, Astra::TypeID<Position>::Value(), &value, sizeof(Position)));
        EXPECT_FALSE(reg.RemoveComponent<Position>(stale));
        EXPECT_EQ(reg.RemoveComponents<Position>(std::span{staleOnly}), 0u);
        EXPECT_FALSE(reg.RemoveComponentByID(stale, Astra::TypeID<Position>::Value()));
        EXPECT_EQ(reg.RemoveComponentsByID(std::span{staleOnly}, Astra::TypeID<Position>::Value()), 0u);
    }
    EXPECT_EQ(count.n, 0);                     // a stale handle is a miss, not a refusal
    ASSERT_TRUE(reg.HasComponent<Position>(fresh));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(fresh)->x, 3.0f);
}
```

- [ ] **Step 2: Build and confirm the failure**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*
```

Expected: FAIL. Today a universal `T` reaches the archetype path, so:
- in `AddOnAPresentUniversalIsANoOpReturningTrue`, `AddComponent<Position>` MOVES the entity into a `{Position}` archetype and fires `ComponentAdded`, so `added` is 1, not 0;
- in `RemoveIsRefusedOnEveryFormWithoutASignal`, every remove returns `false` only because Position is absent from the mask, and nothing asserts, so `count.n` is 0, not 4 (Debug);
- in `CommandBufferRemoveOfAUniversalIsRefusedAtExecute`, `count.n` is 0, not 1.

The `UniversalStorageStaleHandle` arm already passes, because `IsValid` guards every Add and Remove. It is a regression pin: the implementation must keep the stale check ahead of the refusal, so a stale handle never asserts.

- [ ] **Step 3: Implement the Universal arms**

In `include/Astra/Registry/Registry.hpp`:

(a) Add to the `private:` section, next to Task 9's routing helpers:

```cpp
        // D2's single exception (spec 2026-10-01 s3.3, s12.3): a universal component is
        // present on every entity and cannot be removed. Debug: ASTRA_ASSERT. Release:
        // false. Never a signal. Callers check the handle FIRST, so a stale handle is a
        // quiet miss, never a refusal.
        static bool RefuseUniversalRemove() noexcept
        {
            ASTRA_ASSERT(false, "RemoveComponent on a Universal component is refused: it is present on every entity (spec 2026-10-01 s3.3)");
            return false;
        }

        ASTRA_NODISCARD bool IsUniversalId(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS && m_componentStorage->HasUniversals()
                && m_componentStorage->KindOf(id) == StorageKind::Universal;
        }
```

(b) `AddComponent<T>` and `EmplaceComponent<T>`: directly after their `if (!m_entityManager.IsValid(entity)) return false;`, insert:

```cpp
            if (m_componentStorage->HasUniversals() && KindOfT<T>() == StorageKind::Universal) ASTRA_UNLIKELY
                return true;   // always present: today's present-component rule, no signal (s3.3, s12.2)
```

(c) `AddComponents<T>` and `EmplaceComponents<T>`: directly after `if (entities.empty()) return;`, insert:

```cpp
            if (m_componentStorage->HasUniversals() && KindOfT<T>() == StorageKind::Universal) ASTRA_UNLIKELY
                return;        // present on every entity: a no-op, no signal
```

(d) `RemoveComponent<T>`: directly after `AssertContextAffinity();`, insert:

```cpp
            if (m_componentStorage->HasUniversals() && KindOfT<T>() == StorageKind::Universal) ASTRA_UNLIKELY
                return m_entityManager.IsValid(entity) ? RefuseUniversalRemove() : false;
```

(e) `RemoveComponents<T>`: directly after `if (entities.empty()) return 0;`, insert:

```cpp
            if (m_componentStorage->HasUniversals() && KindOfT<T>() == StorageKind::Universal) ASTRA_UNLIKELY
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        (void)RefuseUniversalRemove();   // once per call
                        break;
                    }
                }
                return 0;
            }
```

(f) `AddComponentByID`: as its first statement, insert:

```cpp
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return false;  // present on every entity: the by-id Table rule for a present component
```

(g) `AddComponentsByID`: as its first statement, insert:

```cpp
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return 0;
```

(h) `RemoveComponentByID`: as its first statement, insert:

```cpp
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return m_entityManager.IsValid(entity) ? RefuseUniversalRemove() : false;
```

(i) `RemoveComponentsByID`: as its first statement, insert:

```cpp
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        (void)RefuseUniversalRemove();   // once per call
                        break;
                    }
                }
                return 0;
            }
```

Chunk C adds the Sparse arms to these same functions. Keep the Universal checks first, so that a Sparse-trait type declared universal is refused rather than erased.

- [ ] **Step 4: Build and run the suites**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*:RegistryTest.*:CommandBuffer*
```

Expected: every test passes, including the 4 new ones (`UniversalStorage` +3, `UniversalStorageStaleHandle` +1).

- [ ] **Step 5: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Registry/UniversalStorageTest.cpp
git commit -m "feat(storage): Universal add is a no-op (typed/batch true, by-id false: today's present-component rules) and every remove form is refused (Debug assert once per call, Release false, no signal); stale handles miss quietly (Review Focus 1, Add/Remove arms)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: Universal `SetEnabled` / `IsEnabled` (typed, by-id, CommandBuffer)

Spec s3.3 (the enable row) and s7.2 (`SetEnabledByID` routes store kinds to the store and fires `ComponentEnabled`/`ComponentDisabled` on a change only).

For a universal id:
- **`SetEnabledByID` returns** `true` iff the entity is live, regardless of whether the state changed; `false` for a stale handle or a non-enableable type. These are today's Table answers.
- **The disabled flag** lives in the column page, and the signal fires only on a genuine change.
- **`IsEnabledByID` reports** `!disabled` for a live entity, `true` for a non-enableable universal (a present, non-enableable component is always enabled), and `false` for a stale handle.

The typed `SetEnabled<T>` / `IsEnabled<T>` and the CommandBuffer executor all funnel through these.

The chunk-side `disabled` mirror that queries consume (spec s5.1: an enableable universal also gets a slot and blocks) is Chunk C's. This task only keeps the store, which is the truth (P3), correct.

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (`SetEnabledByID`, `IsEnabledByID`)
- Modify: `tests/Registry/UniversalStorageTest.cpp` (append)

**Interfaces:**
- Consumes: `UniversalColumn::IsLive/IsDisabled/SetDisabled/Get` (Task 8), `IsUniversalId` (Task 12).
- Produces: no new signatures. The behaviour of `SetEnabledByID`/`IsEnabledByID` for universal ids is as described above.

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/UniversalStorageTest.cpp`:

```cpp
namespace
{
    Registry::Config UniversalTimerConfig()
    {
        Registry::Config cfg;
        cfg.AddUniversal<Astra::Test::Timer>();   // enableable
        cfg.AddUniversal<Position>();             // not enableable
        return cfg;
    }
}

TEST(UniversalStorage, SetEnabledTogglesTheColumnFlagAndSignalsOnChangeOnly)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity e = reg.CreateEntity();
    const Entity other = reg.CreateEntity();
    reg.EnableSignals(Astra::Signal::ComponentEnabled | Astra::Signal::ComponentDisabled);
    int enabledSignals = 0, disabledSignals = 0;
    const void* payload = nullptr;
    auto* signals = reg.GetSignalManager();
    auto hOn = signals->On<Astra::Events::ComponentEnabled>().Register(
        [&](const Astra::Events::ComponentEnabled& ev) { ++enabledSignals; payload = ev.component; });
    auto hOff = signals->On<Astra::Events::ComponentDisabled>().Register(
        [&](const Astra::Events::ComponentDisabled& ev) { ++disabledSignals; payload = ev.component; });

    EXPECT_TRUE(reg.IsEnabled<Timer>(e));                  // born enabled
    EXPECT_TRUE(reg.SetEnabled<Timer>(e, false));
    EXPECT_FALSE(reg.IsEnabled<Timer>(e));
    EXPECT_TRUE(reg.IsEnabled<Timer>(other));              // per entity
    EXPECT_EQ(disabledSignals, 1);
    EXPECT_EQ(payload, static_cast<const void*>(std::as_const(reg).GetComponent<Timer>(e)));
    EXPECT_TRUE(reg.SetEnabled<Timer>(e, false));          // idempotent: still true...
    EXPECT_EQ(disabledSignals, 1);                         // ...but silent
    EXPECT_TRUE(reg.HasComponent<Timer>(e));               // existence never lies
    EXPECT_NE(std::as_const(reg).GetComponent<Timer>(e), nullptr);
    EXPECT_TRUE(reg.SetEnabled<Timer>(e, true));
    EXPECT_TRUE(reg.IsEnabled<Timer>(e));
    EXPECT_EQ(enabledSignals, 1);

    // A non-enableable universal: present, always enabled, nothing to toggle.
    EXPECT_FALSE(reg.SetEnabledByID(e, Astra::TypeID<Position>::Value(), false));
    EXPECT_TRUE(reg.IsEnabledByID(e, Astra::TypeID<Position>::Value()));

    signals->On<Astra::Events::ComponentEnabled>().Unregister(hOn);
    signals->On<Astra::Events::ComponentDisabled>().Unregister(hOff);
}

TEST(UniversalStorage, CommandBufferSetEnabledReachesTheColumn)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity e = reg.CreateEntity();
    {
        Astra::CommandBuffer cb(&reg);
        cb.SetEnabled<Timer>(e, false);
        EXPECT_FALSE(cb.Execute().IsErr());
    }
    EXPECT_FALSE(reg.IsEnabled<Timer>(e));
}

TEST(UniversalStorage, DestroyRecycleBringsTheNextOccupantBackEnabled)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.SetEnabled<Timer>(e, false));
    reg.DestroyEntity(e);
    const Entity next = reg.CreateEntity();
    ASSERT_EQ(next.GetID(), e.GetID());
    EXPECT_TRUE(reg.IsEnabled<Timer>(next));               // born enabled, like any component
}

// ---- Review Focus 1 (Universal), SetEnabled arm ----
TEST(UniversalStorageStaleHandle, SetEnabledMissesAndNeverTogglesTheOccupant)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity stale = reg.CreateEntity();
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());

    EXPECT_FALSE(reg.SetEnabled<Timer>(stale, false));
    EXPECT_FALSE(reg.IsEnabled<Timer>(stale));             // stale: "not enabled" (Table answer)
    EXPECT_TRUE(reg.IsEnabled<Timer>(fresh));              // the occupant was never disabled

    reg.Clear();
    const Entity afterClear = reg.CreateEntity();
    ASSERT_EQ(afterClear.GetID(), fresh.GetID());
    EXPECT_FALSE(reg.SetEnabled<Timer>(fresh, false));
    EXPECT_TRUE(reg.IsEnabled<Timer>(afterClear));
}
```

- [ ] **Step 2: Build and confirm the failure**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*
```

Expected: FAIL. `SetEnabledByID`/`IsEnabledByID` read the archetype mask, where a universal component is absent, so:
- `IsEnabled<Timer>(e)` is `false` at the first expectation;
- `SetEnabled<Timer>(e, false)` returns `false`;
- in the CommandBuffer test, the executor's `SetEnabledByID` returns `false`, so `Execute()` reports an error (its final `IsEnabled` is `false` for the wrong reason);
- the recycle test fails at `ASSERT_TRUE(reg.SetEnabled<Timer>(e, false))`;
- the stale arm fails at `EXPECT_TRUE(reg.IsEnabled<Timer>(fresh))`.

- [ ] **Step 3: Route the universal ids**

In `include/Astra/Registry/Registry.hpp`:

(a) In `SetEnabledByID`, directly after its `componentId >= MAX_COMPONENTS` guard, insert:

```cpp
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return SetEnabledUniversal(entity, componentId, enable);
```

(b) In `IsEnabledByID`, directly after its `componentId >= MAX_COMPONENTS` guard, insert:

```cpp
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
            {
                const UniversalColumn* column = m_componentStorage->FindUniversal(componentId);
                if (!column || !column->IsLive(entity)) ASTRA_UNLIKELY
                    return false;                         // stale or never constructed: not enabled
                return !column->IsDisabled(entity);       // non-enableable: IsDisabled is false
            }
```

(c) Add to the `private:` section:

```cpp
        // Universal arm of SetEnabledByID (spec s3.3, s7.2). The column's per-slot flag is
        // the truth (P3). Chunk C's enable task adds the chunk-side `disabled` mirror
        // update here. Same answers as the Table path: true iff the live entity holds an
        // enableable component, whether or not the state changed; the signal fires on a
        // genuine change only.
        bool SetEnabledUniversal(Entity entity, ComponentID componentId, bool enable)
        {
            UniversalColumn* column = m_componentStorage->FindUniversal(componentId);
            if (!column || !column->IsLive(entity)) ASTRA_UNLIKELY
                return false;                             // stale handle: miss
            if (!column->Descriptor().isEnableable) ASTRA_UNLIKELY
                return false;                             // present but not enableable: nothing to toggle
            const bool changed = column->SetDisabled(entity, !enable);   // SET == DISABLED
            if (changed)
            {
                const Signal flag = enable ? Signal::ComponentEnabled : Signal::ComponentDisabled;
                if (m_signalManager.IsSignalEnabled(flag)) ASTRA_UNLIKELY
                {
                    void* component = column->Get(entity);
                    if (enable) m_signalManager.Emit<Events::ComponentEnabled>(entity, componentId, component);
                    else        m_signalManager.Emit<Events::ComponentDisabled>(entity, componentId, component);
                }
            }
            return true;
        }
```

- [ ] **Step 4: Build and run the suites**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*:EnableableTrait*:ViewEnableableVisibility*:ViewTest.*:RegistryTest.*:CommandBuffer*
```

Expected: every test passes, including the 4 new ones (`UniversalStorage` +3, `UniversalStorageStaleHandle` +1) and the existing enableable and CommandBuffer suites.

- [ ] **Step 5: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Registry/UniversalStorageTest.cpp
git commit -m "feat(storage): Universal SetEnabled/IsEnabled -- by-id paths route universal ids to the column's per-slot flag (true iff live + enableable, signal on change only; non-enableable universal is always enabled); typed and CommandBuffer forms follow; stale handles miss (Review Focus 1, SetEnabled arm)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: Universal change ticks -- `GetComponent` stamps, `Modified`, `IsChanged`/`IsAdded`, `SetIfNeq`

Spec s3.3 (the `Modified`/`IsChanged`/`IsAdded`/`SetIfNeq` row: per-slot ticks in the page) and R1 (`GetComponentMut`, `MarkWritten` and `IsChanged` each indexed `idToColumn` and now dispatch on kind).

For a universal `T`:
- **A mutable `GetComponent<T>` is a write.** It bumps the page version and, for a tracked `T`, the slot's `changed`.
- **`Modified<T>` / `Modified(e, id)`** stamp the same way, and return `false` for a stale handle, an absent component or a tag.
- **`IsChanged`/`IsAdded`** are exact per entity for a tracked `T`. For an untracked `T` they are page-coarse: the documented chunk-coarse trade, restated per 1024-slot page. They never stamp, and return `false` for a stale handle or a tag.
- **`SetIfNeq`** compares through a non-stamping fetch, and stamps only on a store.

The Sparse arms of these helpers are Chunk C's: Task 20 REPLACES each helper's `default:` arm with a `case StorageKind::Sparse:` (its `StampSparseWrite` / `SparseChangedSince`, which read `ComponentStorage::CoarseVersion` for untracked types). Because Task 11's `RoutesToStore<T>()` is already `true` for every Sparse-trait `T`, every entry point edited below (mutable `GetComponent`, `Modified`, `IsChanged`, `IsAdded`, `SetIfNeq`) picks up the Sparse case with no further edit.

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp`
- Modify: `tests/Registry/UniversalStorageTest.cpp` (append)

**Interfaces:**
- Consumes: `UniversalColumn::MarkWritten/AddedTick/ChangedTick` (Task 8); `RoutesToStore`, `StoreGet` and `KindOfT` (Tasks 9/11).
- Produces (Registry, private):
  ```cpp
  bool MarkWrittenRouted(Entity entity, ComponentID id);                              // Table -> ArchetypeManager::MarkWritten; store kinds -> MarkStoreWritten
  bool MarkStoreWritten(Entity entity, ComponentID id);                               // Universal arm (Sparse: Chunk C)
  bool StoreTickNewer(Entity entity, ComponentID id, Tick since, bool added) const;   // Universal arm (Sparse: Chunk C)
  ```

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/UniversalStorageTest.cpp`:

```cpp
namespace
{
    void AdvanceTo(Registry& reg, Astra::Tick t)
    {
        while (reg.CurrentTick() < t)
            reg.AdvanceTick();
    }

    Registry::Config TickConfig()
    {
        Registry::Config cfg;
        cfg.AddUniversal<Astra::Test::TrackedPos>();   // change-tracked: exact
        cfg.AddUniversal<Position>();                  // untracked: page-coarse
        cfg.AddUniversal<Astra::Test::Player>();       // tag: no value
        return cfg;
    }
}

TEST(UniversalStorageTicks, TrackedIsExactPerEntity)
{
    using Astra::Test::TrackedPos;
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity a = reg.CreateEntity();
    const Entity b = reg.CreateEntity();
    EXPECT_TRUE(reg.IsAdded<TrackedPos>(a, 1));           // added at 2
    EXPECT_FALSE(reg.IsAdded<TrackedPos>(a, 2));
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(a, 2));

    AdvanceTo(reg, 3);
    reg.GetComponent<TrackedPos>(a)->x = 1.0f;            // a mutable Get is a write
    EXPECT_TRUE(reg.IsChanged<TrackedPos>(a, 2));
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(b, 2));        // exact: b untouched
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(a, 3));        // not newer than its own tick
    EXPECT_FALSE(reg.IsAdded<TrackedPos>(a, 2));          // a write is not a re-add

    AdvanceTo(reg, 4);
    (void)std::as_const(reg).GetComponent<TrackedPos>(b); // a const Get never stamps
    (void)reg.IsChanged<TrackedPos>(b, 0);                // nor does the query
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(b, 3));
    EXPECT_TRUE(reg.Modified<TrackedPos>(b));
    EXPECT_TRUE(reg.IsChanged<TrackedPos>(b, 3));
    EXPECT_TRUE(reg.Modified(a, Astra::TypeID<TrackedPos>::Value()));
    EXPECT_TRUE(reg.IsChanged<TrackedPos>(a, 3));
}

TEST(UniversalStorageTicks, UntrackedIsPageCoarse)
{
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity a = reg.CreateEntity();
    const Entity b = reg.CreateEntity();
    AdvanceTo(reg, 3);
    EXPECT_FALSE(reg.IsChanged<Position>(a, 2));
    reg.GetComponent<Position>(a)->x = 1.0f;
    EXPECT_TRUE(reg.IsChanged<Position>(a, 2));
    EXPECT_TRUE(reg.IsChanged<Position>(b, 2));           // same page: coarse, like a chunk column version
    EXPECT_TRUE(reg.IsAdded<Position>(b, 2));             // untracked IsAdded reads the same version (README caveat)
}

TEST(UniversalStorageTicks, SetIfNeqStampsOnlyOnInequality)
{
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity e = reg.CreateEntity();
    AdvanceTo(reg, 3);
    EXPECT_FALSE(reg.SetIfNeq<Position>(e, Position{}));               // equal: no store, no stamp
    EXPECT_FALSE(reg.IsChanged<Position>(e, 2));
    EXPECT_TRUE(reg.SetIfNeq<Position>(e, Position{9.0f, 0.0f, 0.0f})); // different: stored + stamped
    EXPECT_TRUE(reg.IsChanged<Position>(e, 2));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 9.0f);
    EXPECT_TRUE(reg.SetIfNeq<Astra::Test::TrackedPos>(e, Astra::Test::TrackedPos{1.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(reg.IsChanged<Astra::Test::TrackedPos>(e, 2));
}

TEST(UniversalStorageTicks, TagStaleAndInvalidAnswerFalseAndNeverStampTheOccupant)
{
    using Astra::Test::Player;
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity e = reg.CreateEntity();
    EXPECT_FALSE(reg.Modified<Player>(e));                    // a tag has no value (Table: no column)
    EXPECT_FALSE(reg.Modified(e, Astra::TypeID<Player>::Value()));
    EXPECT_FALSE(reg.IsChanged<Player>(e, 0));
    EXPECT_FALSE(reg.IsAdded<Player>(e, 0));
    EXPECT_FALSE(reg.Modified<Position>(Astra::Entity{}));
    EXPECT_FALSE(reg.IsChanged<Position>(Astra::Entity{}, 0));

    // Review Focus 1 (Universal), tick arm: the recycled id's stale handle stamps nothing.
    reg.DestroyEntity(e);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), e.GetID());
    AdvanceTo(reg, 5);
    EXPECT_FALSE(reg.Modified<Position>(e));
    EXPECT_FALSE(reg.IsChanged<Position>(e, 0));
    EXPECT_FALSE(reg.SetIfNeq<Position>(e, Position{1.0f, 1.0f, 1.0f}));
    EXPECT_FALSE(reg.IsChanged<Position>(fresh, 4));          // the occupant was not stamped
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(fresh)->x, 0.0f);
}
```

- [ ] **Step 2: Build and confirm the failure**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorageTicks.*
```

Expected: FAIL. `IsChanged`/`IsAdded` read the archetype column (`idToColumn[id] < 0` for a universal), so they always answer `false`:
- `TrackedIsExactPerEntity` fails at `EXPECT_TRUE(reg.IsAdded<TrackedPos>(a, 1))`;
- `UntrackedIsPageCoarse` fails after the write;
- `SetIfNeq` returns `false` for a different value, because its non-stamping fetch still reads the archetype, where a universal is absent;
- `Modified` returns `false`, for the same reason.

- [ ] **Step 3: Route the tick surfaces**

In `include/Astra/Registry/Registry.hpp`:

(a) Mutable `GetComponent<T>`: replace Task 9's store arm `return StoreGet<T>(entity);` with:

```cpp
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
            {
                // A mutable fetch is a write (spec s3.3), as for a chunk column. A tag has
                // no value to stamp.
                T* component = StoreGet<T>(entity);
                if constexpr (!std::is_empty_v<T>)
                {
                    if (component) ASTRA_LIKELY
                        (void)MarkStoreWritten(entity, TypeID<T>::Value());
                }
                return component;
            }
```

(b) `Modified<T>(Entity)`: replace its last line, `return m_archetypeManager->MarkWritten(entity, TypeID<T>::Value());`, with `return MarkWrittenRouted(entity, TypeID<T>::Value());`.

(c) `Modified(Entity, ComponentID)`: replace its last line, `return m_archetypeManager->MarkWritten(entity, id);`, with `return MarkWrittenRouted(entity, id);`.

(d) `IsChanged<T>`: directly after `AssertContextAffinity();`, insert:

```cpp
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
                return StoreTickNewer(entity, TypeID<T>::Value(), since, /*added=*/false);
```

`IsAdded<T>`: insert the same lines with `/*added=*/true`.

(e) `SetIfNeq<T>`: replace

```cpp
            T* current = m_archetypeManager->GetComponent<T>(entity);   // non-stamping fetch
```

with

```cpp
            T* current = RoutesToStore<T>() ? StoreGet<T>(entity)                       // non-stamping fetch,
                                            : m_archetypeManager->GetComponent<T>(entity);   // either storage
```

and replace `m_archetypeManager->MarkWritten(entity, TypeID<T>::Value());` with `(void)MarkWrittenRouted(entity, TypeID<T>::Value());`.

(f) Add to the `private:` section:

```cpp
        // Write-side stamp routed by kind: Table -> the chunk column (today's
        // ArchetypeManager::MarkWritten); store kinds -> the store. Same false paths
        // everywhere: invalid/stale handle, absent component, tag.
        bool MarkWrittenRouted(Entity entity, ComponentID id)
        {
            if (id < MAX_COMPONENTS && m_componentStorage->KindOf(id) != StorageKind::Table) ASTRA_UNLIKELY
                return MarkStoreWritten(entity, id);
            return m_archetypeManager->MarkWritten(entity, id);
        }

        bool MarkStoreWritten(Entity entity, ComponentID id)
        {
            if (!m_entityManager.IsValid(entity)) ASTRA_UNLIKELY
                return false;
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Universal:
                {
                    UniversalColumn* column = m_componentStorage->FindUniversal(id);
                    return column && column->MarkWritten(entity, CurrentTick());
                }
                default:
                    return false;   // Sparse: Task 20 replaces this arm with `case StorageKind::Sparse`
            }
        }

        // IsChanged/IsAdded for a store kind. Never stamps. Tracked: the slot's exact
        // tick; untracked: the page version (page-coarse); tag, stale or absent: false.
        ASTRA_NODISCARD bool StoreTickNewer(Entity entity, ComponentID id, Tick since, bool added) const
        {
            if (id >= MAX_COMPONENTS || !m_entityManager.IsValid(entity)) ASTRA_UNLIKELY
                return false;
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Universal:
                {
                    const UniversalColumn* column = m_componentStorage->FindUniversal(id);
                    if (!column || column->Descriptor().size == 0 || !column->IsLive(entity)) ASTRA_UNLIKELY
                        return false;
                    return IsNewer(added ? column->AddedTick(entity) : column->ChangedTick(entity), since);
                }
                default:
                    return false;   // Sparse: Task 20 replaces this arm with `case StorageKind::Sparse`
            }
        }
```

- [ ] **Step 4: Build and run the suites**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*:ChangeDetection*
```

Expected: every test passes, including the 4 new `UniversalStorageTicks` tests and the existing `ChangeDetection*` suites (the Table paths of `Modified`/`IsChanged`/`IsAdded`/`SetIfNeq` are unchanged).

- [ ] **Step 5: Full shuffled run**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Registry/UniversalStorageTest.cpp
git commit -m "feat(storage): Universal change ticks -- a mutable GetComponent and Modified stamp the column page (and the slot for tracked types); IsChanged/IsAdded are exact for tracked and page-coarse for untracked, never stamp; SetIfNeq compares non-stamping and stamps on store; tag/stale/invalid answer false" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---


## Chunk C: sparse structure and presence (Tasks 15-21)

Spec sections: s5 (presence blocks, P1-P3, relocation carry), s7.2 (add/remove/enable), s7.3 (destroy, Clear, Defragment), s3.3 (per-call contract).

Assumed from Tasks 1-14, exactly as the canonical interfaces name them:
- `Registry` owns `std::shared_ptr<ComponentStorage> m_componentStorage` (Task 5), exposed as `ComponentStorage* GetComponentStorage()` (Task 5). `ComponentStorage`'s own members are Chunk A's (Task 4): `m_registry`, `m_universal`, `m_slotOf`, `m_componentOfSlot`, `m_usedSlots`; Chunk B adds `m_universalColumns`, `m_sparseSets`, `m_sparseIds`, `m_storeCount`, `m_tickSource` (Tasks 9, 11).
- `ComponentStorage::ComponentOfSlot(slot)` returns `INVALID_COMPONENT` for an unassigned slot.
- `AcquireSparseSlot(id)` is idempotent: it returns the existing slot when `id` already has one. It accepts any Sparse id and any enableable Universal id.
- `KindOf(id)` reads the descriptor: the component must be registered before the call.
- Task 9 routes `GetComponent`/`HasComponent` for Universal ids and Task 11 extends the routing (`RoutesToStore`, `StoreLookup`) to Sparse ids.
- Task 12 adds the Universal arms of Add/Remove (a no-op `true` / a refusal). They come FIRST in each function, so a Sparse-trait type declared universal is refused, never erased. Keep that order.
- Task 13 adds column-only Universal enable arms, which Task 19 deletes and replaces (it adds the chunk mirror). Task 14 adds `MarkStoreWritten`/`StoreTickNewer` with a `default:` arm that Task 20 replaces with the Sparse case.

This chunk adds only the **Sparse** arms of add and remove. It does not edit Task 12's Universal arms.

### Task 15: Chunk presence blocks (`SparseBlock`) and the pool's word freelist

Spec s5.1. Chunk-level only. No Registry code reads the blocks yet. Relocation sites are taught the carry in Task 17.

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp`
  - add a new `SparseWordFreelist` class before `ArchetypeChunk`;
  - add `ArchetypeChunk::SparseBlock` and its members and methods;
  - update the destructor and move constructor;
  - add the freelist member to `ArchetypeChunkPool`;
  - wire it in `CreateChunk`;
  - add `GetSparseWordStats`;
  - add a trim in `Defragment`.
- Create: `tests/Registry/SparseBlockTest.cpp`

**Interfaces:**
- `SparseWordFreelist`, a non-copyable class:
  - `uint64_t* Acquire(size_t words)` returns zeroed words.
  - `void Release(uint64_t* words, size_t wordCount) noexcept`
  - `void Trim() noexcept`
  - `size_t LiveBlocks() const noexcept`
  - `size_t CachedBlocks() const noexcept`
- `ArchetypeChunk`, canonical members:
  - `struct SparseBlock { uint16_t slot; uint32_t presentCount; uint32_t disabledCount; uint64_t* present; uint64_t* disabled; };`
  - `FindSparseBlock` (const and non-const)
  - `GetOrCreateSparseBlock(slot, withDisabled)`
  - `SetSparsePresent`
  - `IsSparsePresent`
  - `SetSparseDisabled`
  - `CarrySparseBits(src, srcIndex, dstIndex)`
  - `SwapRemoveSparseBits(index, lastIndex)`
- `ArchetypeChunk` additions beyond the canonical list (flagged in the chunk notes):
  - `bool IsSparseDisabled(uint16_t slot, size_t index) const noexcept`
  - `std::span<const SparseBlock> GetSparseBlocks() const noexcept`
  - `bool HasSparseBlocks() const noexcept`
  - `void ReleaseSparseBlock(uint16_t slot) noexcept`, which Task 36's module unload uses.
- `ArchetypeChunkPool`:
  - `struct SparseWordStats { size_t liveBlocks; size_t cachedBlocks; };`
  - `SparseWordStats GetSparseWordStats() const noexcept`

The semantics are the spec s5.1 contract:
- A missing block means that no entity in the chunk holds the slot.
- `present == nullptr` marks a Universal-enableable mirror block, in which every entity is present.
- A Sparse slot's `present` words are allocated by its first `SetSparsePresent(true)`. That call always comes before any disabled bit is set for the slot.
- Clearing a present bit also clears that slot's disabled bit.
- Bits at slot indices `>= count` are always 0.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/SparseBlockTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <memory>

using Astra::ArchetypeChunk;
using Astra::ArchetypeChunkPool;
using Astra::ArchetypeColumnMeta;
using Astra::Entity;

namespace
{
    using ChunkPtr = std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>;

    ArchetypeChunkPool::Config SmallPoolConfig()
    {
        ArchetypeChunkPool::Config c;
        c.useHugePages = false;
        c.chunksPerBlock = 4;
        return c;
    }

    // Column-less chunks (the root archetype's shape): presence blocks only need
    // the entity slots, not any column storage.
    class SparseBlockTest : public ::testing::Test
    {
    protected:
        ArchetypeChunkPool pool{SmallPoolConfig()};
        ArchetypeColumnMeta meta;

        ChunkPtr Make(size_t capacity, size_t count)
        {
            ChunkPtr chunk = pool.CreateChunk(capacity, ArchetypeChunkPool::MIN_CHUNK_SIZE, &meta);
            for (size_t i = 0; i < count; ++i)
                chunk->AddEntity(Entity(static_cast<Entity::StorageType>(i + 1), 1), Astra::Tick{1});
            return chunk;
        }
    };
}

TEST_F(SparseBlockTest, MissingBlockMeansAbsent)
{
    ChunkPtr chunk = Make(64, 4);
    EXPECT_EQ(chunk->FindSparseBlock(3), nullptr);
    EXPECT_FALSE(chunk->IsSparsePresent(3, 1));
    EXPECT_FALSE(chunk->IsSparseDisabled(3, 1));
    EXPECT_FALSE(chunk->HasSparseBlocks());
}

TEST_F(SparseBlockTest, SetPresentCreatesBlockLazilyAndCounts)
{
    ChunkPtr chunk = Make(64, 8);
    EXPECT_TRUE(chunk->SetSparsePresent(3, 5, true));
    EXPECT_FALSE(chunk->SetSparsePresent(3, 5, true));   // idempotent: no change
    const ArchetypeChunk::SparseBlock* b = chunk->FindSparseBlock(3);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(b->present, nullptr);
    EXPECT_EQ(b->disabled, nullptr);                      // disabled words are lazy too
    EXPECT_EQ(b->presentCount, 1u);
    EXPECT_TRUE(chunk->IsSparsePresent(3, 5));
    EXPECT_FALSE(chunk->IsSparsePresent(3, 4));
    EXPECT_TRUE(chunk->SetSparsePresent(3, 5, false));
    EXPECT_EQ(chunk->FindSparseBlock(3)->presentCount, 0u);
}

TEST_F(SparseBlockTest, BlocksStaySortedBySlot)
{
    ChunkPtr chunk = Make(64, 2);
    chunk->SetSparsePresent(7, 0, true);
    chunk->SetSparsePresent(2, 1, true);
    chunk->SetSparsePresent(5, 0, true);
    const auto blocks = chunk->GetSparseBlocks();
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].slot, 2u);
    EXPECT_EQ(blocks[1].slot, 5u);
    EXPECT_EQ(blocks[2].slot, 7u);
}

TEST_F(SparseBlockTest, ClearingPresentClearsDisabled)
{
    ChunkPtr chunk = Make(64, 4);
    chunk->SetSparsePresent(1, 2, true);
    EXPECT_TRUE(chunk->SetSparseDisabled(1, 2, true));
    EXPECT_EQ(chunk->FindSparseBlock(1)->disabledCount, 1u);
    chunk->SetSparsePresent(1, 2, false);
    EXPECT_FALSE(chunk->IsSparseDisabled(1, 2));
    EXPECT_EQ(chunk->FindSparseBlock(1)->disabledCount, 0u);
}

TEST_F(SparseBlockTest, UniversalMirrorBlockHasNoPresentWords)
{
    ChunkPtr chunk = Make(64, 6);
    EXPECT_TRUE(chunk->SetSparseDisabled(9, 4, true));
    const ArchetypeChunk::SparseBlock* b = chunk->FindSparseBlock(9);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->present, nullptr);
    EXPECT_EQ(b->disabledCount, 1u);
    EXPECT_TRUE(chunk->IsSparseDisabled(9, 4));
    EXPECT_FALSE(chunk->IsSparsePresent(9, 4));   // no present words: the caller knows it is Universal
}
```

Continue `tests/Registry/SparseBlockTest.cpp` (same file):

```cpp
TEST_F(SparseBlockTest, SwapRemoveMovesTailBitsAndClearsTail)
{
    ChunkPtr chunk = Make(64, 4);
    chunk->SetSparsePresent(1, 3, true);    // tail (index 3) is a member of slot 1
    chunk->SetSparseDisabled(1, 3, true);
    chunk->SetSparsePresent(2, 1, true);    // removed slot (index 1) is a member of slot 2 only
    chunk->SwapRemoveSparseBits(1, 3);
    EXPECT_TRUE(chunk->IsSparsePresent(1, 1));
    EXPECT_TRUE(chunk->IsSparseDisabled(1, 1));
    EXPECT_FALSE(chunk->IsSparsePresent(1, 3));
    EXPECT_FALSE(chunk->IsSparseDisabled(1, 3));
    EXPECT_FALSE(chunk->IsSparsePresent(2, 1));   // the removed entity's membership is gone
    EXPECT_EQ(chunk->FindSparseBlock(1)->presentCount, 1u);
    EXPECT_EQ(chunk->FindSparseBlock(1)->disabledCount, 1u);
    EXPECT_EQ(chunk->FindSparseBlock(2)->presentCount, 0u);
}

TEST_F(SparseBlockTest, SwapRemoveOfTheTailClearsOnlyTheTail)
{
    ChunkPtr chunk = Make(64, 4);
    chunk->SetSparsePresent(1, 3, true);
    chunk->SetSparsePresent(1, 0, true);
    chunk->SwapRemoveSparseBits(3, 3);
    EXPECT_FALSE(chunk->IsSparsePresent(1, 3));
    EXPECT_TRUE(chunk->IsSparsePresent(1, 0));
    EXPECT_EQ(chunk->FindSparseBlock(1)->presentCount, 1u);
}

TEST_F(SparseBlockTest, CarryCopiesEverySetBitAndAllocatesLazily)
{
    ChunkPtr src = Make(64, 4);
    ChunkPtr dst = Make(64, 1);
    src->SetSparsePresent(1, 2, true);
    src->SetSparseDisabled(1, 2, true);
    src->SetSparsePresent(4, 2, true);
    src->SetSparseDisabled(6, 2, true);      // Universal mirror block (no present words)
    src->SetSparsePresent(8, 0, true);       // a different slot index: not carried
    dst->CarrySparseBits(*src, 2, 0);
    EXPECT_TRUE(dst->IsSparsePresent(1, 0));
    EXPECT_TRUE(dst->IsSparseDisabled(1, 0));
    EXPECT_TRUE(dst->IsSparsePresent(4, 0));
    EXPECT_FALSE(dst->IsSparseDisabled(4, 0));
    ASSERT_NE(dst->FindSparseBlock(6), nullptr);
    EXPECT_EQ(dst->FindSparseBlock(6)->present, nullptr);   // mirror stays a mirror
    EXPECT_TRUE(dst->IsSparseDisabled(6, 0));
    EXPECT_EQ(dst->FindSparseBlock(8), nullptr);            // never created for an unset bit
    EXPECT_EQ(dst->GetSparseBlocks().size(), 3u);
}

TEST_F(SparseBlockTest, CountsMatchPopcountAcrossWords)
{
    ChunkPtr chunk = Make(200, 200);    // 4 words per block
    for (size_t i : {size_t(0), size_t(63), size_t(64), size_t(199)})
        chunk->SetSparsePresent(0, i, true);
    EXPECT_EQ(chunk->FindSparseBlock(0)->presentCount, 4u);
    EXPECT_TRUE(chunk->IsSparsePresent(0, 199));
    EXPECT_FALSE(chunk->IsSparsePresent(0, 198));
}

TEST_F(SparseBlockTest, WordsReturnToThePoolWithTheChunkAndAreReused)
{
    const auto before = pool.GetSparseWordStats();
    {
        ChunkPtr chunk = Make(64, 4);
        chunk->SetSparsePresent(1, 0, true);
        chunk->SetSparsePresent(2, 1, true);
        chunk->SetSparseDisabled(2, 1, true);                // 3 word blocks live
        EXPECT_EQ(pool.GetSparseWordStats().liveBlocks, before.liveBlocks + 3);
    }   // chunk freed: its blocks go back to the freelist
    const auto after = pool.GetSparseWordStats();
    EXPECT_EQ(after.liveBlocks, before.liveBlocks);
    EXPECT_EQ(after.cachedBlocks, before.cachedBlocks + 3);

    ChunkPtr again = Make(64, 1);
    again->SetSparsePresent(5, 0, true);                     // reuses a cached block
    EXPECT_EQ(pool.GetSparseWordStats().cachedBlocks, after.cachedBlocks - 1);
}

TEST_F(SparseBlockTest, ReleaseSparseBlockDropsOneSlot)
{
    ChunkPtr chunk = Make(64, 2);
    chunk->SetSparsePresent(1, 0, true);
    chunk->SetSparsePresent(3, 1, true);
    const size_t live = pool.GetSparseWordStats().liveBlocks;
    chunk->ReleaseSparseBlock(1);
    EXPECT_EQ(chunk->FindSparseBlock(1), nullptr);
    EXPECT_NE(chunk->FindSparseBlock(3), nullptr);
    EXPECT_EQ(pool.GetSparseWordStats().liveBlocks, live - 1);
}

TEST_F(SparseBlockTest, PoolDefragmentTrimsCachedWords)
{
    {
        ChunkPtr chunk = Make(64, 1);
        chunk->SetSparsePresent(1, 0, true);
    }
    EXPECT_GT(pool.GetSparseWordStats().cachedBlocks, 0u);
    pool.Defragment();
    EXPECT_EQ(pool.GetSparseWordStats().cachedBlocks, 0u);
}
```

- [ ] **Step 2: Regenerate the projects and run the test to verify it fails**

A test file was added, so regenerate and build (PowerShell, from `D:\dev\starworks\Astra-storage`):

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS. The first errors are `error C2039: 'FindSparseBlock': is not a member of 'Astra::ArchetypeChunk'` and `'GetSparseWordStats': is not a member of 'Astra::ArchetypeChunkPool'`.

- [ ] **Step 3: Implement the freelist, before `class ArchetypeChunk`**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`:
1. Add `#include <bit>` to the include block.
2. Insert the following directly after the `class Archetype;` forward declaration:

```cpp
    // Power-of-two word-block freelist for per-chunk sparse presence blocks
    // (storage policies, spec s5.1). Owned by ArchetypeChunkPool and shared by
    // every chunk it creates, so presence-block churn never reaches the global
    // allocator. A request of N words is served from the 2^ceil(log2 N) class.
    // Not thread-safe: structural changes run on the registry's context
    // (spec s4.3), the only place blocks are created or freed.
    class SparseWordFreelist
    {
    public:
        SparseWordFreelist() = default;
        SparseWordFreelist(const SparseWordFreelist&) = delete;
        SparseWordFreelist& operator=(const SparseWordFreelist&) = delete;
        ~SparseWordFreelist() { Trim(); }

        // Zeroed words, at least `words` of them (a chunk's WordsForCapacity).
        ASTRA_NODISCARD uint64_t* Acquire(size_t words)
        {
            ASTRA_ASSERT(words > 0, "SparseWordFreelist::Acquire of zero words");
            const size_t cls = ClassOf(words);
            ASTRA_ASSERT(cls < CLASS_COUNT, "sparse presence block larger than the freelist's largest class");
            uint64_t* p = nullptr;
            if (!m_free[cls].empty())
            {
                p = m_free[cls].back();
                m_free[cls].pop_back();
            }
            else
            {
                p = new uint64_t[size_t(1) << cls];
            }
            std::memset(p, 0, words * sizeof(uint64_t));
            ++m_live;
            return p;
        }

        // `wordCount` must be the value passed to the matching Acquire.
        void Release(uint64_t* words, size_t wordCount) noexcept
        {
            if (!words) return;
            ASTRA_ASSERT(m_live > 0, "SparseWordFreelist::Release without a matching Acquire");
            --m_live;
            m_free[ClassOf(wordCount)].push_back(words);
        }

        // Frees every cached (not live) block back to the heap.
        void Trim() noexcept
        {
            for (auto& bucket : m_free)
            {
                for (uint64_t* p : bucket)
                    delete[] p;
                bucket.clear();
            }
        }

        ASTRA_NODISCARD size_t LiveBlocks() const noexcept { return m_live; }
        ASTRA_NODISCARD size_t CachedBlocks() const noexcept
        {
            size_t n = 0;
            for (const auto& bucket : m_free) n += bucket.size();
            return n;
        }

    private:
        // 2^0 .. 2^19 words. The 1 MB chunk ceiling holds at most 2^18 slots
        // (4-byte entities), which is 2^12 words, so the top classes are headroom.
        static constexpr size_t CLASS_COUNT = 20;
        ASTRA_NODISCARD static size_t ClassOf(size_t words) noexcept { return static_cast<size_t>(std::bit_width(words - 1)); }

        std::vector<uint64_t*> m_free[CLASS_COUNT];
        size_t m_live = 0;
    };
```

- [ ] **Step 4: Implement the chunk side**

In `ArchetypeChunk`, make the following edits:

1. The move constructor. Append the two new members to the initializer list, after `m_chunkSize(other.m_chunkSize)`, in their declaration order (step 5):

```cpp
            m_chunkSize(other.m_chunkSize),
            m_sparseBlocks(std::move(other.m_sparseBlocks)),
            m_sparseWords(other.m_sparseWords)
```

   Then, beside `other.m_count = 0;`, add:

```cpp
            other.m_sparseBlocks.clear();   // the blocks' words now belong to *this
```

2. The destructor. Make its FIRST statement, before the `if (!m_meta)` early return, the following call. It is a no-op for a moved-from chunk, whose block list is empty.

```cpp
            ReleaseSparseBlocks();   // presence words go back to the pool (spec s5.1)
```

3. Add this public section after the enableable-components section, which ends at `SetDisabled`:

```cpp
        // ===================== Sparse presence blocks (storage policies, spec s5.1) =====================
        // One lazily created block per sparse SLOT that at least one entity of this
        // chunk holds. A slot is ComponentStorage's dense slot number, never a
        // ComponentID. A SET present bit means the entity in that chunk slot is a
        // member of the slot's sparse set (P1). A SET disabled bit means DISABLED
        // (P3). `present == nullptr` marks a Universal-enableable mirror block,
        // where every entity is present and only disabled words exist. A Sparse
        // slot's present words are allocated by its first SetSparsePresent(true),
        // which always precedes any disabled bit for that slot. Words come from the
        // pool's SparseWordFreelist and go back to it when the chunk dies.
        // Invariant: bits at chunk slots >= count are 0.
        struct SparseBlock
        {
            uint16_t slot{0};
            uint32_t presentCount{0};     // == popcount(present)
            uint32_t disabledCount{0};    // == popcount(disabled)
            uint64_t* present{nullptr};
            uint64_t* disabled{nullptr};
        };

        ASTRA_NODISCARD SparseBlock* FindSparseBlock(uint16_t slot) noexcept
        {
            SparseBlock* it = LowerBoundSparse(slot);
            return (it != m_sparseBlocks.data() + m_sparseBlocks.size() && it->slot == slot) ? it : nullptr;
        }
        ASTRA_NODISCARD const SparseBlock* FindSparseBlock(uint16_t slot) const noexcept
        {
            return const_cast<ArchetypeChunk*>(this)->FindSparseBlock(slot);
        }

        // Creates the slot's block if missing; with `withDisabled` also allocates
        // its disabled words. Never allocates present words (see SetSparsePresent).
        // May reallocate the block list: SparseBlock pointers from an earlier
        // Find/GetOrCreate on THIS chunk are invalidated (word pointers are not).
        SparseBlock& GetOrCreateSparseBlock(uint16_t slot, bool withDisabled)
        {
            SparseBlock* it = LowerBoundSparse(slot);
            const size_t pos = static_cast<size_t>(it - m_sparseBlocks.data());
            if (pos == m_sparseBlocks.size() || it->slot != slot)
            {
                SparseBlock fresh;
                fresh.slot = slot;
                m_sparseBlocks.insert(m_sparseBlocks.begin() + pos, fresh);
            }
            SparseBlock& b = m_sparseBlocks[pos];
            if (withDisabled && !b.disabled)
                b.disabled = AcquireSparseWords();
            return b;
        }

        // Returns true iff the bit changed. Clearing presence also clears the
        // slot's disabled bit: a non-member cannot be disabled (P3).
        bool SetSparsePresent(uint16_t slot, size_t index, bool present)
        {
            ASTRA_ASSERT(index < m_capacity, "SetSparsePresent: index out of capacity");
            if (present)
            {
                SparseBlock& b = GetOrCreateSparseBlock(slot, false);
                if (!b.present)
                    b.present = AcquireSparseWords();
                return WriteSparseBit(b.present, b.presentCount, index, true);
            }
            SparseBlock* b = FindSparseBlock(slot);
            if (!b || !b->present)
                return false;
            const bool changed = WriteSparseBit(b->present, b->presentCount, index, false);
            if (b->disabled)
                WriteSparseBit(b->disabled, b->disabledCount, index, false);
            return changed;
        }

        ASTRA_NODISCARD bool IsSparsePresent(uint16_t slot, size_t index) const noexcept
        {
            const SparseBlock* b = FindSparseBlock(slot);
            return b && b->present && TestSparseBit(b->present, index);
        }

        // Returns true iff the bit changed. Setting creates the block and its
        // disabled words lazily (a Universal mirror block when no present words exist).
        bool SetSparseDisabled(uint16_t slot, size_t index, bool disabled)
        {
            ASTRA_ASSERT(index < m_capacity, "SetSparseDisabled: index out of capacity");
            if (disabled)
            {
                SparseBlock& b = GetOrCreateSparseBlock(slot, true);
                return WriteSparseBit(b.disabled, b.disabledCount, index, true);
            }
            SparseBlock* b = FindSparseBlock(slot);
            if (!b || !b->disabled)
                return false;
            return WriteSparseBit(b->disabled, b->disabledCount, index, false);
        }

        ASTRA_NODISCARD bool IsSparseDisabled(uint16_t slot, size_t index) const noexcept
        {
            const SparseBlock* b = FindSparseBlock(slot);
            return b && b->disabled && TestSparseBit(b->disabled, index);
        }
```

Continue the same public section:

```cpp
        // Relocation carry (spec s5.3): copy every set bit of `src` slot `srcIndex`
        // into this chunk's FRESH slot `dstIndex` (its bits are 0 by the tail
        // invariant), across every block, creating destination blocks and words
        // lazily. A source mirror block (present == nullptr) stays a mirror.
        void CarrySparseBits(const ArchetypeChunk& src, size_t srcIndex, size_t dstIndex)
        {
            ASTRA_ASSERT(&src != this, "CarrySparseBits: source and destination must differ");
            for (const SparseBlock& s : src.m_sparseBlocks)
            {
                const bool present  = s.present  && TestSparseBit(s.present, srcIndex);
                const bool disabled = s.disabled && TestSparseBit(s.disabled, srcIndex);
                if (!present && !disabled)
                    continue;
                if (present)
                    SetSparsePresent(s.slot, dstIndex, true);
                if (disabled)
                    SetSparseDisabled(s.slot, dstIndex, true);
            }
        }

        // Swap-remove carry (spec s5.3): the tail slot's bits fill `index`, then the
        // tail is cleared. With index == lastIndex only the tail clear runs, which
        // drops exactly the removed entity's own bits.
        void SwapRemoveSparseBits(size_t index, size_t lastIndex) noexcept
        {
            for (SparseBlock& b : m_sparseBlocks)
            {
                MoveSparseBit(b.present, b.presentCount, index, lastIndex);
                MoveSparseBit(b.disabled, b.disabledCount, index, lastIndex);
            }
        }

        ASTRA_NODISCARD std::span<const SparseBlock> GetSparseBlocks() const noexcept
        {
            return std::span<const SparseBlock>(m_sparseBlocks.data(), m_sparseBlocks.size());
        }
        ASTRA_NODISCARD bool HasSparseBlocks() const noexcept { return !m_sparseBlocks.empty(); }

        // Drops one slot's block and returns its words (module unload, Task 36).
        void ReleaseSparseBlock(uint16_t slot) noexcept
        {
            SparseBlock* b = FindSparseBlock(slot);
            if (!b) return;
            const size_t words = WordsForCapacity(m_capacity);
            m_sparseWords->Release(b->present, words);
            m_sparseWords->Release(b->disabled, words);
            m_sparseBlocks.erase(b);
        }
```

4. Add these private helpers beside `GetColumnPointer`:

```cpp
        ASTRA_NODISCARD SparseBlock* LowerBoundSparse(uint16_t slot) noexcept
        {
            return std::lower_bound(m_sparseBlocks.data(), m_sparseBlocks.data() + m_sparseBlocks.size(), slot,
                                    [](const SparseBlock& b, uint16_t s) { return b.slot < s; });
        }

        ASTRA_NODISCARD uint64_t* AcquireSparseWords()
        {
            ASTRA_ASSERT(m_sparseWords, "chunk has no sparse word freelist (not created by ArchetypeChunkPool)");
            return m_sparseWords->Acquire(WordsForCapacity(m_capacity));
        }

        void ReleaseSparseBlocks() noexcept
        {
            if (m_sparseBlocks.empty()) return;
            const size_t words = WordsForCapacity(m_capacity);
            for (SparseBlock& b : m_sparseBlocks)
            {
                m_sparseWords->Release(b.present, words);
                m_sparseWords->Release(b.disabled, words);
            }
            m_sparseBlocks.clear();
        }

        ASTRA_NODISCARD static bool TestSparseBit(const uint64_t* words, size_t i) noexcept
        {
            return (words[i >> 6] >> (i & 63)) & 1ull;
        }

        // Writes one bit, keeping `count` == popcount. Returns true iff it changed.
        static bool WriteSparseBit(uint64_t* words, uint32_t& count, size_t i, bool value) noexcept
        {
            uint64_t& word = words[i >> 6];
            const uint64_t bit = 1ull << (i & 63);
            if (((word & bit) != 0) == value)
                return false;
            if (value) { word |= bit;  ++count; }
            else       { word &= ~bit; --count; }
            return true;
        }

        static void MoveSparseBit(uint64_t* words, uint32_t& count, size_t index, size_t lastIndex) noexcept
        {
            if (!words) return;
            const bool tail = TestSparseBit(words, lastIndex);
            if (index != lastIndex)
                WriteSparseBit(words, count, index, tail);
            WriteSparseBit(words, count, lastIndex, false);
        }
```

5. Add the members after `size_t m_chunkSize;`. The order matters for the move constructor's initializer list.

```cpp
        SmallVector<SparseBlock, 2> m_sparseBlocks;     // sorted by slot; lazily populated (spec s5.1)
        SparseWordFreelist* m_sparseWords{nullptr};     // the owning pool's freelist (set by CreateChunk)
```

6. In `ArchetypeChunkPool`:
   - In `CreateChunk`, after `auto* chunk = new Chunk(capacity, meta, memory, chunkBytes);`, add `chunk->m_sparseWords = &m_sparseWords;`. `ArchetypeChunkPool` is already a friend of `ArchetypeChunk`.
   - In `Defragment()`, insert `m_sparseWords.Trim();` as the first statement.
   - Add the following public API after `GetGrowDivisor()`:

```cpp
        // Presence-block word accounting (storage policies, spec s5.1).
        struct SparseWordStats
        {
            size_t liveBlocks = 0;     // word blocks held by live chunks
            size_t cachedBlocks = 0;   // word blocks parked in the freelist
        };
        ASTRA_NODISCARD SparseWordStats GetSparseWordStats() const noexcept
        {
            return SparseWordStats{m_sparseWords.LiveBlocks(), m_sparseWords.CachedBlocks()};
        }
```

   - Add the member `SparseWordFreelist m_sparseWords;` after `SmallVector<ArenaRecord, 16> m_arenas;`.

     Chunks always die before their pool, because `ArchetypeManager` declares `m_chunkPool` first, so the freelist outlives every block. That is the same lifetime the existing `ChunkDeleter` back-pointer already relies on.

- [ ] **Step 5: Run the new suite and confirm it passes**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseBlockTest.*
```

Expected: `[  PASSED  ] 12 tests.`

- [ ] **Step 6: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: every test passes. Record the `Randomizing tests' orders with a seed of N` line in the task report.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Archetype/ArchetypeChunkPool.hpp tests/Registry/SparseBlockTest.cpp
git commit -m "feat(storage): per-chunk sparse presence blocks + pool word freelist (spec s5.1)

ArchetypeChunk gains lazily created SparseBlocks (present/disabled words, sorted
by slot) with Set/Is/Carry/SwapRemove helpers; words come from a power-of-two
SparseWordFreelist owned by ArchetypeChunkPool and return to it when the chunk dies.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 16: Sparse typed add/remove routing, the sparse structural counter, `ValidateStorageInvariants`

Spec s7.2, s5.2, s3.3. This task covers:
- `AddComponent<T>`, `EmplaceComponent<T>` and `RemoveComponent<T>` for a Sparse `T`. Each one inserts into or erases from the set, sets or clears the chunk bit, and never moves the entity between archetypes.
- `ComponentAdded`/`ComponentRemoved` with Table timing.
- `m_sparseStructuralCounter`.
- The P1-P3 validator.
- The creation paths' Sparse inserts (Task 11) are rerouted through the same funnel, so a created member has its bit.

**Files:**
- Create: `tests/Registry/SparseStructureTestComponents.hpp`, holding the shared Tasks 16-21 test types and helpers.
- Create: `tests/Registry/SparseStructureTest.cpp`
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (the counter)
- Modify: `include/Astra/Registry/Registry.hpp`, adding:
  - the `<bit>` and `<string>` includes;
  - accessors;
  - `ResolveKind<T>`, `SparseAttach`, `SparseDetach` and `SparseTypedPtr`;
  - three Sparse arms;
  - the validator.

**Interfaces:**
- `ArchetypeManager`:
  - `uint32_t GetSparseStructuralCounter() const noexcept`
  - `void BumpSparseStructuralCounter() noexcept`
  - member `std::atomic<uint32_t> m_sparseStructuralCounter{0}`, which `View` (a friend) reads in Task 25.
- `Registry` public:
  - `std::string ValidateStorageInvariants() const` (canonical)
  - (`GetComponentStorage()` already exists: Task 5, pointer-returning. Do not add another.)
- `Registry` private:
  - `template<Component T> StorageKind ResolveKind() const`
  - `void* SparseAttach(Entity, const EntityRecord&, ComponentID, void* src)`
  - `bool SparseDetach(Entity, const EntityRecord&, ComponentID)`
  - `template<Component T> static T* SparseTypedPtr(void*)`

- [ ] **Step 1: Write the shared test components**

Create `tests/Registry/SparseStructureTestComponents.hpp`:

```cpp
#pragma once

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <cstdint>
#include <string>
#include <utility>

// Storage-policies Tasks 16-21: test components + helpers. Six types only: every
// test in the binary shares one TypeID space of ASTRA_MAX_COMPONENTS ids (see the
// budget note in tests/TestComponents.hpp). Table parts use Astra::Test::Position/Velocity.
namespace Astra::Test::SparseC
{
    struct SpA
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        float v = 0.0f;
        SpA() = default;
        explicit SpA(float x) : v(x) {}
    };

    struct SpB
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        int v = 0;
        SpB() = default;
        explicit SpB(int x) : v(x) {}
        bool operator==(const SpB&) const = default;   // Chunk D's query tests compare values
    };

    // Sparse + enableable + change-tracked (Tasks 19-20).
    struct SpE
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraEnableable = true;
        static constexpr bool AstraChangeTracked = true;
        float v = 0.0f;
        SpE() = default;
        explicit SpE(float x) : v(x) {}
        bool operator==(const SpE&) const = default;
    };

    // Sparse tag (presence only); enableable (spec s3.2: every tag may be).
    struct SpTag
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraEnableable = true;
    };

    // Non-trivially-copyable sparse value: MoveConstruct/Destruct paths.
    struct SpStr
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        std::string s;
        SpStr() = default;
        explicit SpStr(std::string v) : s(std::move(v)) {}
    };

    // Table by trait; tests make it Universal with Config::AddUniversal<UnE>().
    struct UnE
    {
        static constexpr bool AstraEnableable = true;
        float v = 0.0f;
    };

    inline std::pair<Astra::ArchetypeChunk*, size_t> Locate(Astra::Registry& reg, Astra::Entity e)
    {
        const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        if (!rec || !rec->chunk) return {nullptr, size_t(0)};
        return {rec->chunk, rec->location.GetEntityIndex()};
    }

    template<typename T>
    inline int SlotOf(Astra::Registry& reg)
    {
        return reg.GetComponentStorage()->SparseSlotOf(Astra::TypeID<T>::Value());
    }

    // T's presence bit for e, read straight from e's chunk (P1 observation).
    template<typename T>
    inline bool PresentBit(Astra::Registry& reg, Astra::Entity e)
    {
        auto [chunk, idx] = Locate(reg, e);
        const int slot = SlotOf<T>(reg);
        return chunk && slot >= 0 && chunk->IsSparsePresent(static_cast<uint16_t>(slot), idx);
    }

    // T's disabled-mirror bit for e (P3 observation).
    template<typename T>
    inline bool DisabledBit(Astra::Registry& reg, Astra::Entity e)
    {
        auto [chunk, idx] = Locate(reg, e);
        const int slot = SlotOf<T>(reg);
        return chunk && slot >= 0 && chunk->IsSparseDisabled(static_cast<uint16_t>(slot), idx);
    }

    // P1-P3 must hold: the validator's report is printed on failure.
    inline void ExpectInvariants(const Astra::Registry& reg)
    {
        const std::string report = reg.ValidateStorageInvariants();
        EXPECT_TRUE(report.empty()) << report;
    }
}
```

- [ ] **Step 2: Write the failing tests**

Create `tests/Registry/SparseStructureTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

TEST(SparseStructure, AddSparseNeverMovesTheEntity)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{1, 2, 3});
    const Astra::Archetype* before = reg.GetArchetypeManager()->GetEntityRecord(e)->archetype;
    const size_t archetypes = reg.GetArchetypeCount();
    const uint32_t structural = reg.GetArchetypeManager()->GetStructuralChangeCounter();

    EXPECT_TRUE(reg.AddComponent<SpA>(e, SpA{2.5f}));

    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, before);
    EXPECT_EQ(reg.GetArchetypeCount(), archetypes);
    EXPECT_EQ(reg.GetArchetypeManager()->GetStructuralChangeCounter(), structural);
    ASSERT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 2.5f);
    EXPECT_TRUE(PresentBit<SpA>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseStructure, RemoveSparseClearsMembershipAndBit)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e, SpA{1.0f});
    EXPECT_TRUE(reg.RemoveComponent<SpA>(e));
    EXPECT_FALSE(reg.HasComponent<SpA>(e));
    EXPECT_FALSE(PresentBit<SpA>(reg, e));
    EXPECT_FALSE(reg.RemoveComponent<SpA>(e));   // absent -> false (Table parity)
    EXPECT_FALSE(reg.RemoveComponent<SpB>(e));   // never held -> false
    ExpectInvariants(reg);
}

TEST(SparseStructure, AddWhenPresentIsANoOpReturningTrue)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int added = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded&) { ++added; });

    const Astra::Entity e = reg.CreateEntity();
    EXPECT_TRUE(reg.AddComponent<SpA>(e, SpA{1.0f}));
    EXPECT_TRUE(reg.AddComponent<SpA>(e, SpA{9.0f}));     // present: no-op
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 1.0f);   // value untouched
    EXPECT_EQ(added, 1);                                   // no second signal
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseStructure, SignalsCarryTheElementAndRemovedFiresBeforeRemoval)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    void* addedPtr = nullptr;
    bool presentInHandler = false;
    float valueInHandler = 0.0f;
    auto ha = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    auto hr = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev)
        {
            presentInHandler = reg.HasComponent<SpA>(ev.entity);
            valueInHandler = static_cast<SpA*>(ev.component)->v;
        });

    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{4.0f});
    EXPECT_EQ(addedPtr, static_cast<void*>(reg.GetComponent<SpA>(e)));
    reg.RemoveComponent<SpA>(e);
    EXPECT_TRUE(presentInHandler);           // emitted BEFORE the erase, as for Table
    EXPECT_FLOAT_EQ(valueInHandler, 4.0f);

    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(ha);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(hr);
}

TEST(SparseStructure, EmplaceAndNonTrivialValues)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    EXPECT_TRUE(reg.EmplaceComponent<SpStr>(e, std::string("hello")));
    ASSERT_NE(reg.GetComponent<SpStr>(e), nullptr);
    EXPECT_EQ(reg.GetComponent<SpStr>(e)->s, "hello");
    EXPECT_TRUE(reg.RemoveComponent<SpStr>(e));
    ExpectInvariants(reg);
}

TEST(SparseStructure, SparseTagAddRemove)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    void* addedPtr = nullptr;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    const Astra::Entity e = reg.CreateEntity();
    EXPECT_TRUE(reg.AddComponent<SpTag>(e, SpTag{}));
    EXPECT_NE(addedPtr, nullptr);            // the per-type tag instance
    EXPECT_TRUE(reg.HasComponent<SpTag>(e));
    EXPECT_TRUE(PresentBit<SpTag>(reg, e));
    EXPECT_TRUE(reg.RemoveComponent<SpTag>(e));
    EXPECT_FALSE(reg.HasComponent<SpTag>(e));
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseStructure, SparseAddAndRemoveBumpTheSparseCounterOnly)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    const uint32_t s0 = reg.GetArchetypeManager()->GetSparseStructuralCounter();
    reg.AddComponent<SpA>(e, SpA{});
    const uint32_t s1 = reg.GetArchetypeManager()->GetSparseStructuralCounter();
    EXPECT_NE(s1, s0);
    reg.RemoveComponent<SpA>(e);
    EXPECT_NE(reg.GetArchetypeManager()->GetSparseStructuralCounter(), s1);
}

TEST(SparseStructure, ManyEntitiesInOneChunkKeepP1)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(64);
    reg.CreateEntities<Position>(64, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 2 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 3 == 0) reg.AddComponent<SpB>(es[i], SpB{int(i)});
    }
    for (size_t i = 0; i < es.size(); i += 4) reg.RemoveComponent<SpA>(es[i]);
    ExpectInvariants(reg);
    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 2 == 0 && i % 4 != 0) << i;
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), reg.HasComponent<SpA>(es[i])) << i;
    }
}

TEST(SparseStructure, ValidatorReportsACorruptedPresenceBit)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e, SpA{});
    ASSERT_TRUE(reg.ValidateStorageInvariants().empty());
    auto [chunk, idx] = Locate(reg, e);
    chunk->SetSparsePresent(static_cast<uint16_t>(SlotOf<SpA>(reg)), idx, false);   // corrupt P1
    const std::string report = reg.ValidateStorageInvariants();
    EXPECT_NE(report.find("P1"), std::string::npos) << report;
}

TEST(SparseStructure, CreateWithASparseMemberSetsItsBit)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{}, SpA{3.0f});
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, reg.FindArchetype<Position>());
    EXPECT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_TRUE(PresentBit<SpA>(reg, e));
    ExpectInvariants(reg);
}
```

- [ ] **Step 3: Regenerate the projects and run the tests to verify they fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: the build FAILS. The errors are `'ValidateStorageInvariants': is not a member of 'Astra::Registry'` and `'GetSparseStructuralCounter': is not a member of 'Astra::ArchetypeManager'`. (`GetComponentStorage` already exists from Task 5.)

- [ ] **Step 4: Add the sparse structural counter**

In `include/Astra/Archetype/ArchetypeManager.hpp`:

1. Add the member after `std::atomic<uint32_t> m_archetypeRemovalCounter{0};`:

```cpp
        // Storage policies (spec s7.2/s4.3): bumped by every Sparse membership change
        // (add, remove, destroy-walk erase, Clear). Sparse changes never touch the
        // archetype set, so m_structuralChangeCounter stays put (no view-cache
        // invalidation); this separate counter feeds the iteration-safety guard
        // (Task 25) for views that walk sparse dense arrays.
        std::atomic<uint32_t> m_sparseStructuralCounter{0};
```

2. Add the accessors after `GetStructuralChangeCounter()`:

```cpp
        ASTRA_NODISCARD uint32_t GetSparseStructuralCounter() const noexcept
        {
            return m_sparseStructuralCounter.load(std::memory_order_acquire);
        }
        void BumpSparseStructuralCounter() noexcept
        {
            m_sparseStructuralCounter.fetch_add(1, std::memory_order_release);
        }
```

- [ ] **Step 5: Add the Registry funnel helpers**

In `include/Astra/Registry/Registry.hpp`:

1. Add `#include <bit>` and `#include <string>` to the include block.
2. `GetComponentStorage()` already exists (Task 5). It returns `ComponentStorage*` (const and non-const); tests use `->`. Add nothing here.

3. Add the private section, directly after `CreateEntityInternal` (Task 9 placed it in the private section):

```cpp
        // ---- Storage policies: the Sparse funnel (spec s7.2) ----

        // Effective kind of a Sparse-TRAIT type in this registry: Sparse, or Universal
        // when the Config declares it. Only ever called inside
        // `if constexpr (StorageOfV<T> == Storage::Sparse)`, so the Table hot paths
        // (Table trait) never pay the registration + lookup. KindOf reads T's
        // descriptor, hence the registration first.
        template<Component T>
        ASTRA_NODISCARD StorageKind ResolveKind() const
        {
            m_componentRegistry->RegisterComponent<T>();
            const ComponentID id = TypeID<T>::Value();
            return id < MAX_COMPONENTS ? m_componentStorage->KindOf(id) : StorageKind::Table;
        }

        // The typed signal payload for a Sparse element: the element itself, or (for
        // a tag) one per-type static instance -- the typed tag contract (spec s7.4).
        template<Component T>
        ASTRA_NODISCARD static T* SparseTypedPtr(void* element) noexcept
        {
            if constexpr (std::is_empty_v<T>)
            {
                static T s_tag{};
                (void)element;
                return &s_tag;
            }
            else
            {
                return static_cast<T*>(element);
            }
        }

        // The ONE place a Sparse membership is created. Inserts `entity` into `id`'s
        // set (moving from `src`, or default-constructing when src == nullptr; always
        // nullptr for a tag), sets the presence bit at the entity's chunk slot (P1)
        // and bumps the sparse structural counter. Preconditions: `rec` is entity's
        // validated record; entity is not yet a member. Returns the element pointer
        // (EmptyComponentSentinel() for a tag), or nullptr when the
        // ASTRA_MAX_SPARSE_COMPONENTS cap refuses the component (AcquireSparseSlot
        // asserts in Debug; Release returns false to the caller, spec s3.2).
        void* SparseAttach(Entity entity, const EntityRecord& rec, ComponentID id, void* src)
        {
            int slot = m_componentStorage->SparseSlotOf(id);
            if (slot < 0) ASTRA_UNLIKELY
                slot = m_componentStorage->AcquireSparseSlot(id);
            if (slot < 0) ASTRA_UNLIKELY
                return nullptr;
            SparseSet* set = m_componentStorage->Sparse(id);
            ASTRA_ASSERT(set && !set->Contains(entity), "SparseAttach: entity is already a member");
            void* element = src ? set->InsertMove(entity, src) : set->Insert(entity);
            rec.chunk->SetSparsePresent(static_cast<uint16_t>(slot), rec.location.GetEntityIndex(), true);
            m_archetypeManager->BumpSparseStructuralCounter();
            return set->Descriptor().size == 0 ? EmptyComponentSentinel() : element;
        }

        // Erases `entity` from `id`'s set and clears its presence bit, which also
        // clears its disabled mirror bit. ComponentRemoved is the caller's and fires
        // BEFORE this. Returns false when `entity` is not a member.
        bool SparseDetach(Entity entity, const EntityRecord& rec, ComponentID id)
        {
            const int slot = m_componentStorage->SparseSlotOf(id);
            if (slot < 0) ASTRA_UNLIKELY
                return false;
            SparseSet* set = m_componentStorage->Sparse(id);
            if (!set || !set->Erase(entity))
                return false;
            rec.chunk->SetSparsePresent(static_cast<uint16_t>(slot), rec.location.GetEntityIndex(), false);
            m_archetypeManager->BumpSparseStructuralCounter();
            return true;
        }
```

4. Reroute creation. Run:

```powershell
git grep -n -e "InsertMove(" -e "->Insert(" -- include/Astra/Registry/Registry.hpp include/Astra/Registry/ComponentStorage.hpp include/Astra/Commands/CommandBuffer.hpp
```

Every Sparse insert that the Task 11 creation paths perform must go through the funnel:
- `CreateEntityWith`
- `CreateEntities`
- `CreateEntitiesWith`
- `CreateEntityInternal`
- the CommandBuffer executors

For each one, keep its `src`/default choice and replace the call with `SparseAttach(entity, *rec, id, src)`, where `rec` is `m_archetypeManager->GetEntityRecord(entity)` fetched after placement. If a creation helper lives inside `ComponentStorage`, where no chunk is visible, call `SparseAttach` from its Registry caller instead.

The only remaining `->Insert(` / `InsertMove(` on a `SparseSet` in `Registry.hpp` must be the pair inside `SparseAttach`.

- [ ] **Step 6: Add the Sparse arms to the typed add/emplace/remove**

Each arm is compiled only for a Sparse-trait `T` (`if constexpr`), so Table-trait instantiations are byte-for-byte unchanged (spec S4). Task 12's Universal arm for the same functions stays where it is, FIRST: the two arms are mutually exclusive.

1. In `AddComponent<T>(Entity, const T&)`, insert directly after Task 12's Universal arm (which itself sits directly after `if (!m_entityManager.IsValid(entity)) return false;`):

```cpp
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    if (!rec || !rec->chunk) ASTRA_UNLIKELY
                        return false;
                    const ComponentID id = TypeID<T>::Value();
                    if (SparseSet* set = m_componentStorage->Sparse(id); set && set->Contains(entity))
                        return true;                       // present: no-op, no signal (Table parity, s3.3)
                    void* element = nullptr;
                    if constexpr (std::is_empty_v<T>)
                    {
                        element = SparseAttach(entity, *rec, id, nullptr);
                    }
                    else
                    {
                        T value(component);
                        element = SparseAttach(entity, *rec, id, &value);
                    }
                    if (!element) ASTRA_UNLIKELY
                        return false;                      // sparse-slot cap refused it
                    m_signalManager.Emit<Events::ComponentAdded>(entity, id, SparseTypedPtr<T>(element));
                    return true;
                }
            }
```

2. In `EmplaceComponent<T, Args...>`, insert the same block after its validity check and Task 12's Universal arm, with one change. Replace `T value(component);` with `T value(std::forward<Args>(args)...);`. The tag branch keeps `nullptr` and ignores `args`.

3. In `RemoveComponent<T>`, insert directly after Task 12's Universal arm (which sits directly after `AssertContextAffinity();`):

```cpp
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    const ComponentID id = TypeID<T>::Value();
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    SparseSet* set = m_componentStorage->Sparse(id);
                    if (!rec || !rec->chunk || !set || !set->Contains(entity))
                        return false;                      // stale handle or absent
                    if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved)) ASTRA_UNLIKELY
                    {
                        // BEFORE the erase, as for Table: the pointer is valid only
                        // for the handler's duration.
                        m_signalManager.Emit<Events::ComponentRemoved>(entity, id, SparseTypedPtr<T>(set->Get(entity)));
                        // A handler may have moved or destroyed the entity: re-validate.
                        rec = m_archetypeManager->GetEntityRecord(entity);
                        if (!rec || !rec->chunk || !set->Contains(entity))
                            return false;
                    }
                    return SparseDetach(entity, *rec, id);
                }
            }
```

- [ ] **Step 7: Implement `ValidateStorageInvariants`**

Add the public method after `Defragment(const DefragmentationOptions&)`:

```cpp
        // Storage policies (spec s5.2): exhaustive check of
        //   P1 presence -- chunk present bit <=> sparse-set membership, both directions;
        //   P2 counts   -- presentCount/disabledCount == popcount of their words;
        //   P3 mirror   -- chunk disabled bit <=> the store's disabled flag
        //                  (enableable Sparse and Universal);
        // plus the tail rule (no bit at a chunk slot >= count). Returns "" when all
        // hold, else one line per violation (first 32). O(entities x sparse slots):
        // for tests and the invariant fuzzer, never a hot path.
        ASTRA_NODISCARD std::string ValidateStorageInvariants() const
        {
            std::string out;
            size_t violations = 0;
            auto fail = [&](const char* what, Entity e, uint16_t slot, size_t index)
            {
                if (++violations > 32) return;
                out += what;
                out += " (entity ";
                out += e.IsValid() ? std::to_string(e.GetID()) + "v" + std::to_string(e.GetVersion()) : std::string("-");
                out += ", slot " + std::to_string(slot) + ", index " + std::to_string(index) + ")\n";
            };
            ComponentStorage& storage = *m_componentStorage;

            // Chunk side: every block, every bit.
            for (Archetype* arch : m_archetypeManager->GetArchetypes())
            {
                for (const auto& chunkPtr : arch->GetChunks())
                {
                    const ArchetypeChunk& chunk = *chunkPtr;
                    const size_t count = chunk.GetCount();
                    const size_t capacity = chunk.GetCapacity();
                    const size_t words = (capacity + 63) / 64;
                    for (const ArchetypeChunk::SparseBlock& b : chunk.GetSparseBlocks())
                    {
                        const ComponentID id = storage.ComponentOfSlot(b.slot);
                        const StorageKind kind = id < MAX_COMPONENTS ? storage.KindOf(id) : StorageKind::Table;
                        uint32_t presentPop = 0, disabledPop = 0;
                        for (size_t w = 0; w < words; ++w)
                        {
                            if (b.present)  presentPop  += static_cast<uint32_t>(std::popcount(b.present[w]));
                            if (b.disabled) disabledPop += static_cast<uint32_t>(std::popcount(b.disabled[w]));
                        }
                        if (presentPop != b.presentCount)   fail("P2 presentCount != popcount(present)", Entity::Invalid(), b.slot, 0);
                        if (disabledPop != b.disabledCount) fail("P2 disabledCount != popcount(disabled)", Entity::Invalid(), b.slot, 0);
                        if (kind == StorageKind::Table)     fail("presence block for a slot with no Sparse/Universal component", Entity::Invalid(), b.slot, 0);
                        if (kind == StorageKind::Universal && b.present) fail("present words on a Universal mirror block", Entity::Invalid(), b.slot, 0);

                        for (size_t i = 0; i < capacity; ++i)
                        {
                            const bool p = chunk.IsSparsePresent(b.slot, i);
                            const bool d = chunk.IsSparseDisabled(b.slot, i);
                            if (i >= count)
                            {
                                if (p || d) fail("bit set at a chunk slot >= count", Entity::Invalid(), b.slot, i);
                                continue;
                            }
                            const Entity e = chunk.GetEntity(i);
                            if (kind == StorageKind::Sparse)
                            {
                                SparseSet* set = storage.Sparse(id);
                                const bool member = set && set->Contains(e);
                                if (p != member) fail("P1 presence bit != sparse membership", e, b.slot, i);
                                if (d && !p)     fail("P3 disabled bit on a non-member", e, b.slot, i);
                                if (member && d != set->IsDisabled(e)) fail("P3 disabled mirror != sparse disabled flag", e, b.slot, i);
                            }
                            else if (kind == StorageKind::Universal)
                            {
                                UniversalColumn* col = storage.Universal(id);
                                if (col && d != col->IsDisabled(e)) fail("P3 disabled mirror != universal disabled flag", e, b.slot, i);
                            }
                        }
                    }
                }
            }

            // Store side: every member is a live, located entity whose bits agree.
            for (uint16_t slot = 0; slot < ASTRA_MAX_SPARSE_COMPONENTS; ++slot)
            {
                const ComponentID id = storage.ComponentOfSlot(slot);
                if (id >= MAX_COMPONENTS) continue;                 // unassigned slot
                const StorageKind kind = storage.KindOf(id);
                if (kind == StorageKind::Sparse)
                {
                    SparseSet* set = storage.Sparse(id);
                    if (!set) continue;
                    const bool enableable = set->Descriptor().isEnableable;
                    const Entity* members = set->Entities();
                    for (size_t k = 0; k < set->Size(); ++k)
                    {
                        const Entity e = members[k];
                        const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
                        if (!rec || !rec->chunk)
                        {
                            fail("P1 sparse member is not a live, located entity", e, slot, k);
                            continue;
                        }
                        const size_t i = rec->location.GetEntityIndex();
                        if (!rec->chunk->IsSparsePresent(slot, i)) fail("P1 sparse member without its presence bit", e, slot, i);
                        if (enableable && set->IsDisabled(e) != rec->chunk->IsSparseDisabled(slot, i))
                            fail("P3 sparse disabled flag without its mirror bit", e, slot, i);
                    }
                }
                else if (kind == StorageKind::Universal)
                {
                    UniversalColumn* col = storage.Universal(id);
                    if (!col) continue;
                    col->ForEachLive([&](Entity e, const void*)
                    {
                        const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
                        if (!rec || !rec->chunk)
                        {
                            fail("universal slot live for a dead entity", e, slot, 0);
                            return;
                        }
                        const size_t i = rec->location.GetEntityIndex();
                        if (col->IsDisabled(e) != rec->chunk->IsSparseDisabled(slot, i))
                            fail("P3 universal disabled flag without its mirror bit", e, slot, i);
                    });
                }
            }

            if (violations > 32)
                out += "... " + std::to_string(violations - 32) + " more violation(s)\n";
            return out;
        }
```

- [ ] **Step 8: Run the new suite and confirm it passes**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseStructure.*
```

Expected: `[  PASSED  ] 10 tests.`

If `CreateWithASparseMemberSetsItsBit` fails on `PresentBit`, a Task 11 creation path still inserts directly: return to Step 5.4. If it fails on `HasComponent`, Task 11 did not split Sparse members out of the creation pack (spec s7.1). Add that split there before continuing.

- [ ] **Step 9: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 10: Commit**

```powershell
git add include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp tests/Registry/SparseStructureTestComponents.hpp tests/Registry/SparseStructureTest.cpp
git commit -m "feat(storage): sparse add/remove through one funnel with presence bits; ValidateStorageInvariants P1-P3 (spec s7.2, s5.2)

Typed Add/Emplace/Remove of a Sparse component insert/erase the set and the
chunk bit without an archetype move; ComponentAdded/Removed keep Table timing;
creation paths share SparseAttach; m_sparseStructuralCounter feeds the
iteration guard.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 17: Carry presence bits through every relocation site (pins Review Focus 3)

Spec s5.3. Every site that relocates a chunk slot already carries disabled bits and ticks. Each one now also carries the sparse blocks, as follows.

| Site | Carry |
|---|---|
| `ArchetypeChunk::RemoveEntity` (swap-remove) | `SwapRemoveSparseBits` |
| `Archetype::MoveEntityFrom` | `CarrySparseBits` |
| `ArchetypeManager::MoveAndAdd<T>` | `CarrySparseBits` |
| `ArchetypeManager::MoveAndAddByID` | `CarrySparseBits` |
| the `BatchMoveComponentsFrom` call site in `Archetype::BatchMoveEntitiesFrom` | `CarrySparseBits` |
| `Archetype::CompactChunks` | `CarrySparseBits` |

Task 4 deleted `MoveEntitiesBetweenChunks`, so it has no carry to add. The destroy path's swap-remove is the same `ArchetypeChunk::RemoveEntity`. Task 21 adds the store-side erase in front of it.

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`RemoveEntity`)
- Modify: `include/Astra/Archetype/Archetype.hpp` (`MoveEntityFrom`, `BatchMoveEntitiesFrom`, `CompactChunks`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`MoveAndAdd<T>`, `MoveAndAddByID`)
- Create: `tests/Registry/SparseCarryTest.cpp`

**Interfaces:** No new API. This task consumes Task 15's `CarrySparseBits`, `SwapRemoveSparseBits` and `HasSparseBlocks`.

- [ ] **Step 1: Write the failing tests, including the Review Focus 3 pin**

Create `tests/Registry/SparseCarryTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

// Review Focus 3 (pin): the swap-remove moves the TAIL entity into the vacated
// slot. The removed slot and the tail carry DIFFERENT sparse memberships, so a
// missing or partial carry leaves the wrong bits at the vacated slot.
TEST(SparseCarry, SwapRemoveCarriesTheTailsMembershipIntoTheVacatedSlot)
{
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntityWith(Position{});
    const Astra::Entity e1 = reg.CreateEntityWith(Position{});
    const Astra::Entity e2 = reg.CreateEntityWith(Position{});
    ASSERT_EQ(Locate(reg, e0).second, 0u);
    ASSERT_EQ(Locate(reg, e2).second, 2u);
    reg.AddComponent<SpA>(e0, SpA{1.0f});   // removed slot: {SpA}
    reg.AddComponent<SpB>(e2, SpB{2});      // tail:         {SpB}

    reg.AddComponent<Velocity>(e0, Velocity{});   // e0 leaves; e2 swaps into slot 0

    auto [oldChunk, idx2] = Locate(reg, e2);
    ASSERT_EQ(idx2, 0u);
    EXPECT_TRUE(oldChunk->IsSparsePresent(static_cast<uint16_t>(SlotOf<SpB>(reg)), 0));
    EXPECT_FALSE(oldChunk->IsSparsePresent(static_cast<uint16_t>(SlotOf<SpA>(reg)), 0));
    EXPECT_FALSE(oldChunk->IsSparsePresent(static_cast<uint16_t>(SlotOf<SpB>(reg)), 2));   // tail cleared
    EXPECT_TRUE(PresentBit<SpA>(reg, e0));      // e0's membership travelled with it
    EXPECT_TRUE(reg.HasComponent<SpA>(e0));
    EXPECT_TRUE(reg.HasComponent<SpB>(e2));
    EXPECT_FALSE(reg.HasComponent<SpA>(e1));
    ExpectInvariants(reg);
}

// Review Focus 3 (pin): the removed slot IS the tail -- only the tail clear runs,
// dropping exactly the removed entity's bits and nobody else's.
TEST(SparseCarry, SwapRemoveOfTheTailDropsOnlyItsOwnBits)
{
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntityWith(Position{});
    const Astra::Entity e1 = reg.CreateEntityWith(Position{});
    const Astra::Entity e2 = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpB>(e1, SpB{1});
    reg.AddComponent<SpA>(e2, SpA{});
    reg.AddComponent<SpB>(e2, SpB{2});
    auto [chunk, idx] = Locate(reg, e2);
    ASSERT_EQ(idx, 2u);

    reg.AddComponent<Velocity>(e2, Velocity{});   // the tail leaves; nothing swaps

    EXPECT_EQ(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<SpA>(reg)))->presentCount, 0u);
    EXPECT_EQ(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<SpB>(reg)))->presentCount, 1u);
    EXPECT_TRUE(PresentBit<SpB>(reg, e1));
    EXPECT_FALSE(PresentBit<SpA>(reg, e0));
    EXPECT_TRUE(PresentBit<SpA>(reg, e2));
    EXPECT_TRUE(PresentBit<SpB>(reg, e2));
    ExpectInvariants(reg);
}

TEST(SparseCarry, TableRemoveCarriesThroughMoveEntityFrom)
{
    Astra::Registry reg;
    const Astra::Entity a = reg.CreateEntityWith(Position{}, Velocity{});
    const Astra::Entity b = reg.CreateEntityWith(Position{}, Velocity{});
    reg.AddComponent<SpA>(a, SpA{5.0f});
    reg.AddComponent<SpB>(b, SpB{6});
    EXPECT_TRUE(reg.RemoveComponent<Velocity>(a));
    EXPECT_TRUE(PresentBit<SpA>(reg, a));
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(a)->v, 5.0f);
    EXPECT_TRUE(PresentBit<SpB>(reg, b));
    ExpectInvariants(reg);
}

TEST(SparseCarry, ByIdAddCarriesThroughMoveAndAddByID)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<Velocity>();
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(a, SpA{});
    reg.AddComponent<SpB>(b, SpB{});
    const Velocity v{1, 2, 3};
    ASSERT_TRUE(reg.AddComponentByID(a, Astra::TypeID<Velocity>::Value(), &v, sizeof(Velocity)));
    EXPECT_TRUE(PresentBit<SpA>(reg, a));
    EXPECT_TRUE(PresentBit<SpB>(reg, b));   // b was swapped into a's old slot
    ExpectInvariants(reg);
}
```

Continue `tests/Registry/SparseCarryTest.cpp`:

```cpp
// Batch move OUT OF THE ROOT archetype: the source and destination share no
// column (componentsToMove is empty), and presence must still follow the entity.
TEST(SparseCarry, BatchAddFromRootCarriesWithNoSharedColumn)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(300);
    reg.CreateEntities<>(300, es);
    for (size_t i = 0; i < es.size(); i += 2)
        reg.AddComponent<SpA>(es[i], SpA{float(i)});

    reg.AddComponents<Position>(std::span<Astra::Entity>(es.data(), es.size()), Position{});

    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), i % 2 == 0) << i;
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 2 == 0) << i;
    }
    ExpectInvariants(reg);
}

TEST(SparseCarry, BatchRemoveWithSharedColumnsCarries)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(500);
    reg.CreateEntities<Position, Velocity>(500, es);
    for (size_t i = 0; i < es.size(); i += 3)
        reg.AddComponent<SpB>(es[i], SpB{int(i)});

    // Remove Velocity from every second entity: batch moves interleaved with swap-removes.
    std::vector<Astra::Entity> half;
    for (size_t i = 0; i < es.size(); i += 2) half.push_back(es[i]);
    EXPECT_EQ(reg.RemoveComponents<Velocity>(std::span<Astra::Entity>(half.data(), half.size())), half.size());

    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(PresentBit<SpB>(reg, es[i]), i % 3 == 0) << i;
        if (i % 3 == 0) EXPECT_EQ(reg.GetComponent<SpB>(es[i])->v, int(i)) << i;
    }
    ExpectInvariants(reg);
}

TEST(SparseCarry, DefragmentCompactionCarries)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(3000);
    reg.CreateEntities<Position>(3000, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 3 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 5 == 0) reg.AddComponent<SpB>(es[i], SpB{int(i)});
    }
    ASSERT_GT(reg.FindArchetype<Position>()->GetChunkCount(), 1u);

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;   // compact every multi-chunk archetype
    const auto result = reg.Defragment(opts);
    EXPECT_GT(result.entitiesMoved, 0u);

    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), i % 3 == 0) << i;
        EXPECT_EQ(PresentBit<SpB>(reg, es[i]), i % 5 == 0) << i;
    }
    ExpectInvariants(reg);
}
```

- [ ] **Step 2: Regenerate the projects and run the tests to verify they fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseCarry.*
```

Expected: the build succeeds and all 7 tests FAIL. Every `ExpectInvariants` reports violations such as `P1 presence bit != sparse membership` and `bit set at a chunk slot >= count`. `SwapRemoveCarriesTheTailsMembershipIntoTheVacatedSlot` also fails `EXPECT_TRUE(oldChunk->IsSparsePresent(... SpB ..., 0))`.

- [ ] **Step 3: Swap-remove carry in `ArchetypeChunk::RemoveEntity`**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, `RemoveEntity`, insert directly before `// Remove last entity` / `m_entities.pop_back();`:

```cpp
            // Sparse presence carry (storage policies, spec s5.3): the tail's bits
            // fill the vacated slot, then the tail is cleared. With index == lastIndex
            // this only clears the removed entity's own bits.
            if (!m_sparseBlocks.empty()) ASTRA_UNLIKELY
                SwapRemoveSparseBits(index, lastIndex);
```

- [ ] **Step 4: Carry in `Archetype::MoveEntityFrom`**

In `include/Astra/Archetype/Archetype.hpp`, `MoveEntityFrom`, append after the merge-join `while` loop and before the closing brace. Presence follows the entity whatever the column sets are, so this sits outside the loop. The source bits are cleared by the caller's swap-remove of the source.

```cpp
            // Sparse presence carry (storage policies, spec s5.3).
            if (srcChunk->HasSparseBlocks()) ASTRA_UNLIKELY
                dstChunk->CarrySparseBits(*srcChunk, srcEntityIndex, dstEntityIndex);
```

- [ ] **Step 5: Carry in `MoveAndAdd<T>` and `MoveAndAddByID`**

In `include/Astra/Archetype/ArchetypeManager.hpp`, append the same block after the column `for` loop of BOTH functions. The variable names are the ones both functions already use.

```cpp
            // Sparse presence carry (storage policies, spec s5.3). The added
            // component is a Table column (Sparse adds never move the entity).
            if (srcChunk->HasSparseBlocks()) ASTRA_UNLIKELY
                dstChunk->CarrySparseBits(*srcChunk, srcEntityIdx, dstEntityIdx);
```

- [ ] **Step 6: Carry in `Archetype::BatchMoveEntitiesFrom`**

The spec table names `BatchMoveComponentsFrom`. Its caller returns early when the two archetypes share no column (`componentsToMove.None()`, for example a batch leaving the root archetype), and that path still relocates entities. So the carry goes at the call site, where it runs for every chunk pair.

In `include/Astra/Archetype/Archetype.hpp`, `BatchMoveEntitiesFrom`, replace everything from `// Calculate components to move (only shared components)` through the end of the function with:

```cpp
            // Calculate components to move (only shared components)
            // This is the intersection of source and destination masks
            ComponentMask componentsToMove = srcArchetype.GetMask() & GetMask();

            // Per chunk pair: carry sparse presence (spec s5.3) -- ALWAYS, because
            // presence follows the entity even when no column is shared (a batch
            // leaving the root archetype) -- then move the shared columns.
            for (auto& [key, batch] : batches)
            {
                // Validate chunk indices
                if (batch.srcChunkIndex >= srcArchetype.m_chunks.size() || batch.dstChunkIndex >= m_chunks.size())
                {
                    ASTRA_ASSERT(false, "Invalid chunk index in batch move");
                    continue;
                }

                auto& srcChunk = srcArchetype.m_chunks[batch.srcChunkIndex];
                auto& dstChunk = m_chunks[batch.dstChunkIndex];

                if (srcChunk->HasSparseBlocks()) ASTRA_UNLIKELY
                {
                    for (size_t i = 0; i < batch.srcIndices.size(); ++i)
                        dstChunk->CarrySparseBits(*srcChunk, batch.srcIndices[i], batch.dstIndices[i]);
                }

                if (componentsToMove.None())
                    continue;   // e.g. moving from the root archetype: no column to move

                dstChunk->BatchMoveComponentsFrom(batch.dstIndices, *srcChunk, batch.srcIndices, componentsToMove);
            }

            return dstLocations;
        }
```

- [ ] **Step 7: Carry in `Archetype::CompactChunks`**

Inside the per-run loop, after the change-detection `FoldColumnVersion` loop and before `dst->SetCount(dst->GetCount() + run);`, insert the block below. The old chunks, and their blocks, are freed wholesale when `m_chunks` is replaced, so the source needs no clear.

```cpp
                    // Sparse presence carry (storage policies, spec s5.3): a block is
                    // re-created in the fresh chunk only for a slot some moved entity
                    // holds, so compaction also drops emptied blocks.
                    if (src->HasSparseBlocks()) ASTRA_UNLIKELY
                    {
                        for (size_t k = 0; k < run; ++k)
                            dst->CarrySparseBits(*src, srcIndex + k, dstIndex + k);
                    }
```

- [ ] **Step 8: Run the suite and confirm it passes**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseCarry.*:SparseStructure.*:SparseBlockTest.*
```

Expected: `[  PASSED  ] 29 tests.`

- [ ] **Step 9: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. Record the seed line.

- [ ] **Step 10: Commit**

```powershell
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp tests/Registry/SparseCarryTest.cpp
git commit -m "feat(storage): carry sparse presence bits through every slot relocation (spec s5.3)

RemoveEntity swap-remove, MoveEntityFrom, MoveAndAdd, MoveAndAddByID, the
BatchMoveEntitiesFrom chunk-pair loop (incl. the no-shared-column path out of
the root archetype) and CompactChunks. Pins Review Focus 3: differing
memberships across a swap-remove, and removed slot == tail.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 18: Sparse by-id and batch routing (one archetype move for the Table part)

Spec s7.2 and s3.3. The routes are:
- **Type-erased paths.** `AddComponentByID` and `RemoveComponentByID` serve the CommandBuffer executors. `AddComponentsByID` and `RemoveComponentsByID` are the batch forms.
- **Typed batches.** `AddComponents<T>`, `EmplaceComponents<T>` and `RemoveComponents<T>` route a Sparse `T` to set operations with the Table batch's exact signal rules.

Astra has no add or remove API that takes several component types at once. A "mixed batch" therefore means two cases:
- a creation pack, where Task 11 places the Table subset once;
- a CommandBuffer that touches Table and Sparse components of one entity.

In both cases the Table components cause exactly the archetype moves they cause today, and the Sparse components cause none. The tests pin this through the archetype set.

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp`
- Create: `tests/Registry/SparseBatchTest.cpp`

**Interfaces:** This task adds these private Registry helpers. No public API changes.
- `bool SparseAddByID(Entity, ComponentID, const void* data, size_t dataSize, bool emit)`
- `bool SparseRemoveByID(Entity, ComponentID, bool emit)`
- `static void AssignErased(const ComponentDescriptor&, void* dst, const void* src)`
- `template<Component T, class MakeFn> void SparseBatchAdd(std::span<const Entity>, MakeFn&&)`
- `template<Component T> size_t SparseBatchRemove(std::span<Entity>)`

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/SparseBatchTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <string>
#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

TEST(SparseBatch, AddComponentByIDRoutesToTheSet)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpA>();
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int fired = 0;
    void* addedPtr = nullptr;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { ++fired; addedPtr = ev.component; });

    const Astra::Entity e = reg.CreateEntityWith(Position{});
    const Astra::Archetype* arch = reg.GetArchetypeManager()->GetEntityRecord(e)->archetype;
    const SpA value{7.0f};
    const Astra::ComponentID id = Astra::TypeID<SpA>::Value();
    EXPECT_TRUE(reg.AddComponentByID(e, id, &value, sizeof(SpA)));
    EXPECT_FALSE(reg.AddComponentByID(e, id, &value, sizeof(SpA)));   // by-id: present -> false (Table parity)
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, arch);
    ASSERT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 7.0f);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(addedPtr, static_cast<void*>(reg.GetComponent<SpA>(e)));
    EXPECT_FALSE(reg.AddComponentByID(reg.CreateEntity(), id, &value, 3));   // size mismatch refused
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseBatch, ByIdCopiesANonTrivialValue)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpStr>();
    const Astra::Entity e = reg.CreateEntity();
    const SpStr source{std::string("by-id")};
    ASSERT_TRUE(reg.AddComponentByID(e, Astra::TypeID<SpStr>::Value(), &source, sizeof(SpStr)));
    ASSERT_NE(reg.GetComponent<SpStr>(e), nullptr);
    EXPECT_EQ(reg.GetComponent<SpStr>(e)->s, "by-id");
    EXPECT_EQ(source.s, "by-id");   // copied, not moved from
}

TEST(SparseBatch, RemoveComponentByIDSignalsBeforeRemoval)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);
    bool presentInHandler = false;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev) { presentInHandler = reg.HasComponent<SpA>(ev.entity); });
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{});
    EXPECT_TRUE(reg.RemoveComponentByID(e, Astra::TypeID<SpA>::Value()));
    EXPECT_TRUE(presentInHandler);
    EXPECT_FALSE(reg.HasComponent<SpA>(e));
    EXPECT_FALSE(reg.RemoveComponentByID(e, Astra::TypeID<SpA>::Value()));
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(h);
}

TEST(SparseBatch, ByIdTagUsesTheEmptySentinel)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpTag>();
    reg.EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    void* addedPtr = nullptr;
    void* removedPtr = nullptr;
    auto ha = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    auto hr = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev) { removedPtr = ev.component; });
    const Astra::Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.AddComponentByID(e, Astra::TypeID<SpTag>::Value(), nullptr, 0));
    EXPECT_EQ(addedPtr, Astra::EmptyComponentSentinel());
    EXPECT_TRUE(PresentBit<SpTag>(reg, e));   // routed to the set, not into an archetype mask
    ASSERT_TRUE(reg.RemoveComponentByID(e, Astra::TypeID<SpTag>::Value()));
    EXPECT_EQ(removedPtr, Astra::EmptyComponentSentinel());
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(ha);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(hr);
}

TEST(SparseBatch, ByIdBatchesCountSuccesses)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpB>();
    std::vector<Astra::Entity> es(10);
    reg.CreateEntities<Position>(10, es);
    for (size_t i = 0; i < 3; ++i) reg.AddComponent<SpB>(es[i], SpB{});
    const SpB v{4};
    const Astra::ComponentID id = Astra::TypeID<SpB>::Value();
    EXPECT_EQ(reg.AddComponentsByID(std::span<Astra::Entity>(es.data(), es.size()), id, &v, sizeof(SpB)), 7u);
    EXPECT_EQ(reg.RemoveComponentsByID(std::span<Astra::Entity>(es.data(), es.size()), id), 10u);
    for (Astra::Entity e : es) EXPECT_FALSE(reg.HasComponent<SpB>(e));
    ExpectInvariants(reg);
}
```

Continue `tests/Registry/SparseBatchTest.cpp`:

```cpp
TEST(SparseBatch, TypedBatchAddFollowsTheTableSignalRule)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int fired = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded&) { ++fired; });
    std::vector<Astra::Entity> es(4);
    reg.CreateEntities<Position>(4, es);
    reg.AddComponent<SpA>(es[0], SpA{1.0f});
    fired = 0;

    std::vector<Astra::Entity> batch{es[0], es[1], es[2], es[3], es[1]};   // a duplicate
    reg.AddComponents<SpA>(std::span<Astra::Entity>(batch.data(), batch.size()), SpA{2.0f});

    // Table parity (Registry::AddComponents): after the pass, ComponentAdded fires
    // once per de-duplicated entity that HOLDS T -- the pre-existing holder included.
    EXPECT_EQ(fired, 4);
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(es[0])->v, 1.0f);   // present: untouched
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(es[3])->v, 2.0f);
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseBatch, TypedEmplaceAndRemoveBatches)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(64);
    reg.CreateEntities<Position>(64, es);
    reg.EmplaceComponents<SpStr>(std::span<Astra::Entity>(es.data(), es.size()), std::string("x"));
    for (Astra::Entity e : es) EXPECT_EQ(reg.GetComponent<SpStr>(e)->s, "x");

    std::vector<Astra::Entity> evens;
    for (size_t i = 0; i < es.size(); i += 2) evens.push_back(es[i]);
    EXPECT_EQ(reg.RemoveComponents<SpStr>(std::span<Astra::Entity>(evens.data(), evens.size())), evens.size());
    for (size_t i = 0; i < es.size(); ++i) EXPECT_EQ(reg.HasComponent<SpStr>(es[i]), i % 2 == 1) << i;
    ExpectInvariants(reg);
}

TEST(SparseBatch, CommandBufferMixedTableAndSparseMovesOnlyForTheTablePart)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpA>();
    reg.GetComponentRegistry()->RegisterComponent<Velocity>();
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    const size_t archetypes = reg.GetArchetypeCount();

    Astra::CommandBuffer cmd(&reg);
    cmd.AddComponent(e, SpA{3.0f});
    cmd.AddComponent(e, Velocity{1, 1, 1});
    cmd.Execute();

    EXPECT_EQ(reg.GetArchetypeCount(), archetypes + 1);   // only {Position, Velocity} appeared
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, (reg.FindArchetype<Position, Velocity>()));
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 3.0f);
    ExpectInvariants(reg);

    Astra::CommandBuffer cmd2(&reg);
    cmd2.RemoveComponent<SpA>(e);
    cmd2.Execute();
    EXPECT_FALSE(reg.HasComponent<SpA>(e));
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, (reg.FindArchetype<Position, Velocity>()));
    ExpectInvariants(reg);
}

TEST(SparseBatch, MixedCreationPlacesTheTableSubsetOnce)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{}, SpA{1.0f}, Velocity{}, SpB{2});
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, (reg.FindArchetype<Position, Velocity>()));
    EXPECT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_TRUE(reg.HasComponent<SpB>(e));
    ExpectInvariants(reg);
}
```

- [ ] **Step 2: Regenerate the projects and run the tests to verify they fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseBatch.*
```

Expected: the build succeeds. `MixedCreationPlacesTheTableSubsetOnce` already passes, because Task 11 owns creation. The other 8 tests FAIL. The Sparse id still reaches the Table paths:
- `ArchetypeManager::AddComponentByID` and the typed batch moves send the entity into a new archetype whose mask holds the Sparse id.
- The set never sees the value, so the archetype, `HasComponent`, `PresentBit` and count checks fail.

If Task 11's `ConstructComponentAt` "component has no column" assert trips first in Debug, the run aborts. That abort is an equally valid red.

- [ ] **Step 3: Add the by-id and batch helpers**

In `include/Astra/Registry/Registry.hpp`, add these to the private Sparse funnel section after `SparseDetach`:

```cpp
        // Overwrites a default-constructed element with the type-erased source,
        // using MoveAndAddByID's construction ladder (memcpy / constructWith / copy /
        // move-out-of-buffer for a move-only type / default).
        static void AssignErased(const ComponentDescriptor& desc, void* dst, const void* src)
        {
            if (desc.is_trivially_copyable)
            {
                std::memcpy(dst, src, desc.size);
                return;
            }
            desc.Destruct(dst);
            if (desc.constructWith)       desc.constructWith(dst, src);
            else if (desc.copyConstruct)  desc.copyConstruct(dst, src);
            else if (desc.moveConstruct)  desc.moveConstruct(dst, const_cast<void*>(src));
            else                          desc.DefaultConstruct(dst);
        }

        // By-id Sparse add with ArchetypeManager::AddComponentByID's contract:
        // false for an unknown id, a size mismatch, a stale handle, or an id the
        // entity already holds. `emit` gates ComponentAdded (the batch form never emits).
        bool SparseAddByID(Entity entity, ComponentID id, const void* data, size_t dataSize, bool emit)
        {
            const ComponentDescriptor* desc = m_componentRegistry->GetComponentDescriptor(id);
            if (!desc) ASTRA_UNLIKELY
                return false;
            if (dataSize != desc->size && desc->size > 0) ASTRA_UNLIKELY
                return false;
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return false;
            if (SparseSet* set = m_componentStorage->Sparse(id); set && set->Contains(entity))
                return false;
            void* element = SparseAttach(entity, *rec, id, nullptr);   // default-constructed
            if (!element) ASTRA_UNLIKELY
                return false;
            if (desc->size > 0 && data)
                AssignErased(*desc, element, data);
            if (emit && m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
                m_signalManager.Emit<Events::ComponentAdded>(entity, id, element);
            return true;
        }

        // By-id Sparse remove: ComponentRemoved (when `emit`) BEFORE the erase, with
        // the element pointer (sentinel for a tag); false when stale or absent.
        bool SparseRemoveByID(Entity entity, ComponentID id, bool emit)
        {
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            SparseSet* set = m_componentStorage->Sparse(id);
            if (!rec || !rec->chunk || !set || !set->Contains(entity))
                return false;
            if (emit && m_signalManager.IsSignalEnabled(Signal::ComponentRemoved)) ASTRA_UNLIKELY
            {
                void* element = set->Descriptor().size == 0 ? EmptyComponentSentinel() : set->Get(entity);
                m_signalManager.Emit<Events::ComponentRemoved>(entity, id, element);
                rec = m_archetypeManager->GetEntityRecord(entity);   // a handler may have moved/destroyed it
                if (!rec || !rec->chunk || !set->Contains(entity))
                    return false;
            }
            return SparseDetach(entity, *rec, id);
        }

        // Typed batch add over de-duplicated live entities. Absent -> attach a fresh
        // value from make(); present -> untouched. Then ComponentAdded for every entity
        // HOLDING T, which is Registry::AddComponents' exact post-pass rule for Table.
        template<Component T, typename MakeFn>
        void SparseBatchAdd(std::span<const Entity> entities, MakeFn&& make)
        {
            const ComponentID id = TypeID<T>::Value();
            for (Entity e : entities)
            {
                const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
                if (!rec || !rec->chunk) ASTRA_UNLIKELY
                    continue;
                if (SparseSet* set = m_componentStorage->Sparse(id); set && set->Contains(e))
                    continue;
                if constexpr (std::is_empty_v<T>)
                {
                    SparseAttach(e, *rec, id, nullptr);
                }
                else
                {
                    T value = make();
                    SparseAttach(e, *rec, id, &value);
                }
            }
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                SparseSet* set = m_componentStorage->Sparse(id);
                for (Entity e : entities)
                    if (set && set->Contains(e))
                        m_signalManager.Emit<Events::ComponentAdded>(e, id, SparseTypedPtr<T>(set->Get(e)));
            }
        }

        // Typed batch remove, mirroring Registry::RemoveComponents for Table:
        // ComponentRemoved fires per occurrence BEFORE anything is erased (a
        // duplicated entity double-signals, the documented Table quirk); erasure
        // runs once per de-duplicated member. Returns the number removed.
        template<Component T>
        size_t SparseBatchRemove(std::span<Entity> entities)
        {
            const ComponentID id = TypeID<T>::Value();
            SparseSet* set = m_componentStorage->Sparse(id);
            if (!set) ASTRA_UNLIKELY
                return 0;
            const bool emit = m_signalManager.IsSignalEnabled(Signal::ComponentRemoved);
            SmallVector<Entity, 256> members;
            members.reserve(entities.size());
            for (Entity e : entities)
            {
                if (!m_entityManager.IsValid(e) || !set->Contains(e))
                    continue;
                if (emit) ASTRA_UNLIKELY
                    m_signalManager.Emit<Events::ComponentRemoved>(e, id, SparseTypedPtr<T>(set->Get(e)));
                members.push_back(e);
            }
            std::sort(members.begin(), members.end());
            members.erase(std::unique(members.begin(), members.end()), members.end());
            size_t removed = 0;
            for (Entity e : members)
                if (const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e); rec && rec->chunk)
                    removed += SparseDetach(e, *rec, id) ? 1u : 0u;
            return removed;
        }
```

- [ ] **Step 4: Insert the Sparse arms**

The by-id arms dispatch on the runtime kind. Insert each one directly AFTER Task 12's Universal arm, which must stay the first statement of its function (Chunk B's ordering rule). The kinds are exclusive, so behaviour does not depend on the order; the rule only keeps Task 12's guard visibly first.

`AddComponentByID`:

```cpp
            if (componentId < MAX_COMPONENTS && m_componentStorage->KindOf(componentId) == StorageKind::Sparse) ASTRA_UNLIKELY
                return SparseAddByID(entity, componentId, data, dataSize, /*emit*/ true);
```

`RemoveComponentByID`:

```cpp
            if (componentId < MAX_COMPONENTS && m_componentStorage->KindOf(componentId) == StorageKind::Sparse) ASTRA_UNLIKELY
                return SparseRemoveByID(entity, componentId, /*emit*/ true);
```

`AddComponentsByID`. This form documents "no signals":

```cpp
            if (componentId < MAX_COMPONENTS && m_componentStorage->KindOf(componentId) == StorageKind::Sparse) ASTRA_UNLIKELY
            {
                size_t added = 0;
                for (Entity e : entities)
                    added += SparseAddByID(e, componentId, data, dataSize, /*emit*/ false) ? 1u : 0u;
                return added;
            }
```

`RemoveComponentsByID`:

```cpp
            if (componentId < MAX_COMPONENTS && m_componentStorage->KindOf(componentId) == StorageKind::Sparse) ASTRA_UNLIKELY
            {
                size_t removed = 0;
                for (Entity e : entities)
                    removed += SparseRemoveByID(e, componentId, /*emit*/ false) ? 1u : 0u;
                return removed;
            }
```

The typed batches are compile-time gated and cost Table-trait types nothing.

`AddComponents<T>`. Insert directly after its `if (validEntities.empty()) return;`:

```cpp
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    SparseBatchAdd<T>(std::span<const Entity>(validEntities.data(), validEntities.size()),
                                      [&component] { return T(component); });
                    return;
                }
            }
```

`EmplaceComponents<T, Args...>`. Insert directly after its `if (validEntities.empty()) return;`. One value is built from `args` and copied to each entity, exactly as `BatchMoveEntitiesWithComponent` does for Table.

```cpp
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    const T prototype(std::forward<Args>(args)...);
                    SparseBatchAdd<T>(std::span<const Entity>(validEntities.data(), validEntities.size()),
                                      [&prototype] { return T(prototype); });
                    return;
                }
            }
```

`RemoveComponents<T>`. Insert directly after `if (entities.empty()) return 0;`:

```cpp
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                    return SparseBatchRemove<T>(entities);
            }
```

- [ ] **Step 5: Run the suites and confirm they pass**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseBatch.*:SparseStructure.*:SparseCarry.*
```

Expected: `[  PASSED  ] 26 tests.`

- [ ] **Step 6: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. `tests/Commands/CommandBufferTest.cpp` is unchanged and green. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Registry/SparseBatchTest.cpp
git commit -m "feat(storage): sparse routing for the by-id and batch add/remove paths (spec s7.2)

AddComponentByID/RemoveComponentByID (CommandBuffer), their batch forms, and the
typed AddComponents/EmplaceComponents/RemoveComponents route Sparse ids to set
operations with the Table paths' signal rules; Table parts of a mixed buffer or
creation pack move once, Sparse parts never.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 19: `SetEnabled`/`IsEnabled` for Sparse and Universal, with the chunk disabled mirror

Spec s7.2 (`SetEnabledByID` routing), s5.1 and s5.2 (P3), s3.3 (`SetEnabled` row).

The store is the truth: either the `SparseSet` per-element flag or the `UniversalColumn` per-slot flag. On a genuine change, the chunk's `disabled` bit for the component's slot is updated, and that update is the mirror.

An enableable Universal component gets a sparse slot on its first disable. Its blocks have `present == nullptr`.

The typed `SetEnabled<T>` and `IsEnabled<T>`, and the CommandBuffer's `SetEnabled`, already delegate to the by-id bodies, so routing those two bodies covers every entry point.

**This task SUPERSEDES Task 13's Universal enable code.** Task 13 inserted an `if (IsUniversalId(componentId))` arm at the top of `SetEnabledByID` and of `IsEnabledByID`, plus the private `SetEnabledUniversal` helper; they toggle only the column flag. Step 4 DELETES all three and routes every non-Table id (Sparse and Universal) through `SetEnabledInStore`, which also maintains the chunk `disabled` mirror. Task 13's tests (`UniversalStorage.SetEnabledTogglesTheColumnFlagAndSignalsOnChangeOnly`, `CommandBufferSetEnabledReachesTheColumn`, `DestroyRecycleBringsTheNextOccupantBackEnabled`, `UniversalStorageStaleHandle.SetEnabledMissesAndNeverTogglesTheOccupant`) must stay green unedited; Step 5 runs `UniversalStorage*` to prove it: the answers (live + enableable -> `true`; stale -> `false`; non-enableable universal -> `SetEnabledByID` `false`, `IsEnabledByID` `true`) are unchanged.

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp`
- Create: `tests/Registry/SparseEnableTest.cpp`

**Interfaces:** This task adds the private helper `bool SetEnabledInStore(Entity, const EntityRecord&, ComponentID, StorageKind, bool enable)`. Public behaviour follows the existing `SetEnabled` contract:
- `true` iff the entity holds the enableable component, whether or not the state changed;
- `false` for a stale handle, an absent component or a non-enableable component;
- signals fire only on a genuine change.

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/SparseEnableTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <utility>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

namespace
{
    Astra::Registry::Config UniversalUnE()
    {
        Astra::Registry::Config cfg;
        cfg.AddUniversal<UnE>();
        return cfg;
    }
}

TEST(SparseEnable, SparseToggleUpdatesTheStoreAndTheMirror)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpE>(e, SpE{1.0f});
    EXPECT_TRUE(reg.IsEnabled<SpE>(e));
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, false));
    EXPECT_FALSE(reg.IsEnabled<SpE>(e));
    EXPECT_TRUE(reg.HasComponent<SpE>(e));       // existence never lies
    EXPECT_TRUE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, true));
    EXPECT_TRUE(reg.IsEnabled<SpE>(e));
    EXPECT_FALSE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, SignalsFireOnGenuineChangeOnly)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentEnabled | Astra::Signal::ComponentDisabled);
    int disabled = 0, enabled = 0;
    void* payload = nullptr;
    auto hd = reg.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const Astra::Events::ComponentDisabled& ev) { ++disabled; payload = ev.component; });
    auto he = reg.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const Astra::Events::ComponentEnabled&) { ++enabled; });
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, false));
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, false));    // idempotent: true, silent
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, true));
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, true));
    EXPECT_EQ(disabled, 1);
    EXPECT_EQ(enabled, 1);
    EXPECT_EQ(payload, static_cast<void*>(reg.GetComponent<SpE>(e)));
    reg.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Unregister(hd);
    reg.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Unregister(he);
}

TEST(SparseEnable, AbsentInvalidOrNonEnableableIsRefused)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.GetComponentRegistry()->RegisterComponent<SpE>();
    EXPECT_FALSE(reg.SetEnabled<SpE>(e, false));                 // absent
    EXPECT_FALSE(reg.IsEnabled<SpE>(e));
    EXPECT_FALSE(reg.SetEnabled<SpE>(Astra::Entity::Invalid(), false));
    reg.AddComponent<SpA>(e, SpA{});
    const Astra::ComponentID a = Astra::TypeID<SpA>::Value();
    EXPECT_FALSE(reg.SetEnabledByID(e, a, false));               // present, not enableable
    EXPECT_TRUE(reg.IsEnabledByID(e, a));                        // present non-enableable: enabled
    ExpectInvariants(reg);
}

TEST(SparseEnable, SparseTagIsEnableable)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpTag>(e, SpTag{});
    EXPECT_TRUE(reg.SetEnabled<SpTag>(e, false));
    EXPECT_FALSE(reg.IsEnabled<SpTag>(e));
    EXPECT_TRUE(DisabledBit<SpTag>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, UniversalToggleUsesAMirrorBlockWithoutPresentWords)
{
    Astra::Registry reg(UniversalUnE());
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    ASSERT_TRUE(reg.HasComponent<UnE>(e));
    EXPECT_TRUE(reg.IsEnabled<UnE>(e));
    EXPECT_TRUE(reg.SetEnabled<UnE>(e, false));
    EXPECT_FALSE(reg.IsEnabled<UnE>(e));
    ASSERT_GE(SlotOf<UnE>(reg), 0);
    EXPECT_TRUE(DisabledBit<UnE>(reg, e));
    Astra::ArchetypeChunk* chunk = Locate(reg, e).first;
    EXPECT_EQ(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<UnE>(reg)))->present, nullptr);
    ExpectInvariants(reg);
    EXPECT_TRUE(reg.SetEnabled<UnE>(e, true));
    EXPECT_FALSE(DisabledBit<UnE>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, DisabledStateSurvivesTableMovesAndSwapRemoves)
{
    Astra::Registry reg(UniversalUnE());
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    const Astra::Entity c = reg.CreateEntityWith(Position{});
    for (Astra::Entity e : {a, b, c}) reg.AddComponent<SpE>(e, SpE{});
    reg.SetEnabled<SpE>(a, false);
    reg.SetEnabled<SpE>(c, false);           // the tail
    reg.SetEnabled<UnE>(c, false);

    reg.AddComponent<Velocity>(a, Velocity{});   // a leaves; c swaps into slot 0
    EXPECT_FALSE(reg.IsEnabled<SpE>(a));
    EXPECT_TRUE(reg.IsEnabled<SpE>(b));
    EXPECT_FALSE(reg.IsEnabled<SpE>(c));
    EXPECT_FALSE(reg.IsEnabled<UnE>(c));
    EXPECT_TRUE(DisabledBit<UnE>(reg, c));
    ExpectInvariants(reg);

    reg.RemoveComponent<Velocity>(a);
    EXPECT_FALSE(reg.IsEnabled<SpE>(a));
    ExpectInvariants(reg);
}

TEST(SparseEnable, CommandBufferSetEnabledRoutes)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    Astra::CommandBuffer cmd(&reg);
    cmd.SetEnabled<SpE>(e, false);
    cmd.Execute();
    EXPECT_FALSE(reg.IsEnabled<SpE>(e));
    EXPECT_TRUE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, RemovingADisabledMemberClearsTheMirrorAndReAddIsBornEnabled)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    reg.SetEnabled<SpE>(e, false);
    reg.RemoveComponent<SpE>(e);
    EXPECT_FALSE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
    reg.AddComponent<SpE>(e, SpE{});
    EXPECT_TRUE(reg.IsEnabled<SpE>(e));
    ExpectInvariants(reg);
}

// ---- Review Focus 1 (Sparse), registry level: Add / Remove / SetEnabled ----
// Extends Task 11's SparseStorageStaleHandle (Get/Has) now that all three route to the
// set. A handle whose id was recycled at version+1 is refused and never touches the
// new occupant's element or its disabled mirror.
TEST(SparseEnable, StaleHandleIsRefusedOnAddRemoveAndSetEnabled)
{
    Astra::Registry reg;
    const Astra::Entity stale = reg.CreateEntity();
    reg.AddComponent<SpE>(stale, SpE{1.0f});
    reg.DestroyEntity(stale);
    const Astra::Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());
    reg.AddComponent<SpE>(fresh, SpE{2.0f});

    EXPECT_FALSE(reg.SetEnabled<SpE>(stale, false));
    EXPECT_FALSE(reg.IsEnabled<SpE>(stale));
    EXPECT_FALSE(reg.RemoveComponent<SpE>(stale));
    EXPECT_FALSE(reg.AddComponent<SpE>(stale, SpE{9.0f}));
    ASSERT_TRUE(reg.HasComponent<SpE>(fresh));
    EXPECT_TRUE(reg.IsEnabled<SpE>(fresh));
    EXPECT_FALSE(DisabledBit<SpE>(reg, fresh));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<SpE>(fresh)->v, 2.0f);
    ExpectInvariants(reg);
}
```

- [ ] **Step 2: Regenerate the projects and run the tests to verify they fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseEnable.*
```

Expected: the build succeeds. These tests FAIL:
- the Sparse toggle tests: `SetEnabledByID` tests the archetype mask, which never holds a Sparse id, so `EXPECT_TRUE(reg.SetEnabled<SpE>(e, false))` fails;
- the Universal tests: there is no mirror, so `DisabledBit<UnE>` is false and `SlotOf<UnE>` is -1.

`AbsentInvalidOrNonEnableableIsRefused` and `StaleHandleIsRefusedOnAddRemoveAndSetEnabled` may pass already. That is fine: they pin the refusal table and Review Focus 1.

- [ ] **Step 3: Add the store-side toggle**

In `include/Astra/Registry/Registry.hpp`, add this to the private Sparse funnel section:

```cpp
        // Storage policies (spec s7.2): SetEnabled for a Sparse or Universal id. The
        // STORE flag is the truth; on a genuine change the chunk's disabled bit for
        // the component's slot is updated as the P3 mirror (an enableable Universal
        // component acquires its slot on first use and gets present == nullptr
        // blocks). Return/signal contract = the Table body's.
        bool SetEnabledInStore(Entity entity, const EntityRecord& rec, ComponentID id, StorageKind kind, bool enable)
        {
            const ComponentDescriptor* desc = m_componentRegistry->GetComponentDescriptor(id);
            if (!desc || !desc->isEnableable) ASTRA_UNLIKELY
                return false;                                      // absent or not enableable: nothing to toggle
            int slot = m_componentStorage->SparseSlotOf(id);
            bool changed = false;
            void* element = nullptr;
            if (kind == StorageKind::Sparse)
            {
                SparseSet* set = m_componentStorage->Sparse(id);
                if (!set || !set->Contains(entity))
                    return false;                                  // absent
                changed = set->SetDisabled(entity, !enable);       // SET == DISABLED
                element = desc->size == 0 ? EmptyComponentSentinel() : set->Get(entity);
            }
            else
            {
                UniversalColumn* col = m_componentStorage->Universal(id);
                if (!col || !col->IsLive(entity))
                    return false;
                if (slot < 0) ASTRA_UNLIKELY
                    slot = m_componentStorage->AcquireSparseSlot(id);
                if (slot < 0) ASTRA_UNLIKELY
                    return false;                                  // slot cap exhausted (AcquireSparseSlot asserts)
                changed = col->SetDisabled(entity, !enable);
                element = desc->size == 0 ? EmptyComponentSentinel() : col->Get(entity);
            }
            if (changed)
            {
                rec.chunk->SetSparseDisabled(static_cast<uint16_t>(slot), rec.location.GetEntityIndex(), !enable);
                const Signal flag = enable ? Signal::ComponentEnabled : Signal::ComponentDisabled;
                if (m_signalManager.IsSignalEnabled(flag)) ASTRA_UNLIKELY
                {
                    if (enable) m_signalManager.Emit<Events::ComponentEnabled>(entity, id, element);
                    else        m_signalManager.Emit<Events::ComponentDisabled>(entity, id, element);
                }
            }
            return true;
        }
```

- [ ] **Step 4: Route `SetEnabledByID` and `IsEnabledByID`**

0. Delete Task 13's three pieces: the `if (IsUniversalId(componentId)) ASTRA_UNLIKELY return SetEnabledUniversal(...);` arm in `SetEnabledByID`, the `if (IsUniversalId(componentId)) ASTRA_UNLIKELY { ... }` block in `IsEnabledByID`, and the private `SetEnabledUniversal` function. (`IsUniversalId` itself stays: Task 12 uses it.)

1. In `SetEnabledByID`, insert this directly after the `if (!rec || !rec->chunk) ASTRA_UNLIKELY return false;` check and before the mask test. Placing it after the check makes stale handles fail first.

```cpp
            if (const StorageKind kind = m_componentStorage->KindOf(componentId); kind != StorageKind::Table) ASTRA_UNLIKELY
                return SetEnabledInStore(entity, *rec, componentId, kind, enable);
```

2. In `IsEnabledByID`, insert this at the same position (after its `rec` check):

```cpp
            if (const StorageKind kind = m_componentStorage->KindOf(componentId); kind != StorageKind::Table) ASTRA_UNLIKELY
            {
                if (kind == StorageKind::Sparse)
                {
                    const SparseSet* set = m_componentStorage->FindSparse(componentId);   // a read never creates a store
                    return set && set->Contains(entity) && !set->IsDisabled(entity);
                }
                const UniversalColumn* col = m_componentStorage->FindUniversal(componentId);
                return col && col->IsLive(entity) && !col->IsDisabled(entity);
            }
```

For a present non-enableable element, `IsDisabled` returns false, so `IsEnabledByID` returns true. That matches the Table body's "present non-enableable column: always enabled".

- [ ] **Step 5: Run the suites and confirm they pass**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseEnable.*:SparseCarry.*:SparseStructure.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=UniversalStorage*
```

Expected: `[  PASSED  ] 26 tests.` for the first run; every `UniversalStorage*` test passes in the second (Task 13's enable answers survive the replacement).

- [ ] **Step 6: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass, including the existing enableable suites (`ViewTest`, `CommandBufferTest` SetEnabled cases). Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Registry/SparseEnableTest.cpp
git commit -m "feat(storage): SetEnabled/IsEnabled for Sparse and Universal with the chunk disabled mirror (spec s7.2, P3)

SetEnabledByID/IsEnabledByID route non-Table ids to the store flag; a genuine
change updates the chunk's disabled bit (Universal: present-less mirror block,
slot acquired on first use) and fires ComponentEnabled/Disabled once.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 20: Change ticks for Sparse components

Spec s3.3, rows `Modified`/`IsChanged`/`IsAdded`/`SetIfNeq`, "per-element ticks in the store".

| Case | Behaviour |
|---|---|
| Tracked Sparse `T` | Exact: the element's `EntityTicks` in the set are written `{now, now}` on attach. `changed` is stamped on a mutable `GetComponent`, `Modified` and a storing `SetIfNeq`. |
| Untracked Sparse `T` | Coarse: one version per component, the analogue of the Table chunk-column version. `IsChanged`/`IsAdded` compare against that version, so every member answers true after any member is written. This is documented and is the same trade `Changed<T>` makes for an untracked Table column. |
| Sparse tag | Like a Table tag (no column): `Modified`, `IsChanged`, `IsAdded` and `SetIfNeq` return false. |

The Universal arms are Task 14's and are untouched: untracked Universal stays page-coarse through `UniversalColumn` (integration ruling: one coarse scheme per kind). The Sparse coarse version lives in `ComponentStorage` keyed by `ComponentID`, so Task 27 (`Changed<S>`/`Added<S>` on an untracked Sparse argument) reads the same value. `SparseSet` itself keeps no coarse tick (Task 10).

**What this task replaces.** Every typed and by-id entry point already routes a Sparse id through Task 14's helpers (Task 11 made `RoutesToStore<T>()` true for every Sparse-trait `T`). This task REPLACES the `default:` arm of Task 14's `MarkStoreWritten` and `StoreTickNewer` with a `case StorageKind::Sparse:`. It adds no separate Sparse arm to `GetComponent`, `Modified`, `IsChanged`, `IsAdded` or `SetIfNeq`.

**Files:**
- Modify: `include/Astra/Registry/ComponentStorage.hpp`, adding the coarse version array and accessors.
- Modify: `include/Astra/Registry/Registry.hpp`:
  - the `SparseAttach` stamp;
  - the helpers;
  - the `case StorageKind::Sparse:` arms of Task 14's `MarkStoreWritten` and `StoreTickNewer` (replacing their `default:` arms).
- Create: `tests/Registry/SparseTicksTest.cpp`

**Interfaces:**
- `ComponentStorage`:
  - `Tick CoarseVersion(ComponentID) const noexcept`
  - `void StampCoarse(ComponentID, Tick) noexcept`
- `Registry`, private:
  - `static EntityTicks* SparseTicksOf(SparseSet&, Entity) noexcept`
  - `void StampSparseWrite(SparseSet&, Entity)`
  - `bool SparseChangedSince(Entity, ComponentID, Tick since, bool added) const`

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/SparseTicksTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <utility>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

TEST(SparseTicks, AddStampsAddedAndChanged)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    const Astra::Tick before = reg.CurrentTick();
    reg.AdvanceTick();
    reg.AddComponent<SpE>(e, SpE{1.0f});
    EXPECT_TRUE(reg.IsAdded<SpE>(e, before));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, before));
    const Astra::Tick at = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_FALSE(reg.IsAdded<SpE>(e, at));
    EXPECT_FALSE(reg.IsChanged<SpE>(e, at));
}

TEST(SparseTicks, MutableGetIsAWriteConstGetIsNot)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    const Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    (void)std::as_const(reg).GetComponent<SpE>(e);
    EXPECT_FALSE(reg.IsChanged<SpE>(e, mark));
    reg.GetComponent<SpE>(e)->v = 2.0f;
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    EXPECT_FALSE(reg.IsAdded<SpE>(e, mark));
}

TEST(SparseTicks, ModifiedTypedAndById)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    reg.AddComponent<SpTag>(e, SpTag{});
    Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_TRUE(reg.Modified<SpE>(e));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_TRUE(reg.Modified(e, Astra::TypeID<SpE>::Value()));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    EXPECT_FALSE(reg.Modified<SpTag>(e));                   // tag: no data, Table parity
    EXPECT_FALSE(reg.Modified<SpA>(e));                     // absent
    EXPECT_FALSE(reg.Modified<SpE>(Astra::Entity::Invalid()));
}

TEST(SparseTicks, SetIfNeqStoresAndStampsOnlyOnDifference)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{1.0f});
    const Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_FALSE(reg.SetIfNeq<SpE>(e, SpE{1.0f}));
    EXPECT_FALSE(reg.IsChanged<SpE>(e, mark));
    EXPECT_TRUE(reg.SetIfNeq<SpE>(e, SpE{3.0f}));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<SpE>(e)->v, 3.0f);
}

TEST(SparseTicks, UntrackedSparseIsComponentCoarse)
{
    Astra::Registry reg;
    const Astra::Entity a = reg.CreateEntity();
    const Astra::Entity b = reg.CreateEntity();
    reg.AddComponent<SpA>(a, SpA{});
    reg.AddComponent<SpA>(b, SpA{});
    const Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_FALSE(reg.IsChanged<SpA>(a, mark));
    EXPECT_TRUE(reg.Modified<SpA>(b));
    EXPECT_TRUE(reg.IsChanged<SpA>(b, mark));
    EXPECT_TRUE(reg.IsChanged<SpA>(a, mark));     // coarse: any member's write (documented)
    EXPECT_TRUE(reg.IsAdded<SpA>(a, mark));       // untracked: IsAdded == IsChanged (README caveat)
}

TEST(SparseTicks, AbsentOrInvalidAnswersFalse)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.GetComponentRegistry()->RegisterComponent<SpE>();
    EXPECT_FALSE(reg.IsChanged<SpE>(e, Astra::Tick{0}));
    EXPECT_FALSE(reg.IsAdded<SpE>(Astra::Entity::Invalid(), Astra::Tick{0}));
}
```

- [ ] **Step 2: Regenerate the projects and run the tests to verify they fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseTicks.*
```

Expected: the build succeeds. Five tests FAIL: `IsChanged`/`IsAdded`/`Modified` already route a Sparse id to Task 14's `StoreTickNewer`/`MarkStoreWritten`, whose `default:` arm answers false. For example, `EXPECT_TRUE(reg.IsAdded<SpE>(e, before))` fails. `AbsentOrInvalidAnswersFalse` passes, which pins the refusal.

- [ ] **Step 3: Add the coarse version to `ComponentStorage`**

In `include/Astra/Registry/ComponentStorage.hpp`:
1. Include `"../Core/Tick.hpp"` if it is not already included.
2. Add the public methods:

```cpp
        // Coarse change version per Sparse/Universal component (spec s3.3): the
        // analogue of a Table chunk-column version, stamped on every add, mutable
        // Get, Modified and storing SetIfNeq of the component; read by IsChanged /
        // IsAdded / Changed<T> for an UNTRACKED component. 0 == never. Survives
        // Clear() (a version is a time, not a value).
        ASTRA_NODISCARD Tick CoarseVersion(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS ? m_coarseVersion[id] : Tick{0};
        }
        void StampCoarse(ComponentID id, Tick tick) noexcept
        {
            if (id < MAX_COMPONENTS) ASTRA_LIKELY
                m_coarseVersion[id] = tick;
        }
```

3. Add the private member `Tick m_coarseVersion[MAX_COMPONENTS]{};`.

- [ ] **Step 4: Add the Registry helpers and stamp at attach**

In `include/Astra/Registry/Registry.hpp`:

1. Add these to the private Sparse funnel section:

```cpp
        // The per-element ticks of a tracked, non-empty Sparse element, else nullptr.
        // Dense index from the element address (stride == descriptor size; a
        // SparseSet has no DenseIndexOf accessor, Task 10).
        ASTRA_NODISCARD static EntityTicks* SparseTicksOf(SparseSet& set, Entity entity) noexcept
        {
            const ComponentDescriptor& d = set.Descriptor();
            if (!d.isChangeTracked || d.size == 0)
                return nullptr;
            void* element = set.Get(entity);
            if (!element)
                return nullptr;
            const size_t dense = static_cast<size_t>(static_cast<std::byte*>(element) -
                                                     static_cast<std::byte*>(set.DataAt(0))) / d.size;
            return set.TicksAt(dense);
        }

        // A write to a present Sparse element: stamp the coarse version and, for a
        // tracked type, the element's `changed` tick.
        void StampSparseWrite(SparseSet& set, Entity entity)
        {
            const Tick now = m_archetypeManager->CurrentTick();
            m_componentStorage->StampCoarse(set.Descriptor().id, now);
            if (EntityTicks* t = SparseTicksOf(set, entity))
                t->changed = now;
        }

        // IsChanged/IsAdded for a Sparse id. False for stale/absent/tag (Table
        // parity: a tag has no column); tracked = exact, untracked = coarse.
        ASTRA_NODISCARD bool SparseChangedSince(Entity entity, ComponentID id, Tick since, bool added) const
        {
            SparseSet* set = m_componentStorage->FindSparse(id);   // a read never creates a store
            if (!set || set->Descriptor().size == 0 || !set->Get(entity))
                return false;
            if (const EntityTicks* t = SparseTicksOf(*set, entity))
                return IsNewer(added ? t->added : t->changed, since);
            return IsNewer(m_componentStorage->CoarseVersion(id), since);
        }
```

2. In `SparseAttach`, insert the following directly after `rec.chunk->SetSparsePresent(...)`. The element is born at Now(), as a Table slot is in `InitTicks`.

```cpp
            const Tick now = m_archetypeManager->CurrentTick();
            m_componentStorage->StampCoarse(id, now);
            if (EntityTicks* t = SparseTicksOf(*set, entity))
                *t = EntityTicks{now, now};
```

- [ ] **Step 5: Replace the `default:` arms of Task 14's routed helpers**

Task 11's `RoutesToStore<T>()` is `true` for every Sparse-trait `T`, so these calls already reach Task 14's helpers for a Sparse id:
- the mutable `GetComponent<T>` -> `MarkStoreWritten` (a non-const access is a write, spec s3.3);
- `Modified<T>`, `Modified(Entity, ComponentID)` and a storing `SetIfNeq<T>` -> `MarkWrittenRouted` -> `MarkStoreWritten`;
- `IsChanged<T>` / `IsAdded<T>` -> `StoreTickNewer`.

They still answer `false` for Sparse through the `default:` arms. Replace those two arms; edit nothing else in those functions.

1. In `MarkStoreWritten`, replace its

```cpp
                default:
                    return false;   // Sparse: Task 20 replaces this arm with `case StorageKind::Sparse`
```

with

```cpp
                case StorageKind::Sparse:
                {
                    // A write to a present Sparse element (spec s3.3): the coarse version,
                    // plus the element's `changed` tick when tracked. Tag/absent/stale: false.
                    SparseSet* set = m_componentStorage->FindSparse(id);
                    if (!set || set->Descriptor().size == 0 || !set->Get(entity)) ASTRA_UNLIKELY
                        return false;
                    StampSparseWrite(*set, entity);
                    return true;
                }
                default:
                    return false;
```

2. In `StoreTickNewer`, replace its

```cpp
                default:
                    return false;   // Sparse: Task 20 replaces this arm with `case StorageKind::Sparse`
```

with

```cpp
                case StorageKind::Sparse:
                    return SparseChangedSince(entity, id, since, added);   // tracked: exact; untracked: CoarseVersion
                default:
                    return false;
```

A Sparse tag answers `false` everywhere (Table parity: a tag has no column): `StampSparseWrite` is never reached for `size == 0`, and `SparseChangedSince` returns `false` for it. `SetIfNeq<T>` for a Sparse `T` compares through Task 14's non-stamping `StoreGet<T>` and stamps through `MarkWrittenRouted`, so it needs no edit.

- [ ] **Step 6: Run the suites and confirm they pass**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseTicks.*:SparseStructure.*:SparseEnable.*
```

Expected: `[  PASSED  ] 24 tests.`

- [ ] **Step 7: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass, including `ChangeDetection*` (the Table paths are untouched). Record the seed line.

- [ ] **Step 8: Commit**

```powershell
git add include/Astra/Registry/ComponentStorage.hpp include/Astra/Registry/Registry.hpp tests/Registry/SparseTicksTest.cpp
git commit -m "feat(storage): change ticks for Sparse components -- exact per-element for tracked, component-coarse for untracked (spec s3.3)

Attach stamps {now, now}; mutable GetComponent, Modified (typed + by-id) and a
storing SetIfNeq stamp 'changed'; IsChanged/IsAdded read the store. The coarse
version lives in ComponentStorage keyed by id for the query layer.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 21: Destroy walk, `Clear`, and `Defragment` for the stores

Spec s7.3. The steps for `DestroyEntity`/`DestroyEntities` are:
1. `EntityDestroyed` fires.
2. `ComponentStorage::OnEntityDestroyed(e, chunk, index)` walks the entity's chunk `SparseBlock`s. For each set `present` bit it erases the entity from that slot's set, then it destructs the entity's slot in every universal column.
3. The archetype swap-remove runs. Its `SwapRemoveSparseBits` (Task 17) fixes the chunk bits.

Per-component `ComponentRemoved` is still not emitted on destroy (today's behaviour). `Clear()` calls `ComponentStorage::Clear()`, which destroys every element and keeps the slot assignments. `Defragment` needs no new code: `CompactChunks` carries since Task 17. Its test lives here because it needs destroys to fragment.

This task makes `OnEntityDestroyed` the single destroy hook for both stores, and states exactly what it replaces:
- **Task 9 (h)/(i)** call `OnEntityDestroyed` from `DestroyEntity`/`DestroyEntities` behind `HasAnyStore()`. Step 4 REPLACES those two blocks (same position, same guard) to add the sparse structural-counter bump.
- **Task 11** added an interim O(sets) erase loop over `m_sparseIds` inside `OnEntityDestroyed`. Step 3 REPLACES the whole body with the presence-block walk.
- **Task 9 (j)** already calls `m_componentStorage->Clear()` first in `Registry::Clear()`; Step 4 keeps that single call and adds the counter bump.

**Files:**
- Modify: `include/Astra/Registry/ComponentStorage.hpp`, giving `OnEntityDestroyed` its final body.
- Modify: `include/Astra/Registry/Registry.hpp` (`DestroyEntity`, `DestroyEntities`, `Clear`)
- Create: `tests/Registry/SparseLifecycleTest.cpp`

**Interfaces:**
- No new API. The canonical `void ComponentStorage::OnEntityDestroyed(Entity, ArchetypeChunk&, size_t)` and `void ComponentStorage::Clear()` gain their final behaviour.

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/SparseLifecycleTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

namespace
{
    template<typename T>
    size_t SetSize(Astra::Registry& reg)
    {
        Astra::SparseSet* set = reg.GetComponentStorage()->Sparse(Astra::TypeID<T>::Value());
        return set ? set->Size() : 0u;
    }
}

TEST(SparseLifecycle, DestroyErasesEveryMembership)
{
    Astra::Registry reg;
    const Astra::Entity keep = reg.CreateEntityWith(Position{});
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(keep, SpA{1.0f});
    reg.AddComponent<SpA>(e, SpA{2.0f});
    reg.AddComponent<SpB>(e, SpB{3});
    const uint32_t counter = reg.GetArchetypeManager()->GetSparseStructuralCounter();

    reg.DestroyEntity(e);

    EXPECT_EQ(SetSize<SpA>(reg), 1u);
    EXPECT_EQ(SetSize<SpB>(reg), 0u);
    EXPECT_NE(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(keep)->v, 1.0f);
    ExpectInvariants(reg);

    const Astra::Entity reused = reg.CreateEntityWith(Position{});   // may recycle e's id at version + 1
    EXPECT_FALSE(reg.HasComponent<SpA>(reused));
    EXPECT_FALSE(reg.HasComponent<SpB>(reused));
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DestroySwapRemoveKeepsTheTailsMembership)
{
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntityWith(Position{});
    const Astra::Entity e1 = reg.CreateEntityWith(Position{});
    const Astra::Entity e2 = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e0, SpA{});
    reg.AddComponent<SpB>(e2, SpB{9});
    reg.DestroyEntity(e0);                      // e2 swaps into slot 0
    ASSERT_EQ(Locate(reg, e2).second, 0u);
    EXPECT_TRUE(PresentBit<SpB>(reg, e2));
    EXPECT_FALSE(PresentBit<SpA>(reg, e2));
    EXPECT_FALSE(reg.HasComponent<SpA>(e1));
    EXPECT_EQ(reg.GetComponent<SpB>(e2)->v, 9);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, EntityDestroyedHandlersStillSeeSparseComponents)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::EntityDestroyed);
    bool had = false;
    float value = 0.0f;
    auto h = reg.GetSignalManager()->On<Astra::Events::EntityDestroyed>().Register(
        [&](const Astra::Events::EntityDestroyed& ev)
        {
            had = reg.HasComponent<SpA>(ev.entity);
            if (had) value = std::as_const(reg).GetComponent<SpA>(ev.entity)->v;
        });
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{6.0f});
    reg.DestroyEntity(e);
    EXPECT_TRUE(had);
    EXPECT_FLOAT_EQ(value, 6.0f);
    reg.GetSignalManager()->On<Astra::Events::EntityDestroyed>().Unregister(h);
}

TEST(SparseLifecycle, DestroyEntitiesBatchWithDuplicates)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(200);
    reg.CreateEntities<Position>(200, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 2 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 3 == 0) reg.AddComponent<SpE>(es[i], SpE{});
        if (i % 6 == 0) reg.SetEnabled<SpE>(es[i], false);
    }
    std::vector<Astra::Entity> doomed;
    for (size_t i = 1; i < es.size(); i += 4) doomed.push_back(es[i]);
    for (size_t i = 0; i < es.size(); i += 8) doomed.push_back(es[i]);
    doomed.push_back(es[0]);                                   // a duplicate
    reg.DestroyEntities(std::span<Astra::Entity>(doomed.data(), doomed.size()));

    size_t expectA = 0;
    for (size_t i = 0; i < es.size(); ++i)
    {
        const bool dead = (i % 4 == 1) || (i % 8 == 0);
        if (dead) { EXPECT_FALSE(reg.IsValid(es[i])); continue; }
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 2 == 0) << i;
        expectA += (i % 2 == 0) ? 1u : 0u;
        if (i % 3 == 0) EXPECT_EQ(reg.IsEnabled<SpE>(es[i]), i % 6 != 0) << i;
    }
    EXPECT_EQ(SetSize<SpA>(reg), expectA);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, CommandBufferDestroyRoutes)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{});
    Astra::CommandBuffer cmd(&reg);
    cmd.DestroyEntity(e);
    cmd.Execute();
    EXPECT_EQ(SetSize<SpA>(reg), 0u);
    ExpectInvariants(reg);
}
```

Continue `tests/Registry/SparseLifecycleTest.cpp`:

```cpp
TEST(SparseLifecycle, ClearDestroysElementsAndKeepsSlots)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(10);
    reg.CreateEntities<Position>(10, es);
    for (Astra::Entity e : es) reg.AddComponent<SpStr>(e, SpStr{std::string("v")});
    const int slot = SlotOf<SpStr>(reg);
    ASSERT_GE(slot, 0);

    reg.Clear();

    EXPECT_EQ(SetSize<SpStr>(reg), 0u);
    EXPECT_EQ(SlotOf<SpStr>(reg), slot);                     // slot assignment survives Clear
    for (Astra::Entity e : es) EXPECT_FALSE(reg.HasComponent<SpStr>(e));   // stale handles miss
    const Astra::Entity fresh = reg.CreateEntityWith(Position{});
    EXPECT_FALSE(reg.HasComponent<SpStr>(fresh));            // recycled id, next version
    reg.AddComponent<SpStr>(fresh, SpStr{std::string("w")});
    EXPECT_EQ(reg.GetComponent<SpStr>(fresh)->s, "w");
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, UniversalDestroyAndRecycleIsBornEnabled)
{
    Astra::Registry::Config cfg;
    cfg.AddUniversal<UnE>();
    Astra::Registry reg(cfg);
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    reg.SetEnabled<UnE>(a, false);
    reg.SetEnabled<UnE>(b, false);
    reg.DestroyEntity(a);                       // b swaps into a's slot, disabled mirror travels
    EXPECT_FALSE(reg.IsEnabled<UnE>(b));
    ExpectInvariants(reg);
    const Astra::Entity c = reg.CreateEntityWith(Position{});   // may reuse a's id
    EXPECT_TRUE(reg.IsEnabled<UnE>(c));
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DefragmentAfterDestroysKeepsPresenceAndDisabledState)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(3000);
    reg.CreateEntities<Position>(3000, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 3 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 2 == 0) reg.AddComponent<SpE>(es[i], SpE{});
        if (i % 4 == 0) reg.SetEnabled<SpE>(es[i], false);
    }
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < es.size(); ++i)
        if (i % 5 != 0) doomed.push_back(es[i]);
    reg.DestroyEntities(std::span<Astra::Entity>(doomed.data(), doomed.size()));

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;
    reg.Defragment(opts);

    size_t expectA = 0;
    for (size_t i = 0; i < es.size(); i += 5)
    {
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 3 == 0) << i;
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), i % 3 == 0) << i;
        if (i % 3 == 0) EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(es[i])->v, float(i)) << i;
        if (i % 2 == 0) EXPECT_EQ(reg.IsEnabled<SpE>(es[i]), i % 4 != 0) << i;
        expectA += (i % 3 == 0) ? 1u : 0u;
    }
    EXPECT_EQ(SetSize<SpA>(reg), expectA);
    ExpectInvariants(reg);
}
```

- [ ] **Step 2: Regenerate the projects and run the tests to verify they fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseLifecycle.*
```

Expected: the build succeeds. `DestroyErasesEveryMembership` FAILS on `EXPECT_NE(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter)`: Task 9's destroy call sites do not bump the sparse structural counter yet. The other destroy tests may already pass, because Task 11's interim loop inside `OnEntityDestroyed` erases the destroyed entity from every set (O(sets)); they pin the behaviour across Step 3's replacement by the O(blocks) walk. If any of them fails, the usual symptoms are `EXPECT_EQ(SetSize<SpB>(reg), 0u)` failing with `1 vs 0`, or `ExpectInvariants` reporting `P1 sparse member is not a live, located entity`.

`EntityDestroyedHandlersStillSeeSparseComponents` passes, which pins the ordering. `ClearDestroysElementsAndKeepsSlots` and `UniversalDestroyAndRecycleIsBornEnabled` already pass, because Task 9 wired `Clear` and the universal destroy. That is expected; they pin the behaviour across this task's replacement.

- [ ] **Step 3: Give `ComponentStorage::OnEntityDestroyed` its final body**

In `include/Astra/Registry/ComponentStorage.hpp`:
1. Include `"../Archetype/ArchetypeChunkPool.hpp"` if it is not already included. The body needs the complete `ArchetypeChunk`.
2. Replace the body of `OnEntityDestroyed` with the code below.

`m_universal` is the deduplicated universal spec list Task 4's constructor retained. The walk uses the non-creating lookups (`FindSparse`/`FindUniversal`, Tasks 9 and 11): a destroy never creates a store, and a slot or column released by a module unload (Task 35) is skipped.

```cpp
        // Destroy walk (spec s7.3). Called after EntityDestroyed and BEFORE the
        // archetype swap-remove: the chunk's presence blocks name exactly the sparse
        // sets holding `e` (P1), so destroy costs O(blocks in the chunk), not
        // O(sparse components). The swap-remove that follows fixes the chunk bits
        // (SwapRemoveSparseBits). Universal slots are destructed for every universal
        // column. No ComponentRemoved: destroy never emitted it (Registry.hpp:230-258).
        void OnEntityDestroyed(Entity e, ArchetypeChunk& chunk, size_t index)
        {
            for (const ArchetypeChunk::SparseBlock& b : chunk.GetSparseBlocks())
            {
                if (!b.present || !chunk.IsSparsePresent(b.slot, index))
                    continue;                                  // mirror-only block, or not a member
                if (SparseSet* set = FindSparse(ComponentOfSlot(b.slot)))
                    set->Erase(e);
            }
            for (const UniversalSpec& u : m_universal)
            {
                if (UniversalColumn* col = FindUniversal(u.id))
                    col->Destroy(e);
            }
        }
```

- [ ] **Step 4: Call it from the destroy paths and from `Clear`**

In `include/Astra/Registry/Registry.hpp`:

1. `DestroyEntity`. REPLACE the block Task 9 (h) inserted (between the `EntityDestroyed` emission block and `m_archetypeManager->RemoveEntity(entity, rec);`) with:

```cpp
            // Storage policies (spec s7.3): sparse erase + universal destruct, after
            // EntityDestroyed (handlers still see every component) and before the
            // swap-remove (whose SwapRemoveSparseBits fixes the chunk bits). `rec`
            // points into the record table, so a handler's relocation is visible.
            // Table-only registries pay one predictable branch (G4).
            if (m_componentStorage->HasAnyStore()) ASTRA_UNLIKELY
            {
                ASTRA_ASSERT(rec->chunk, "DestroyEntity: located record without a chunk");
                if (rec->chunk)
                {
                    if (rec->chunk->HasSparseBlocks()) ASTRA_UNLIKELY
                        m_archetypeManager->BumpSparseStructuralCounter();   // a sparse holder's membership changes
                    m_componentStorage->OnEntityDestroyed(entity, *rec->chunk, rec->location.GetEntityIndex());
                }
            }
```

2. `DestroyEntities`. REPLACE the block Task 9 (i) inserted (between the `EntityDestroyed` emission loop and `m_archetypeManager->RemoveEntities(validEntities);`) with the block below. Locations are stable across this loop because nothing relocates until `RemoveEntities`. The counter is bumped only when a doomed entity actually held a sparse member, so a Table-only batch destroy inside a `ForEach` keeps today's guard outcome (integration ruling on Task 38: parity means "same outcome as Table today").

```cpp
            // Storage policies (spec s7.3): walk every doomed entity BEFORE any swap-remove.
            if (m_componentStorage->HasAnyStore()) ASTRA_UNLIKELY
            {
                bool sparseMembershipChanged = false;
                for (Entity entity : validEntities)
                {
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    if (rec && rec->chunk)
                    {
                        sparseMembershipChanged |= rec->chunk->HasSparseBlocks();
                        m_componentStorage->OnEntityDestroyed(entity, *rec->chunk, rec->location.GetEntityIndex());
                    }
                }
                if (sparseMembershipChanged)
                    m_archetypeManager->BumpSparseStructuralCounter();
            }
```

3. `Clear()`. Its final form is below (Task 9 (j) already has the single `m_componentStorage->Clear()` call first; keep exactly one).

```cpp
        void Clear()
        {
            const bool hadStores = m_componentStorage->HasAnyStore();
            m_componentStorage->Clear();   // every sparse/universal element destroyed; slots kept (spec s7.3)
            m_archetypeManager->Clear();   // in place -- keeps cached Views/Relations valid; chunks free their presence blocks
            if (hadStores) ASTRA_UNLIKELY
                m_archetypeManager->BumpSparseStructuralCounter();
            m_relationshipGraph->Clear();
            m_entityManager.Clear();
        }
```

4. Confirm that no second destroy path remains:

```powershell
git grep -n -e "->Destroy(" -e "->Erase(" -- include/Astra/Registry/Registry.hpp
```

The command should print nothing from `DestroyEntity`, `DestroyEntities` or `Clear`. It may print `SparseDetach`'s `set->Erase(entity)`, which is the remove path, not a destroy path.

- [ ] **Step 5: Run the chunk's suites and confirm they pass**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SparseBlockTest.*:SparseStructure.*:SparseCarry.*:SparseBatch.*:SparseEnable.*:SparseTicks.*:SparseLifecycle.*
```

Expected: `[  PASSED  ] 61 tests.` The breakdown is `SparseBlockTest` 12, `SparseStructure` 10, `SparseCarry` 7, `SparseBatch` 9, `SparseEnable` 9, `SparseTicks` 6, `SparseLifecycle` 8.

- [ ] **Step 6: Run the full suite, shuffled**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all tests pass. The existing destroy, Clear and Defragment suites (`RegistryTest`, `ClearOrphansViewTest`, `MemoryCleanupTest`, `ConfigPreservationTest`) stay green unedited (S1). Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/ComponentStorage.hpp include/Astra/Registry/Registry.hpp tests/Registry/SparseLifecycleTest.cpp
git commit -m "feat(storage): destroy walk, Clear and Defragment for the sparse/universal stores (spec s7.3)

OnEntityDestroyed erases the entity from exactly the sets its chunk blocks name
and destructs its universal slots, after EntityDestroyed and before the
swap-remove; it is now the single destroy hook. Clear destroys every element and
keeps slot assignments; Defragment keeps P1-P3 (carry from Task 17).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


## Chunk D: queries (Tasks 22-30)

Spec sections: s6.1-s6.5 (classification, driver selection, the generalised run extractor, fetching, every iteration surface), s3.3 (reflection row), s4.3 (iteration safety).

**What this chunk takes from Tasks 1-21**, by the canonical names:
- `StorageOfV<T>`, `StorageKind`, `ASTRA_UNIVERSAL_PAGE_SLOTS` (Task 3).
- `Registry::Config::AddUniversal<T>()` (Task 5).
- The Registry's `ComponentStorage` member `m_componentStorage`, a `std::shared_ptr` (Task 5); `.get()` yields the raw pointer.
- `ComponentStorage::KindOf / SparseSlotOf / Sparse / Universal`. `KindOf` reads the descriptor, so this chunk calls it only for a REGISTERED id.
- `SparseSet::Contains / Get / Size / Entities / DataAt / TicksAt / IsDisabled / Descriptor`, and the Chunk B additions on both stores: `MarkWritten(Entity, Tick)`, `AddedTick(Entity)`, `ChangedTick(Entity)` (tracked: exact. Untracked Universal: the column's per-page version. Untracked Sparse: the SparseSet has NO coarse tier and answers 0; read `ComponentStorage::CoarseVersion(id)` (Task 20) instead -- one coarse scheme per kind).
- `UniversalColumn::Get / IsLive / IsDisabled / Ticks / MarkWritten / AddedTick / ChangedTick`.
- `ArchetypeChunk::FindSparseBlock(slot)`, which returns a block with `present`, `disabled`, `presentCount` and `disabledCount`. An enableable Universal component's blocks carry `present == nullptr`.
- `ArchetypeManager::GetSparseStructuralCounter()` (Task 16). It is bumped by every Sparse membership change.
- `Registry::ValidateStorageInvariants()` (Task 16) and the Sparse/Universal arms of Add/Remove/SetEnabled/Get/Has/Modified/IsChanged (Tasks 9-20).
- `Astra::Test::PosS` (Task 3), the Sparse twin of `Position`.

**What this chunk adds beyond the canonical interfaces** (also listed in the chunk notes):
- `ArchetypeManager::SetComponentStorage(ComponentStorage*)` and `GetComponentStorage()` (non-owning). These exist because `View` and `Relations` hold only the manager.
- `Detail::ForEachMatchingRun` (canonical), plus `Detail::CountMatching` and `Detail::WriteMatchWords`.
- `QueryBuilder::GetTableRequiredMask()` and `GetTableExcludedMask()`, and the `Detail` storage traits in `Query.hpp`.
- The header `Registry/StorageAccess.hpp`: `Detail::SparseTicksOf` (Task 25), then `Detail::EffectiveKindOf<T>`, `Detail::HasInStorage<T>` and `Detail::GetInStorage<T>` (Task 30).
- `ViewIterHooks` in `ViewIterator.hpp`.
- `View::HasSparseArg` (public `static constexpr`).

**Design in one paragraph.** `View` resolves a per-registry storage plan once at construction: one `StorageKind` per component the query names, and runtime archetype masks that contain only Table-kind members. A view naming no Sparse type, in a registry that declares none of its types Universal, keeps every pre-existing loop byte for byte (G4). The only additions on its path are one predictable `m_anyUniversal` test per archetype walk and the `col >= 0` stamp guard in `ViewIterator`. Every other view runs the storage path:
- `VisitChunkStorage` builds per-chunk word sets (required-Sparse presence, `Not<Sparse>`, the disabled mirrors, Any/OneOf words, the change-excluded set) and hands them to `ForEachMatchingRun`.
- It fetches per argument through a `Fetch<T>`: Table `base[i]`, Sparse `set->Get(e)`, Universal through a page cursor.
- A view with a required Sparse argument and no required Table argument is sparse-driven: it walks the smallest required set's dense array.

All commands run from `D:\dev\starworks\Astra-storage` in **PowerShell**:
- build: `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
- regenerate after ADDING a test file: `D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`
- full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Record the `Randomizing tests' orders with a seed of N` line in the task report.

---

### Task 22: Query classification and the per-view storage plan (s6.1)

Only Table-kind arguments reach archetype masks, `CollectArchetypes`' pre-filter count and archetype caching. Universal is resolved per registry when the view is built. Scheduler access (`ViewAccess`) stays id-level and is not touched.

**Files:**
- Create: `tests/Registry/StorageQueryComponents.hpp` (shared by Tasks 22-30)
- Create: `tests/Registry/StorageViewPlanTest.cpp`
- Modify: `include/Astra/Registry/Query.hpp` (the `Detail` storage traits; two `QueryBuilder` masks)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (the non-owning storage pointer)
- Modify: `include/Astra/Registry/Registry.hpp` (wire the pointer in the three constructors and in `LoadInternal`)
- Modify: `include/Astra/Registry/View.hpp` (the plan, `MatchesArchetype`, `CollectArchetypes` / `EnsureArchetypes` / `VisibleRecord`)

**Interfaces:**
- Consumes: `StorageOfV`, `StorageKind`, `ComponentStorage::KindOf`, `Registry::Config::AddUniversal<T>()`, `m_componentStorage`.
- Produces:
  ```cpp
  namespace Astra::Detail {
    template<typename T> inline constexpr bool IsSparseArgV;        // Sparse TRAIT, cv-stripped
    template<typename Tuple> using FilterTableTrait_t = ...;          // keeps non-Sparse-trait members
    template<typename Tuple> using FilterSparseTrait_t = ...;         // keeps Sparse-trait members
    template<typename Tuple> struct AnySparseTrait;                   // bool_constant
    template<typename... QueryArgs> using QueryPlanTypes = ...;       // every named component, cv-stripped, unique
    template<typename T, typename Tuple> struct TupleIndexOf;         // ::value
  }
  QueryBuilder<...>::GetTableRequiredMask();  QueryBuilder<...>::GetTableExcludedMask();
  void ArchetypeManager::SetComponentStorage(ComponentStorage*) noexcept;
  ComponentStorage* ArchetypeManager::GetComponentStorage() const noexcept;
  static constexpr bool View<...>::HasSparseArg;
  ```

- [ ] **Step 1: Write the shared test components**

Create `tests/Registry/StorageQueryComponents.hpp`:

```cpp
#pragma once

// Storage-policies query suite (Tasks 22-30): test components + helpers. ONE new
// TypeID only (SpTag; the binary shares one ASTRA_MAX_COMPONENTS id space -- see the
// budget note in tests/TestComponents.hpp). SpB and SpE are chunk C's types (Task 16's
// SparseStructureTestComponents.hpp), brought in by using-declarations, so they mint no
// id. The Sparse data type is Astra::Test::PosS (Task 3);
// Universal variants reuse Table types (Health, Timer, TrackedPos, TrackedVel)
// declared universal per registry with Config::AddUniversal<T>().

#include <Astra/Astra.hpp>
#include <memory>
#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"   // Task 16: SpB, SpE (shared, no new TypeIDs)

namespace Astra::Test::StorageQ
{
    // Second Sparse data type (Any/OneOf groups, the smallest-set driver): chunk C's.
    using Astra::Test::SparseC::SpB;

    // Sparse tag: presence only.
    struct SpTag
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
    };

    // Sparse + enableable + change-tracked: disabled mirror, Mut<T>, Changed/Added: chunk C's.
    using Astra::Test::SparseC::SpE;

    static_assert(Component<SpB> && Component<SpTag> && Component<SpE>);

    // A registry declaring every Us... universal, optionally with a work scheduler.
    template<typename... Us>
    std::unique_ptr<Astra::Registry> MakeRegistry(std::shared_ptr<Astra::IWorkScheduler> scheduler = nullptr)
    {
        Astra::Registry::Config cfg;
        (cfg.AddUniversal<Us>(), ...);
        cfg.workScheduler = std::move(scheduler);
        return std::make_unique<Astra::Registry>(cfg);
    }
}
```

- [ ] **Step 2: Write the failing test**

Create `tests/Registry/StorageViewPlanTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

// s6.1: only Table-TRAIT members reach the archetype masks; Sparse members never do.
TEST(StorageViewPlan, TableTraitMasksLeaveSparseMembersOut)
{
    using QB = Astra::QueryBuilder<Position, PosS, Astra::With<SpTag>, Astra::Not<SpB>, Astra::Not<Velocity>>;
    EXPECT_EQ(QB::GetTableRequiredMask(), Astra::MakeComponentMask<Position>());
    EXPECT_EQ(QB::GetTableExcludedMask(), Astra::MakeComponentMask<Velocity>());

    static_assert(Astra::View<Position, PosS>::HasSparseArg);
    static_assert(Astra::View<Position, Astra::Optional<PosS>>::HasSparseArg);
    static_assert(Astra::View<Position, Astra::Any<Velocity, SpB>>::HasSparseArg);
    static_assert(!Astra::View<Position, Velocity, Astra::Not<Health>>::HasSparseArg);
    static_assert(Astra::Detail::IsSparseArgV<const PosS> && !Astra::Detail::IsSparseArgV<Position>);
}

// The scheduler's access sets stay id-level for every kind (ViewAccess is untouched).
TEST(StorageViewPlan, ViewAccessStaysIdLevel)
{
    using VA = Astra::ViewAccess<Astra::View<PosS, const Position, Astra::Optional<const SpB>>>;
    EXPECT_TRUE(VA::WriteMask().Test(Astra::TypeID<PosS>::Value()));
    EXPECT_TRUE(VA::ReadMask().Test(Astra::TypeID<Position>::Value()));
    EXPECT_TRUE(VA::ReadMask().Test(Astra::TypeID<SpB>::Value()));
}

// A Universal component is on every live entity and in no archetype mask: a view
// requiring it matches every archetype, and Not<U> matches nothing.
TEST(StorageViewPlan, UniversalArgumentsLeaveTheMasksAndMatchEveryArchetype)
{
    auto reg = MakeRegistry<Health>();
    for (int i = 0; i < 3; ++i) (void)reg->CreateEntity<Position>();
    for (int i = 0; i < 2; ++i) (void)reg->CreateEntity<Velocity>();
    (void)reg->CreateEntity();

    EXPECT_EQ(reg->CreateView<Health>().Size(), 6u);
    EXPECT_EQ((reg->CreateView<Position, Health>().Size()), 3u);
    EXPECT_EQ((reg->CreateView<Position, Astra::With<Health>>().Size()), 3u);
    EXPECT_EQ((reg->CreateView<Position, Astra::Not<Health>>().Size()), 0u);
    EXPECT_EQ(reg->CreateView<Astra::Not<Health>>().Size(), 0u);
}

// Archetypes created AFTER the view was built go through the same runtime matcher.
TEST(StorageViewPlan, UniversalViewCacheRefreshUsesThePlan)
{
    auto reg = MakeRegistry<Health>();
    (void)reg->CreateEntity<Position>();
    auto view = reg->CreateView<Health>();
    EXPECT_EQ(view.Size(), 1u);
    (void)reg->CreateEntity<Position, Velocity>();   // new archetype
    (void)reg->CreateEntity<Transform>();            // new archetype
    EXPECT_EQ(view.Size(), 3u);
}

// G4: a registry without universals keeps today's matching for a pure-Table view.
TEST(StorageViewPlan, PureTableViewMatchingIsUnchanged)
{
    Astra::Registry reg;
    (void)reg.CreateEntity<Position>();
    (void)reg.CreateEntity<Position, Velocity>();
    (void)reg.CreateEntity<Position, Health>();
    EXPECT_EQ(reg.CreateView<Position>().Size(), 3u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<Velocity>>().Size()), 2u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Any<Velocity, Health>>().Size()), 2u);
    EXPECT_EQ((reg.CreateView<Astra::OneOf<Velocity, Health>>().Size()), 2u);
}

// The manager carries the registry's ComponentStorage for View/Relations.
TEST(StorageViewPlan, ArchetypeManagerCarriesTheComponentStorage)
{
    Astra::Registry reg;
    EXPECT_NE(reg.GetArchetypeManager()->GetComponentStorage(), nullptr);
    reg.Clear();
    EXPECT_NE(reg.GetArchetypeManager()->GetComponentStorage(), nullptr);
}
```

- [ ] **Step 3: Regenerate, build, and verify the failure**

Run: `D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`, then the build command.
Expected: the build FAILS with these errors:
- `'GetTableRequiredMask': is not a member of 'Astra::QueryBuilder<...>'`;
- `'HasSparseArg': is not a member`;
- `'IsSparseArgV': is not a member of 'Astra::Detail'`;
- `'GetComponentStorage': is not a member of 'Astra::ArchetypeManager'`.

- [ ] **Step 4: Add the `Detail` storage traits and the Table-trait masks**

In `include/Astra/Registry/Query.hpp`, insert this block inside `namespace Detail`, after the `AccessWrites` struct and before the namespace's closing brace (the `}` before `// Query modifier types`):

```cpp
        // ============ Storage policies (spec 2026-10-01 s6.1) ============
        // Compile-time half of the per-argument storage classification. Table vs Sparse
        // is the component TYPE's trait; Universal is a per-registry runtime fact that
        // View resolves against its registry. Only Table-trait members can ever be in an
        // archetype mask.
        template<typename T>
        inline constexpr bool IsSparseArgV = (StorageOfV<std::remove_cvref_t<T>> == Storage::Sparse);

        template<typename Tuple> struct FilterTableTrait;
        template<typename... Ts>
        struct FilterTableTrait<std::tuple<Ts...>>
        {
            using type = decltype(std::tuple_cat(
                std::conditional_t<!IsSparseArgV<Ts>, std::tuple<Ts>, std::tuple<>>{}...));
        };
        template<typename Tuple> using FilterTableTrait_t = typename FilterTableTrait<Tuple>::type;

        template<typename Tuple> struct FilterSparseTrait;
        template<typename... Ts>
        struct FilterSparseTrait<std::tuple<Ts...>>
        {
            using type = decltype(std::tuple_cat(
                std::conditional_t<IsSparseArgV<Ts>, std::tuple<Ts>, std::tuple<>>{}...));
        };
        template<typename Tuple> using FilterSparseTrait_t = typename FilterSparseTrait<Tuple>::type;

        template<typename Tuple> struct AnySparseTrait;
        template<typename... Ts>
        struct AnySparseTrait<std::tuple<Ts...>> : std::bool_constant<(IsSparseArgV<Ts> || ...)> {};

        // Every component a query NAMES, through any modifier (ExtractComponents skips
        // Optional/Not/With/Changed/Added/IncludeDisabled, so it cannot serve here).
        template<typename A> struct ArgComponents                  { using type = std::tuple<std::remove_const_t<A>>; };
        template<typename T> struct ArgComponents<Optional<T>>        { using type = std::tuple<std::remove_const_t<T>>; };
        template<typename T> struct ArgComponents<Not<T>>             { using type = std::tuple<std::remove_const_t<T>>; };
        template<typename T> struct ArgComponents<IncludeDisabled<T>> { using type = std::tuple<std::remove_const_t<T>>; };
        template<typename T> struct ArgComponents<With<T>>            { using type = std::tuple<std::remove_const_t<T>>; };
        template<typename T> struct ArgComponents<Changed<T>>         { using type = std::tuple<std::remove_const_t<T>>; };
        template<typename T> struct ArgComponents<Added<T>>           { using type = std::tuple<std::remove_const_t<T>>; };
        template<typename... Ts> struct ArgComponents<Any<Ts...>>     { using type = std::tuple<std::remove_const_t<Ts>...>; };
        template<typename... Ts> struct ArgComponents<OneOf<Ts...>>   { using type = std::tuple<std::remove_const_t<Ts>...>; };

        template<typename... QueryArgs>
        using QueryPlanTypes = typename UniqueTypes<decltype(std::tuple_cat(
            std::declval<typename ArgComponents<QueryArgs>::type>()...))>::type;

        // Position of T in a tuple (== tuple size when absent).
        template<typename T, typename Tuple> struct TupleIndexOf;
        template<typename T> struct TupleIndexOf<T, std::tuple<>> { static constexpr size_t value = 0; };
        template<typename T, typename U, typename... Us>
        struct TupleIndexOf<T, std::tuple<U, Us...>>
        {
            static constexpr size_t value = std::is_same_v<T, U> ? 0 : 1 + TupleIndexOf<T, std::tuple<Us...>>::value;
        };
```

In `QueryBuilder`, add after `GetChangeMask()`:

```cpp
        // Storage policies (spec 2026-10-01 s6.1): the archetype-mask candidates. A
        // Sparse-TRAIT member is never in an archetype mask, so it is left out here;
        // View additionally removes the members its registry declares Universal.
        static ComponentMask GetTableRequiredMask()
        {
            return MakeMaskFromTuple<Detail::FilterTableTrait_t<typename Classifier::RequiredComponents>>()
                 | MakeMaskFromTuple<Detail::FilterTableTrait_t<typename Classifier::WithComponents>>()
                 | MakeMaskFromTuple<Detail::FilterTableTrait_t<typename Classifier::ChangedComponents>>()
                 | MakeMaskFromTuple<Detail::FilterTableTrait_t<typename Classifier::AddedComponents>>();
        }

        static ComponentMask GetTableExcludedMask()
        {
            return MakeMaskFromTuple<Detail::FilterTableTrait_t<typename Classifier::ExcludedComponents>>();
        }
```

- [ ] **Step 5: Give `ArchetypeManager` a non-owning storage pointer, and wire it**

In `include/Astra/Archetype/ArchetypeManager.hpp`:

1. Next to the existing `template<typename... QueryArgs> class View;` forward declaration, add `class ComponentStorage;`.
2. After `ArchetypeChunkPool& GetChunkPool() { return m_chunkPool; }`, add:

```cpp
        // Storage policies (spec 2026-10-01 s6): the owning Registry's ComponentStorage,
        // so View and Relations -- which hold only this manager -- reach sparse sets and
        // universal columns. Non-owning, exactly like m_records: never retain a manager
        // (or a View) past its Registry. Clear() keeps it (the storage is cleared in place).
        void SetComponentStorage(ComponentStorage* storage) noexcept { m_componentStorage = storage; }
        ASTRA_NODISCARD ComponentStorage* GetComponentStorage() const noexcept { return m_componentStorage; }
```

3. After the `EntityTable* m_records = nullptr;` member, add `ComponentStorage* m_componentStorage = nullptr;   // non-owning; see SetComponentStorage`.

In `include/Astra/Registry/Registry.hpp`:

1. In each of the three constructors, append this to the body, after Task 9's `m_componentStorage->SetTickSource(...)` statement:

```cpp
            m_archetypeManager->SetComponentStorage(m_componentStorage.get());
```

2. In `LoadInternal`, add this statement once, directly after Task 9 (c)'s `registry->m_componentStorage->SetTickSource(...)` line. At that point BOTH the ArchetypeManager and the ComponentStorage have been rebuilt (Task 5 (g)), and `m_archetypeManager->Deserialize` has not run yet. Do NOT place it directly after the `make_shared<ArchetypeManager>` line: that is before the storage rebuild, so the manager would hold a pointer to the constructor-built store, which the rebuild destroys. No later step rebuilds the store, so one copy suffices:

```cpp
            registry->m_archetypeManager->SetComponentStorage(registry->m_componentStorage.get());
```

- [ ] **Step 6: Build the plan in `View`**

In `include/Astra/Registry/View.hpp`:

1. Add `#include <array>` to the standard includes and `#include "ComponentStorage.hpp"` beside `#include "EnabledRuns.hpp"`.
2. Insert this block directly after the `HasTrackedYield` definition (before the "Parallel execution thresholds" comment):

```cpp
        // ================= Storage policies (spec 2026-10-01 s6) =================
        //
        // PlanTypes: every component the query names (through any modifier), const
        // stripped, deduplicated. m_kinds[PlanIndex<T>] is T's effective StorageKind in
        // this view's registry, resolved ONCE at construction: Table/Sparse from the
        // trait, Universal from the registry's Config (fixed for the registry's life).
        using PlanTypes       = Detail::QueryPlanTypes<QueryArgs...>;
        using WithTypes       = typename Detail::QueryClassifier<QueryArgs...>::WithComponents;
        using ExcludedTypes   = typename Detail::QueryClassifier<QueryArgs...>::ExcludedComponents;
        using AnyGroupTypes   = typename Detail::QueryClassifier<QueryArgs...>::AnyGroups;
        using OneOfGroupTypes = typename Detail::QueryClassifier<QueryArgs...>::OneOfGroups;
        static constexpr size_t NPlan      = std::tuple_size_v<PlanTypes>;
        static constexpr size_t NPlanSlots = NPlan == 0 ? 1 : NPlan;
        template<typename T>
        static constexpr size_t PlanIndex = Detail::TupleIndexOf<std::remove_const_t<T>, PlanTypes>::value;
    public:
        // Compile-time half of the plan: a Sparse-trait type anywhere in the query routes
        // every iteration surface through the storage path (spec s6.3, s6.5).
        static constexpr bool HasSparseArg = Detail::AnySparseTrait<PlanTypes>::value;
    private:
```

3. In the constructor, call `BuildStoragePlan();` first in the body, before the `if (manager)` block. A null manager yields an all-trait plan.
4. In `VisibleRecord`, replace `if (!QueryBuilder::Matches(rec->archetype->GetMask())) return nullptr;` with `if (!MatchesArchetype(rec->archetype->GetMask())) return nullptr;`.
5. In `EnsureArchetypes`, replace `if (QueryBuilder::Matches(arch->GetMask()))` with `if (MatchesArchetype(arch->GetMask()))`.
6. In `CollectArchetypes`:
   - delete the `queryComponentCount` local and its three-line initializer;
   - replace `if (archetype->GetComponentCount() < queryComponentCount) ASTRA_UNLIKELY` with `if (archetype->GetComponentCount() < m_matchCount) ASTRA_UNLIKELY`;
   - replace `if (QueryBuilder::Matches(archetype->GetMask()))` with `if (MatchesArchetype(archetype->GetMask()))`.
7. Add these private members directly before `struct ArchetypeEntityCountComparator`:

```cpp
        // Effective kind of one named type. KindOf reads T's descriptor, so it is asked
        // only for a REGISTERED id; an unregistered type cannot be Universal
        // (AddUniversal registers it), so its trait decides.
        template<typename T>
        StorageKind ResolveKind(ComponentStorage* storage, const ComponentRegistry* creg) const
        {
            const ComponentID id = TypeID<T>::Value();
            if (storage && creg && id < MAX_COMPONENTS && creg->GetComponentDescriptor(id) &&
                storage->KindOf(id) == StorageKind::Universal)
                return StorageKind::Universal;
            return Detail::IsSparseArgV<T> ? StorageKind::Sparse : StorageKind::Table;
        }

        void BuildStoragePlan()
        {
            m_kinds.fill(StorageKind::Table);
            m_matchRequired = QueryBuilder::GetTableRequiredMask();
            m_matchExcluded = QueryBuilder::GetTableExcludedMask();
            m_anyUniversal = false;
            m_neverMatches = false;

            ComponentStorage* storage = m_archetypeManager ? m_archetypeManager->GetComponentStorage() : nullptr;
            std::shared_ptr<ComponentRegistry> creg =
                m_archetypeManager ? m_archetypeManager->m_componentRegistry.lock() : nullptr;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((m_kinds[PlanIndex<Ts>] = ResolveKind<Ts>(storage, creg.get())), ...);
            }(static_cast<PlanTypes*>(nullptr));

            // A Universal member is in no archetype mask: it leaves both masks.
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((m_kinds[PlanIndex<Ts>] == StorageKind::Universal
                    ? (m_anyUniversal = true,
                       m_matchRequired.Reset(TypeID<Ts>::Value()),
                       m_matchExcluded.Reset(TypeID<Ts>::Value()))
                    : void()), ...);
            }(static_cast<PlanTypes*>(nullptr));

            // Not<U> on a Universal U: every live entity holds U, so nothing matches.
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((m_neverMatches = m_neverMatches || m_kinds[PlanIndex<Ts>] == StorageKind::Universal), ...);
            }(static_cast<ExcludedTypes*>(nullptr));

            m_matchCount = m_matchRequired.Count();
        }

        // Archetype-level match against the plan's runtime masks: QueryBuilder::Matches
        // with the Sparse/Universal members taken out (spec s6.1).
        ASTRA_NODISCARD bool MatchesArchetype(const ComponentMask& mask) const
        {
            if (m_neverMatches) ASTRA_UNLIKELY
                return false;
            if (!mask.HasAll(m_matchRequired))
                return false;
            if ((mask & m_matchExcluded).Any())
                return false;
            return GroupsPass(mask);
        }

        // Any/OneOf at archetype level. Task 26 makes this storage-aware.
        ASTRA_NODISCARD bool GroupsPass([[maybe_unused]] const ComponentMask& mask) const
        {
            constexpr size_t nAny = std::tuple_size_v<AnyGroupTypes>;
            constexpr size_t nOne = std::tuple_size_v<OneOfGroupTypes>;
            bool ok = true;
            if constexpr (nAny > 0)
                ok = QueryBuilder::template CheckAnyGroups<AnyGroupTypes>(mask, std::make_index_sequence<nAny>{});
            if constexpr (nOne > 0)
                ok = ok && QueryBuilder::template CheckOneOfGroups<OneOfGroupTypes>(mask, std::make_index_sequence<nOne>{});
            return ok;
        }
```

8. Add these data members directly after `uint32_t m_lastRemovalCounter = 0;`:

```cpp
        // Storage plan (spec 2026-10-01 s6.1), built once by BuildStoragePlan().
        std::array<StorageKind, NPlanSlots> m_kinds{};
        ComponentMask m_matchRequired{};   // Table-kind required | With | Changed | Added
        ComponentMask m_matchExcluded{};   // Table-kind Not<>
        size_t m_matchCount = 0;           // m_matchRequired.Count(): CollectArchetypes' pre-filter
        bool m_anyUniversal = false;       // some named type is Universal in this registry
        bool m_neverMatches = false;       // Not<U> on a Universal U
```

The plan changes no iteration loop yet. Size() on a Universal-requiring view is already right: no fetch happens, and `m_archetypes` now holds every archetype.

- [ ] **Step 7: Run the tests**

Build, then: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewPlan.*:View*:Query*:EnableableTag*`
Expected: all PASS.

Then the full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`
Expected: all PASS. Record the seed line.

- [ ] **Step 8: Commit**

```powershell
git add include/Astra/Registry/Query.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp include/Astra/Registry/View.hpp tests/Registry/StorageQueryComponents.hpp tests/Registry/StorageViewPlanTest.cpp
git commit -m "feat(storage): query classification + per-view storage plan -- only Table-kind arguments reach archetype masks, CollectArchetypes and the view cache; Universal resolved per registry; ArchetypeManager carries the ComponentStorage (s6.1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 23: `ForEachMatchingRun` -- the generalised run extractor (s6.3)

The match word is `AND(require[k][w]) & ~OR(exclude[k][w])`, tail-masked to `count`, and runs are maximal and ascending, exactly as today. `ForEachEnabledRun` stays as a one-line wrapper (exclude only), so every existing caller and its tests are unchanged.

**Files:**
- Modify: `include/Astra/Registry/EnabledRuns.hpp`
- Create: `tests/Registry/MatchingRunsTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace Astra::Detail {
    template<class Fn> void ForEachMatchingRun(const uint64_t* const* exclude, size_t nExclude,
                                               const uint64_t* const* require, size_t nRequire, size_t count, Fn&& fn);
    template<class Fn> void ForEachEnabledRun(const uint64_t* const* wordSets, size_t setCount, size_t count, Fn&& fn); // wrapper
    size_t CountMatching(const uint64_t* const* exclude, size_t nExclude, const uint64_t* const* require, size_t nRequire, size_t count);
    void   WriteMatchWords(const uint64_t* const* exclude, size_t nExclude, const uint64_t* const* require, size_t nRequire,
                           size_t count, uint64_t* out);   // out: (count + 63) / 64 words
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/MatchingRunsTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Registry/EnabledRuns.hpp>
#include <random>
#include <utility>
#include <vector>

namespace
{
    using Runs = std::vector<std::pair<size_t, size_t>>;

    // Brute-force reference: index i matches iff every require set has bit i and no
    // exclude set has it. Runs are maximal and ascending.
    Runs Reference(const std::vector<std::vector<uint64_t>>& ex, const std::vector<std::vector<uint64_t>>& req, size_t count)
    {
        Runs runs;
        auto bit = [](const std::vector<uint64_t>& w, size_t i) { return ((w[i >> 6] >> (i & 63)) & 1ull) != 0; };
        size_t i = 0;
        while (i < count)
        {
            auto match = [&](size_t k)
            {
                for (const auto& r : req) if (!bit(r, k)) return false;
                for (const auto& e : ex)  if (bit(e, k))  return false;
                return true;
            };
            if (!match(i)) { ++i; continue; }
            const size_t b = i;
            while (i < count && match(i)) ++i;
            runs.emplace_back(b, i);
        }
        return runs;
    }

    std::vector<const uint64_t*> Ptrs(const std::vector<std::vector<uint64_t>>& sets)
    {
        std::vector<const uint64_t*> p;
        for (const auto& s : sets) p.push_back(s.data());
        return p;
    }

    Runs Collect(const std::vector<std::vector<uint64_t>>& ex, const std::vector<std::vector<uint64_t>>& req, size_t count)
    {
        Runs runs;
        auto pe = Ptrs(ex);
        auto pr = Ptrs(req);
        Astra::Detail::ForEachMatchingRun(pe.data(), pe.size(), pr.data(), pr.size(), count,
            [&](size_t b, size_t e) { runs.emplace_back(b, e); });
        return runs;
    }
}

TEST(MatchingRuns, NoSetsYieldsOneWholeRun)
{
    EXPECT_EQ(Collect({}, {}, 130), (Runs{{0, 130}}));
    EXPECT_TRUE(Collect({}, {}, 0).empty());
}

TEST(MatchingRuns, RequireOnlyStitchesAcrossTheWordBoundary)
{
    std::vector<uint64_t> req(2, 0);
    req[0] = 0xF000000000000000ull;   // bits 60..63
    req[1] = 0x0000000000000007ull;   // bits 64..66
    EXPECT_EQ(Collect({}, {req}, 128), (Runs{{60, 67}}));
}

TEST(MatchingRuns, RequireAndExcludeCombine)
{
    std::vector<uint64_t> req(1, 0b1111'1111ull);
    std::vector<uint64_t> ex(1, 0b0010'0100ull);
    EXPECT_EQ(Collect({ex}, {req}, 8), (Runs{{0, 2}, {3, 5}, {6, 8}}));
}

TEST(MatchingRuns, GarbageTailBitsAreMasked)
{
    std::vector<uint64_t> req(1, ~0ull);   // every bit set, including beyond count
    EXPECT_EQ(Collect({}, {req}, 10), (Runs{{0, 10}}));
    EXPECT_EQ(Astra::Detail::CountMatching(nullptr, 0, Ptrs({req}).data(), 1, 10), 10u);
}

TEST(MatchingRuns, RandomSetsAgreeWithTheReferenceAndTheHelpers)
{
    std::mt19937_64 rng(0xA57A);
    for (int round = 0; round < 300; ++round)
    {
        const size_t count = 1 + rng() % 400;
        const size_t words = (count + 63) / 64;
        auto randomSet = [&] { std::vector<uint64_t> s(words); for (auto& w : s) w = rng() | rng(); return s; };
        std::vector<std::vector<uint64_t>> ex(rng() % 3), req(rng() % 3);
        for (auto& s : ex)  s = randomSet();
        for (auto& s : req) s = randomSet();

        const Runs expected = Reference(ex, req, count);
        ASSERT_EQ(Collect(ex, req, count), expected) << "round " << round;

        size_t total = 0;
        for (auto [b, e] : expected) total += e - b;
        auto pe = Ptrs(ex);
        auto pr = Ptrs(req);
        ASSERT_EQ(Astra::Detail::CountMatching(pe.data(), pe.size(), pr.data(), pr.size(), count), total);

        std::vector<uint64_t> out(words, 0xDEADull);
        Astra::Detail::WriteMatchWords(pe.data(), pe.size(), pr.data(), pr.size(), count, out.data());
        ASSERT_EQ(Collect({}, {out}, count), expected) << "round " << round;
        if (count % 64) ASSERT_EQ(out.back() >> (count % 64), 0ull);   // tail cleared

        // The kept wrapper is exclude-only and agrees with ForEachMatchingRun.
        Runs viaWrapper;
        Astra::Detail::ForEachEnabledRun(pe.data(), pe.size(), count, [&](size_t b, size_t e) { viaWrapper.emplace_back(b, e); });
        ASSERT_EQ(viaWrapper, Reference(ex, {}, count));
    }
}
```

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, then build.
Expected: the build FAILS with `'ForEachMatchingRun': is not a member of 'Astra::Detail'` (and the same for `CountMatching` and `WriteMatchWords`).

- [ ] **Step 3: Generalise the extractor**

In `include/Astra/Registry/EnabledRuns.hpp`, replace the whole `ForEachEnabledRun` definition (the doc comment stays and now sits above the wrapper) with:

```cpp
    /**
     * Match word `w` of the generalised extractor (spec 2026-10-01 s6.3):
     * AND over every require set, AND NOT the OR over every exclude set, with the
     * bits at or beyond `count` masked off. With no sets at all every valid bit is set.
     */
    inline uint64_t MatchWord(const uint64_t* const* exclude, size_t nExclude,
                              const uint64_t* const* require, size_t nRequire,
                              size_t w, size_t count) noexcept
    {
        uint64_t m = ~uint64_t(0);
        for (size_t k = 0; k < nRequire; ++k)
            m &= require[k][w];
        uint64_t ex = 0;
        for (size_t k = 0; k < nExclude; ++k)
            ex |= exclude[k][w];
        m &= ~ex;
        const size_t validBits = count - (w << 6);       // in [1, 64] for the last word
        if (validBits < 64)
            m &= (uint64_t(1) << validBits) - 1;
        return m;
    }

    /**
     * Generalised run extraction (spec 2026-10-01 s6.3). Invokes `fn(begin, end)` for
     * every maximal run of indices in [0, count) whose bit is set in EVERY `require`
     * set and in NO `exclude` set. Runs are ascending and stitched across word
     * boundaries, exactly like the enabled-run walk this generalises. Each set points
     * at (count + 63) / 64 words. Tail bits are masked, so whole-word regions may be
     * passed. Both lists empty: the single run [0, count).
     */
    template<class Fn>
    inline void ForEachMatchingRun(const uint64_t* const* exclude, size_t nExclude,
                                   const uint64_t* const* require, size_t nRequire,
                                   size_t count, Fn&& fn)
    {
        if (count == 0)
            return;

        const size_t numWords = (count + 63) >> 6;
        bool   open = false;    // a run left open at the top of the previous word
        size_t openStart = 0;

        for (size_t w = 0; w < numWords; ++w)
        {
            uint64_t e = MatchWord(exclude, nExclude, require, nRequire, w, count);
            const size_t base = w << 6;

            // Stitch a run carried over from the previous word.
            if (open)
            {
                if (e & 1ull)
                {
                    const int cont = std::countr_one(e);
                    if (cont == 64)
                        continue;                            // whole word continues the run
                    fn(openStart, base + static_cast<size_t>(cont));
                    open = false;
                    e &= ~((uint64_t(1) << cont) - 1);       // consume the leading ones
                }
                else
                {
                    fn(openStart, base);                     // run ended at the word boundary
                    open = false;
                }
            }

            // Scan the remaining maximal one-runs inside this word.
            while (e != 0ull)
            {
                const int start = std::countr_zero(e);
                const int len   = std::countr_one(e >> start);
                const int end   = start + len;               // exclusive, within (0, 64]
                if (end == 64)
                {
                    open = true;                             // reaches the top: carry to next word
                    openStart = base + static_cast<size_t>(start);
                    break;
                }
                fn(base + static_cast<size_t>(start), base + static_cast<size_t>(end));
                e &= ~(((uint64_t(1) << len) - 1) << start); // clear [start, end)
            }
        }

        if (open)
            fn(openStart, count);
    }

    // The pre-existing enabled-run entry point (doc comment above): `wordSets` are
    // EXCLUDE sets (SET bit == disabled). Kept so every existing caller is unchanged.
    template<class Fn>
    inline void ForEachEnabledRun(const uint64_t* const* wordSets, size_t setCount,
                                  size_t count, Fn&& fn)
    {
        ForEachMatchingRun(wordSets, setCount, nullptr, 0, count, std::forward<Fn>(fn));
    }

    // Number of matching indices (View::Size over a filtered chunk).
    inline size_t CountMatching(const uint64_t* const* exclude, size_t nExclude,
                                const uint64_t* const* require, size_t nRequire, size_t count) noexcept
    {
        size_t total = 0;
        const size_t numWords = (count + 63) >> 6;
        for (size_t w = 0; w < numWords; ++w)
            total += static_cast<size_t>(std::popcount(MatchWord(exclude, nExclude, require, nRequire, w, count)));
        return total;
    }

    // Materialise the match words into `out` ((count + 63) / 64 words, tail cleared):
    // the per-chunk mask a range-for iterator steps through (Task 29).
    inline void WriteMatchWords(const uint64_t* const* exclude, size_t nExclude,
                                const uint64_t* const* require, size_t nRequire,
                                size_t count, uint64_t* out) noexcept
    {
        const size_t numWords = (count + 63) >> 6;
        for (size_t w = 0; w < numWords; ++w)
            out[w] = MatchWord(exclude, nExclude, require, nRequire, w, count);
    }
```

`count - (w << 6)` cannot underflow: `MatchWord` is called only for `w < numWords`.

- [ ] **Step 4: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=MatchingRuns.*:*Enable*:ChangeDetection*`
Expected: all PASS. The existing enableable and change-detection suites still pass through the wrapper.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Registry/EnabledRuns.hpp tests/Registry/MatchingRunsTest.cpp
git commit -m "feat(storage): ForEachMatchingRun -- the enabled-run extractor generalised to AND(require) & ~OR(exclude); ForEachEnabledRun kept as the exclude-only wrapper; CountMatching + WriteMatchWords (s6.3)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 24: The storage chunk walk -- filters, fetchers, `Optional`, serial `ForEach` and `Size` (s6.3, s6.4)

`VisitChunkFiltered` dispatches every storage-path view to the new `VisitChunkStorage`. That covers a view naming a Sparse type, and a view whose registry declares one of its types Universal. `VisitChunkStorage` builds the chunk's word sets:
- a required or `With<>` Sparse argument adds its block's `present` words to `require`, and a missing block skips the chunk;
- `Not<Sparse>` adds `present` to `exclude`;
- an enableable required argument adds its disabled words: the column for Table, the chunk mirror for Sparse/Universal. `IncludeDisabled<T>` opts out.

It then runs `ForEachMatchingRun` and fetches each yield through a per-chunk `Fetch<T>`. The old filtered loop survives as `VisitChunkTable` for pure-Table views (G4).

**Files:**
- Modify: `include/Astra/Registry/View.hpp`
- Create: `tests/Registry/StorageViewFilterTest.cpp`

**Interfaces:**
- Consumes: `ComponentStorage::SparseSlotOf / Sparse / Universal`, `ArchetypeChunk::FindSparseBlock`, `SparseSet::Get / IsDisabled`, `UniversalColumn::Get / IsDisabled`, `Detail::ForEachMatchingRun / CountMatching` (Task 23), the plan (Task 22).
- Produces (all `View` private, used by Tasks 25-29): `StoreRef`, `StoreArray`, `m_stores`, `ResolveStores()`, `ResolveStoresInto(StoreArray&) const`, `UsesStoragePath()`, `ChunkFilter`, `BuildChunkFilter(Archetype*, ArchetypeChunk*, size_t, ChunkFilter&) const`, `Fetch<T>`, `MakeFetch<T>(ArchetypeChunk*, const ArchetypeColumnMeta&, const StoreArray&) const`, `InvokeStorageEntity<Stamp>(...)`, `VisitChunkStorage<Stamp>(...)`, `VisitChunkTable<Stamp>(...)` (the old body), `SizeStorage(Archetype*)`.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageViewFilterTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <map>
#include <set>
#include <span>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    template<typename V>
    std::set<Astra::Entity> Visited(V&& view)
    {
        std::set<Astra::Entity> s;
        view.ForEach([&](Astra::Entity e, auto&&...) { s.insert(e); });
        return s;
    }
}

// s6.3: a required Sparse argument filters per entity (present words) and skips every
// chunk without a presence block; Not<Sparse> excludes on the same words.
TEST(StorageViewFilter, RequiredSparseFiltersPerEntityAndSkipsChunks)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> a, b;
    for (int i = 0; i < 300; ++i) a.push_back(reg.CreateEntityWith(Position{float(i), 0, 0}));
    for (int i = 0; i < 300; ++i) b.push_back(reg.CreateEntityWith(Position{float(i), 0, 0}, Velocity{}));
    std::set<Astra::Entity> holders;
    for (int i = 0; i < 300; i += 3)
    {
        ASSERT_TRUE(reg.AddComponent(b[i], PosS{float(i), 1.0f, 2.0f}));
        holders.insert(b[i]);
    }

    auto view = reg.CreateView<Position, PosS>();
    std::map<Astra::Entity, float> seen;
    view.ForEach([&](Astra::Entity e, Position& p, PosS& s)
    {
        seen[e] = s.x;
        EXPECT_FLOAT_EQ(p.x, s.x);
    });
    EXPECT_EQ(seen.size(), holders.size());
    for (Astra::Entity e : holders) EXPECT_EQ(seen.count(e), 1u);
    EXPECT_EQ(view.Size(), holders.size());
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<PosS>>().Size()), 600u - holders.size());
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::Not<PosS>>()).count(b[0]), 0u);
}

TEST(StorageViewFilter, WritesThroughSparseYieldsPersist)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 64; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), PosS{float(i), 1.0f, 0.0f});
    }
    reg.CreateView<PosS>().ForEach([](PosS& s) { s.y += 10.0f; });
    for (int i = 0; i < 64; ++i) EXPECT_FLOAT_EQ(reg.GetComponent<PosS>(es[i])->y, 11.0f);
}

TEST(StorageViewFilter, SparseTagsFilterAsWithRequiredAndNot)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 100; ++i) es.push_back(reg.CreateEntity<Position>());
    for (int i = 0; i < 100; i += 4) reg.AddComponent(es[i], SpTag{});
    EXPECT_EQ((reg.CreateView<Position, Astra::With<SpTag>>().Size()), 25u);
    size_t n = 0;
    reg.CreateView<Position, SpTag>().ForEach([&](Astra::Entity, Position&, SpTag&) { ++n; });
    EXPECT_EQ(n, 25u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<SpTag>>().Size()), 75u);
}

// The disabled MIRROR (P3) filters an enableable Sparse argument; IncludeDisabled opts out.
TEST(StorageViewFilter, EnableableSparseUsesTheDisabledMirror)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 10; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), SpE{float(i)});
    }
    ASSERT_TRUE(reg.SetEnabled<SpE>(es[2], false));
    ASSERT_TRUE(reg.SetEnabled<SpE>(es[7], false));

    EXPECT_EQ((reg.CreateView<Position, SpE>().Size()), 8u);
    const auto seen = Visited(reg.CreateView<Position, SpE>());
    EXPECT_EQ(seen.size(), 8u);
    EXPECT_EQ(seen.count(es[2]) + seen.count(es[7]), 0u);
    EXPECT_EQ((reg.CreateView<Position, Astra::IncludeDisabled<SpE>>().Size()), 10u);

    size_t nulls = 0;
    reg.CreateView<Position, Astra::Optional<const SpE>>().ForEach([&](Astra::Entity, Position&, const SpE* s) { nulls += (s == nullptr); });
    EXPECT_EQ(nulls, 2u);
}

// s6.4: Universal elements are fetched by entity index through the page cursor; 3000
// entities span three 1024-slot pages.
TEST(StorageViewFilter, UniversalYieldsAcrossPages)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es(3000);
    ASSERT_EQ(reg->CreateEntities<Position>(es.size(), std::span{es}), es.size());
    for (size_t i = 0; i < es.size(); ++i) reg->GetComponent<Health>(es[i])->current = int(i);

    std::map<Astra::Entity, int> seen;
    reg->CreateView<Position, Health>().ForEach([&](Astra::Entity e, Position&, Health& h)
    {
        seen[e] = h.current;
        h.max = 7;
    });
    ASSERT_EQ(seen.size(), es.size());
    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(seen[es[i]], int(i));
        EXPECT_EQ(reg->GetComponent<Health>(es[i])->max, 7);
    }
    (void)reg->CreateEntity();
    EXPECT_EQ(reg->CreateView<Health>().Size(), es.size() + 1);
}

// An enableable Universal component filters on the chunk's disabled mirror.
TEST(StorageViewFilter, EnableableUniversalUsesTheDisabledMirror)
{
    auto reg = MakeRegistry<Timer>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 50; ++i) es.push_back(reg->CreateEntity<Position>());
    for (int i = 0; i < 50; i += 5) ASSERT_TRUE(reg->SetEnabled<Timer>(es[i], false));

    EXPECT_EQ((reg->CreateView<Position, Timer>().Size()), 40u);
    const auto seen = Visited(reg->CreateView<Timer>());
    EXPECT_EQ(seen.size(), 40u);
    EXPECT_EQ(seen.count(es[0]), 0u);
    EXPECT_EQ(reg->CreateView<Astra::IncludeDisabled<Timer>>().Size(), 50u);

    size_t nulls = 0;
    reg->CreateView<Position, Astra::Optional<Timer>>().ForEach([&](Astra::Entity, Position&, Timer* t) { nulls += (t == nullptr); });
    EXPECT_EQ(nulls, 10u);
}

TEST(StorageViewFilter, OptionalSparseAndUniversalYieldPointers)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 20; ++i)
    {
        es.push_back(reg->CreateEntity<Position>());
        if (i % 2 == 0) reg->AddComponent(es.back(), PosS{float(i), 0.0f, 0.0f});
    }
    size_t visits = 0;
    reg->CreateView<Position, Astra::Optional<PosS>, Astra::Optional<const Health>>().ForEach(
        [&](Astra::Entity e, Position&, PosS* s, const Health* h)
        {
            ++visits;
            const size_t i = size_t(std::find(es.begin(), es.end(), e) - es.begin());
            EXPECT_EQ(s != nullptr, i % 2 == 0);
            if (s) EXPECT_FLOAT_EQ(s->x, float(i));
            EXPECT_NE(h, nullptr);
        });
    EXPECT_EQ(visits, 20u);
}

// Universal overrides the trait: a Sparse-trait type declared Universal is on every entity.
TEST(StorageViewFilter, UniversalOverridesASparseTrait)
{
    auto reg = MakeRegistry<PosS>();
    for (int i = 0; i < 30; ++i) (void)reg->CreateEntity<Position>();
    size_t n = 0;
    reg->CreateView<Position, PosS>().ForEach([&](Astra::Entity, Position&, PosS& s) { ++n; s.z = 3.0f; });
    EXPECT_EQ(n, 30u);
    EXPECT_EQ((reg->CreateView<Position, PosS>().Size()), 30u);
}
```

Add `#include <algorithm>` to the include block (for `std::find`).

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewFilter.*`
Expected: FAIL.
- `RequiredSparseFiltersPerEntityAndSkipsChunks` reports `Size() == 600`: Task 22's mask drops the Sparse member, so every Position archetype matches.
- The first `ForEach` over a Sparse or Universal yield crashes with an access violation: the pre-existing loop indexes a null column base.
The crash ends the run. That is the expected red.

- [ ] **Step 3: Gate the filtered path on Sparse arguments**

In `include/Astra/Registry/View.hpp`:

1. Delete the line `static constexpr bool HasChunkFilter = HasEnabledFilter || HasChangeFilter;` and its two-line comment.
2. Re-add it directly after the `private:` line that closes Task 22's `HasSparseArg` declaration, followed by the storage-path type lists. A `static constexpr` initializer cannot name a member declared below it, which is why the definition moves:

```cpp
        // Any per-chunk filter at all: routes iteration through VisitChunkFiltered. A
        // Sparse argument always filters (presence words), so it joins the gate (s6.3).
        static constexpr bool HasChunkFilter = HasEnabledFilter || HasChangeFilter || HasSparseArg;

        // Sparse-trait members of the required (+ IncludeDisabled) and With<> sets --
        // the presence words a chunk must carry -- and of the Not<> set.
        using SparseRequiredTypes = Detail::FilterSparseTrait_t<decltype(std::tuple_cat(
            std::declval<RequiredTypes>(), std::declval<WithTypes>()))>;
        using SparseExcludedTypes = Detail::FilterSparseTrait_t<ExcludedTypes>;
```

`HasRequiredFilter` keeps its meaning: the required **enableable** filter. Only that gate drives the range-for refusal and `EnabledVisible`, so a required Sparse argument never triggers the refusal (s6.5).

- [ ] **Step 4: Add the storage path**

Insert the following block directly before the comment `// Exact visible-entity count for a required-filtered view` (above `SizeFiltered`). It must come after `YieldType`, which it names in a return type.

```cpp
        // ================= Storage path (spec 2026-10-01 s6.3, s6.4) =================
        // Taken by every view naming a Sparse type, and by any view whose registry
        // declares one of its types Universal (UsesStoragePath). Pure-Table views never
        // instantiate or reach it beyond one m_anyUniversal test per archetype walk.

        // One resolved store per plan type. Refreshed on the walking thread before any
        // chunk is visited and before any parallel dispatch: slots are acquired lazily
        // (s3.2), so a sparse set may appear after the view was built. Workers only read.
        struct StoreRef
        {
            int              slot = -1;            // sparse slot (Sparse; enableable Universal's mirror); -1 = none
            SparseSet*       sparse = nullptr;     // Sparse kind with a slot
            UniversalColumn* universal = nullptr;  // Universal kind
        };
        using StoreArray = std::array<StoreRef, NPlanSlots>;

        ASTRA_NODISCARD bool UsesStoragePath() const noexcept
        {
            if constexpr (HasSparseArg)
                return true;
            else
                return m_anyUniversal;
        }

        template<typename T>
        void ResolveStore(ComponentStorage* storage, StoreRef& s) const
        {
            s = StoreRef{};
            if (!storage)
                return;
            const ComponentID id = TypeID<T>::Value();
            switch (m_kinds[PlanIndex<T>])
            {
            case StorageKind::Sparse:
                s.slot = storage->SparseSlotOf(id);
                s.sparse = s.slot >= 0 ? storage->Sparse(id) : nullptr;   // no slot => no member anywhere
                break;
            case StorageKind::Universal:
                s.universal = storage->Universal(id);
                s.slot = storage->SparseSlotOf(id);   // enableable Universal: its disabled-mirror slot
                break;
            case StorageKind::Table:
                break;
            }
        }

        void ResolveStoresInto(StoreArray& out) const
        {
            ComponentStorage* storage = m_archetypeManager ? m_archetypeManager->GetComponentStorage() : nullptr;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                (ResolveStore<Ts>(storage, out[PlanIndex<Ts>]), ...);
            }(static_cast<PlanTypes*>(nullptr));
        }

        void ResolveStores() { ResolveStoresInto(m_stores); }

        // ---- Per-chunk word sets, fed to Detail::ForEachMatchingRun ----
        // Holds word POINTERS only, copied before any callback runs. No SparseBlock* is
        // kept across a callback: a SetEnabled or sparse add made inside one may insert a
        // block into the chunk's block list (Task 15's lifetime rule).
        static constexpr size_t NGroups     = std::tuple_size_v<AnyGroupTypes> + std::tuple_size_v<OneOfGroupTypes>;
        static constexpr size_t MaxWordSets = 3 * NPlanSlots + NGroups + 2;
        struct ChunkFilter
        {
            const uint64_t* exclude[MaxWordSets];
            const uint64_t* require[MaxWordSets];
            size_t nExclude = 0;
            size_t nRequire = 0;
        };

        // Required (or With<>) Sparse T: its block's present words. Returns false when no
        // entity of the chunk holds T (missing block / no slot): the whole-chunk skip.
        template<typename T>
        bool RequirePresent(ArchetypeChunk* chunk, size_t count, ChunkFilter& f) const
        {
            if (m_kinds[PlanIndex<T>] == StorageKind::Universal)
                return true;                                   // declared Universal here: always present
            const StoreRef& s = m_stores[PlanIndex<T>];
            if (s.slot < 0)
                return false;
            const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(s.slot));
            if (!b || b->presentCount == 0)
                return false;
            if (b->presentCount != static_cast<uint32_t>(count))
                f.require[f.nRequire++] = b->present;          // all-present needs no words
            return true;
        }

        // Not<Sparse T>: its present words are excluded. False when every entity holds T.
        template<typename T>
        bool ExcludePresent(ArchetypeChunk* chunk, size_t count, ChunkFilter& f) const
        {
            if (m_kinds[PlanIndex<T>] != StorageKind::Sparse)
                return true;                                   // Not<Universal> never reaches a chunk (m_neverMatches)
            const StoreRef& s = m_stores[PlanIndex<T>];
            if (s.slot < 0)
                return true;
            const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(s.slot));
            if (!b || b->presentCount == 0)
                return true;
            if (b->presentCount == static_cast<uint32_t>(count))
                return false;
            f.exclude[f.nExclude++] = b->present;
            return true;
        }

        // Enabled-only filter of one required enableable T (IncludeDisabled opts out
        // upstream): the column's disabled words for Table, the chunk's disabled MIRROR
        // (P3) for Sparse/Universal. False when every entity of the chunk is disabled.
        template<typename T>
        bool ExcludeDisabled(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, size_t count, ChunkFilter& f) const
        {
            using Bare = std::remove_const_t<T>;
            const uint64_t* words = nullptr;
            uint32_t dc = 0;
            if (m_kinds[PlanIndex<Bare>] == StorageKind::Table)
            {
                const int col = cm.idToColumn[TypeID<Bare>::Value()];   // required => present
                dc = chunk->GetDisabledCount(col);
                words = chunk->GetDisabledWords(col);
            }
            else
            {
                const StoreRef& s = m_stores[PlanIndex<Bare>];
                if (s.slot < 0)
                    return true;
                const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(s.slot));
                if (!b || !b->disabled)
                    return true;
                dc = b->disabledCount;
                words = b->disabled;
            }
            if (dc == 0)
                return true;
            if (dc == static_cast<uint32_t>(count))
                return false;
            f.exclude[f.nExclude++] = words;
            return true;
        }

        // Every per-chunk constraint. Returns false when nothing in the chunk can match.
        bool BuildChunkFilter(Archetype* archetype, [[maybe_unused]] ArchetypeChunk* chunk,
                              [[maybe_unused]] size_t count, [[maybe_unused]] ChunkFilter& f) const
        {
            [[maybe_unused]] const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
            bool pass = true;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((pass = pass && RequirePresent<std::remove_const_t<Ts>>(chunk, count, f)), ...);
            }(static_cast<SparseRequiredTypes*>(nullptr));
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((pass = pass && ExcludeDisabled<Ts>(chunk, cm, count, f)), ...);
            }(static_cast<EnabledRequiredFilter*>(nullptr));
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((pass = pass && ExcludePresent<std::remove_const_t<Ts>>(chunk, count, f)), ...);
            }(static_cast<SparseExcludedTypes*>(nullptr));
            return pass;
        }
```

Continue the same block:

```cpp
        // ---- Fetching (spec s6.4) ----
        // Per-chunk element source of one yielded type; At() is the per-entity fetch.
        // The kind is fixed for the chunk: Table base[i]; Sparse the set's dense element
        // by entity; Universal the slot-indexed page element, with the page pointer
        // cached across consecutive entities of one ASTRA_UNIVERSAL_PAGE_SLOTS page.
        template<typename T>
        struct Fetch
        {
            using Bare = std::remove_const_t<T>;
            Bare*            base = nullptr;        // Table: column base in this chunk
            ArchetypeChunk*  chunk = nullptr;       // Table: for the disabled-bit test
            int              col = -1;              // Table: column ordinal, -1 = absent here
            EntityTicks*     ticks = nullptr;       // Table: tick column (tracked only)
            SparseSet*       sparse = nullptr;      // Sparse
            UniversalColumn* universal = nullptr;   // Universal
            uint32_t         page = UINT32_MAX;     // Universal page cursor
            std::byte*       pageBase = nullptr;

            ASTRA_FORCEINLINE Bare* At([[maybe_unused]] Entity e, [[maybe_unused]] size_t i) noexcept
            {
                if constexpr (std::is_empty_v<Bare>)
                {
                    static Bare s_emptyInstance{};    // a present tag (RequiredElement's contract)
                    return &s_emptyInstance;
                }
                else
                {
                    if (universal)
                        return UniversalAt(e);
                    if (sparse)
                        return static_cast<Bare*>(sparse->Get(e));
                    return base ? base + i : nullptr;
                }
            }

            // Optional yield: null when absent, or disabled under an enableable T. An
            // Optional TAG yields null, the pre-existing ForEach behaviour for Table tags
            // (their column base is null), kept for parity (D2).
            ASTRA_FORCEINLINE Bare* OptionalAt([[maybe_unused]] Entity e, [[maybe_unused]] size_t i) noexcept
            {
                if constexpr (std::is_empty_v<Bare>)
                {
                    return nullptr;
                }
                else
                {
                    Bare* p = At(e, i);
                    if constexpr (IsEnableableV<Bare>)
                    {
                        if (p)
                        {
                            const bool disabled = universal ? universal->IsDisabled(e)
                                                : sparse    ? sparse->IsDisabled(e)
                                                :             chunk->IsDisabled(col, i);
                            if (disabled)
                                return nullptr;
                        }
                    }
                    return p;
                }
            }

            Bare* UniversalAt(Entity e) noexcept
            {
                constexpr uint32_t Slots = static_cast<uint32_t>(ASTRA_UNIVERSAL_PAGE_SLOTS);
                const uint32_t idx  = static_cast<uint32_t>(e.GetID());
                const uint32_t pg   = idx / Slots;
                const size_t   slot = static_cast<size_t>(idx % Slots);
                if (pg != page) ASTRA_UNLIKELY
                {
                    Bare* p = static_cast<Bare*>(universal->Get(e));   // validated lookup, once per page
                    if (!p)
                        return nullptr;
                    page = pg;
                    pageBase = reinterpret_cast<std::byte*>(p) - slot * sizeof(Bare);   // slot 0 of this page
                    return p;
                }
                return reinterpret_cast<Bare*>(pageBase + slot * sizeof(Bare));
            }
        };

        template<typename T>
        ASTRA_FORCEINLINE Fetch<T> MakeFetch(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, const StoreArray& st) const noexcept
        {
            using Bare = std::remove_const_t<T>;
            Fetch<T> f;
            const StoreRef& s = st[PlanIndex<Bare>];
            switch (m_kinds[PlanIndex<Bare>])
            {
            case StorageKind::Universal:
                f.universal = s.universal;
                break;
            case StorageKind::Sparse:
                f.sparse = s.sparse;
                break;
            case StorageKind::Table:
                f.chunk = chunk;
                f.col = cm.idToColumn[TypeID<Bare>::Value()];
                if constexpr (!std::is_empty_v<Bare>)
                {
                    if (f.col >= 0)
                    {
                        f.base = static_cast<Bare*>(chunk->GetComponentArrayByID(TypeID<Bare>::Value()));
                        if constexpr (IsChangeTrackedV<Bare>)
                            f.ticks = chunk->GetTicks(f.col);
                    }
                }
                break;
            }
            return f;
        }

        // Coarse stamp of the non-const TABLE-kind yields present on this archetype.
        // Sparse/Universal elements have no chunk column; Task 25 stamps their stores.
        template<typename T>
        ASTRA_FORCEINLINE void StampTableYield([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                               [[maybe_unused]] Tick now) const
        {
            if constexpr (Detail::IsMutableYield<T>)
            {
                using Bare = std::remove_const_t<T>;
                if (m_kinds[PlanIndex<Bare>] != StorageKind::Table)
                    return;
                const int col = cm.idToColumn[TypeID<Bare>::Value()];
                if (col >= 0)                                   // an Optional absent here has none
                    chunk->StampColumn(col, now);
            }
        }
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void StampChunkStorage([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                                 [[maybe_unused]] Tick now, std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>) const
        {
            (StampTableYield<std::tuple_element_t<ReqIs, RequiredTypes>>(chunk, cm, now), ...);
            (StampTableYield<std::tuple_element_t<OptIs, OptionalTypes>>(chunk, cm, now), ...);
        }

        // Tick slot for a Mut<T> yield. Table: the chunk's tick column. Task 25 wires
        // the Sparse and Universal stores; until then their Mut marks a scratch slot.
        template<typename T>
        ASTRA_FORCEINLINE EntityTicks* TicksFor(Fetch<T>& f, [[maybe_unused]] Entity e, size_t i) const noexcept
        {
            if (f.ticks)
                return f.ticks + i;
            thread_local EntityTicks s_unwired{};
            return &s_unwired;
        }

        template<size_t K, typename F>
        ASTRA_FORCEINLINE typename YieldType<std::tuple_element_t<K, RequiredTypes>>::type
        MakeStorageYield([[maybe_unused]] F& f, std::remove_const_t<std::tuple_element_t<K, RequiredTypes>>* p,
                         [[maybe_unused]] Entity e, [[maybe_unused]] size_t i, [[maybe_unused]] Tick now) const noexcept
        {
            using R = std::tuple_element_t<K, RequiredTypes>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
                return Mut<R>(p, TicksFor(f, e, i), now);
            else
                return *p;
        }
```

Continue the same block:

```cpp
        // One matching entity: fetch, yield, call. A required element missing HERE was
        // removed by a structural change made inside this walk (the Debug guard reports
        // it, s4.3, Task 25): the entity is skipped, never dereferenced.
        template<bool Stamp, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeStorageEntity(Entity e, size_t i, ReqTuple& req, OptTuple& opt, Func& func,
                                                   [[maybe_unused]] Tick now,
                                                   std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>)
        {
            std::tuple<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>*...> rp{ std::get<ReqIs>(req).At(e, i)... };
            if ((... || (std::get<ReqIs>(rp) == nullptr))) ASTRA_UNLIKELY
                return;
            std::tuple<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>*...> op{ std::get<OptIs>(opt).OptionalAt(e, i)... };
            std::tuple<typename YieldType<std::tuple_element_t<ReqIs, RequiredTypes>>::type...> y{
                MakeStorageYield<ReqIs>(std::get<ReqIs>(req), std::get<ReqIs>(rp), e, i, now)... };
            std::apply([&](auto&... r)
            {
                func(e, r..., static_cast<std::tuple_element_t<OptIs, OptionalTypes>*>(std::get<OptIs>(op))...);
            }, y);
        }

        // The storage-path chunk visit (shared by the serial and parallel walks).
        template<bool Stamp, typename Func, size_t... ReqIs, size_t... OptIs>
        void VisitChunkStorage(Archetype* archetype, ArchetypeChunk* chunk, Func& func, Tick now, [[maybe_unused]] Tick since,
                               std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq)
        {
            const size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;
            const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();

            ChunkFilter f;
            if (!BuildChunkFilter(archetype, chunk, count, f))
                return;                                   // Tier 2: nothing in this chunk can match

            // Coarse stamp after every whole-chunk reject (Ruling B: a skipped chunk is
            // not "visited" and must not be stamped).
            if constexpr (Stamp)
                StampChunkStorage(chunk, cm, now, reqSeq, optSeq);

            std::tuple<Fetch<std::tuple_element_t<ReqIs, RequiredTypes>>...> req{
                MakeFetch<std::tuple_element_t<ReqIs, RequiredTypes>>(chunk, cm, m_stores)... };
            std::tuple<Fetch<std::tuple_element_t<OptIs, OptionalTypes>>...> opt{
                MakeFetch<std::tuple_element_t<OptIs, OptionalTypes>>(chunk, cm, m_stores)... };
            const Entity* entities = chunk->GetEntities().data();

            Detail::ForEachMatchingRun(f.exclude, f.nExclude, f.require, f.nRequire, count,
                [&](size_t begin, size_t end)
                {
                    for (size_t i = begin; i < end; ++i)
                        InvokeStorageEntity<Stamp>(entities[i], i, req, opt, func, now, reqSeq, optSeq);
                });
        }

        // Size() over one archetype on the storage path: popcount of the match words.
        size_t SizeStorage(Archetype* archetype) const
        {
            size_t total = 0;
            for (auto& chunkPtr : archetype->GetChunks())
            {
                ArchetypeChunk* chunk = chunkPtr.get();
                const size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                    continue;
                ChunkFilter f;
                if (!BuildChunkFilter(archetype, chunk, count, f))
                    continue;
                total += Detail::CountMatching(f.exclude, f.nExclude, f.require, f.nRequire, count);
            }
            return total;
        }
```

- [ ] **Step 5: Route the walks**

1. Rename the existing `VisitChunkFiltered` to `VisitChunkTable`. Keep its body and signature unchanged. Then add the dispatcher under the old name, directly above it:

```cpp
        // Per-chunk filtered visit. Storage-path views (a Sparse type, or a Universal
        // member in this registry) take VisitChunkStorage; every other view keeps the
        // pre-existing loop (VisitChunkTable), which a HasSparseArg view never instantiates.
        template<bool Stamp, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void VisitChunkFiltered(Archetype* archetype, ArchetypeChunk* chunk, Func&& func, Tick now, Tick since,
                                                  std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq)
        {
            if constexpr (HasSparseArg)
                VisitChunkStorage<Stamp>(archetype, chunk, func, now, since, reqSeq, optSeq);
            else if (m_anyUniversal) ASTRA_UNLIKELY
                VisitChunkStorage<Stamp>(archetype, chunk, func, now, since, reqSeq, optSeq);
            else
                VisitChunkTable<Stamp>(archetype, chunk, func, now, since, reqSeq, optSeq);
        }
```

2. In `ForEachImpl`, at the top of the `if constexpr (!HasChunkFilter)` branch (before `(void)since;`), add:

```cpp
                if (m_anyUniversal) ASTRA_UNLIKELY
                {
                    // A Table-trait argument declared Universal in this registry: no
                    // chunk column exists for it, so the archetype fast loops cannot run.
                    const Tick now = m_archetypeManager->CurrentTick();
                    for (auto& chunk : archetype->GetChunks())
                        VisitChunkStorage<Stamp>(archetype, chunk.get(), func, now, since,
                                                 std::make_index_sequence<sizeof...(Required)>{},
                                                 std::make_index_sequence<sizeof...(Optional)>{});
                    return;
                }
```

3. In `ForEachBody`, directly after `EnsureArchetypes();`, add:

```cpp
            if (UsesStoragePath())
                ResolveStores();     // before the Debug counter capture and before any chunk
```

4. In `Size()`, change `if constexpr (!HasRequiredFilter)` to `if constexpr (!HasRequiredFilter && !HasSparseArg)`. In its `else` branch, add `if (UsesStoragePath()) ResolveStores();` before the loop.
5. At the top of `SizeFiltered`'s body, add:

```cpp
            if (UsesStoragePath())
                return SizeStorage(archetype);
```

6. Add the data member after Task 22's plan members: `StoreArray m_stores{};   // resolved per walk (ResolveStores)`.

`ParallelForEach*` reaches `VisitChunkFiltered` only through `ParallelForEachChunkImpl`. Task 28 resolves the stores there and adds the sparse-driven partition. Until Task 28, a parallel walk below the parallel threshold falls back to `ForEachSince`, which resolves the stores itself.

- [ ] **Step 6: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewFilter.*:StorageViewPlan.*:View*:Enableable*:ChangeDetection*`
Expected: all PASS.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/View.hpp tests/Registry/StorageViewFilterTest.cpp
git commit -m "feat(storage): storage-path chunk walk -- required/With Sparse presence words with whole-chunk skip, Not<Sparse>, the disabled mirror for enableable Sparse/Universal (IncludeDisabled opts out), Optional, per-kind fetchers (Table base[i], Sparse dense, Universal page cursor); serial ForEach + Size (s6.3, s6.4)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 25: Write stamps and `Mut<T>` for Sparse/Universal yields; the iteration-safety pin (s6.4, s4.3, Review Focus 2)

Writes on the storage path mark the same sources `Registry::IsChanged` reads:
- **Sparse:** the element's ticks when tracked (Task 20); otherwise the per-component coarse version `ComponentStorage::CoarseVersion`, stamped once per visited chunk.
- **Universal:** the slot's ticks when tracked; otherwise the page version, through `UniversalColumn::MarkWritten` (Task 14).
- **`Mut<T>`** over a Sparse or Universal element marks the element's own ticks.

The Debug structural-mutation guard also compares `ArchetypeManager::GetSparseStructuralCounter()` (Task 16). A sparse add or remove inside `ForEach` is therefore reported like a Table structural change. The walk itself stays memory-safe:
- word pointers are copied before any callback;
- every Sparse element is re-fetched by entity, so a swap-and-pop never hands out a moved-in element;
- a required element that vanished mid-walk is skipped.

**Files:**
- Create: `include/Astra/Registry/StorageAccess.hpp`
- Modify: `include/Astra/Registry/View.hpp`
- Create: `tests/Registry/StorageViewMutTest.cpp`
- Modify: `tests/Registry/IterationSafetyTest.cpp` (append the Review Focus 2 pins)

**Interfaces:**
- Consumes: `SparseSet::Descriptor / Get / DataAt / TicksAt`, `ComponentDescriptor::isChangeTracked`, `ComponentStorage::StampCoarse` (Task 20), `UniversalColumn::Ticks / MarkWritten` (Tasks 8, 14), `ArchetypeManager::GetSparseStructuralCounter()` (Task 16), `Registry::IsChanged` for every kind (Tasks 14, 20).
- Produces: `EntityTicks* Detail::SparseTicksOf(SparseSet&, Entity) noexcept` (`Registry/StorageAccess.hpp`).

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/StorageViewMutTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <utility>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

// Mut<T> over a tracked Sparse element marks ONLY the entity written through it.
TEST(StorageViewMut, SparseMutMarksOnlyTheWrittenEntity)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), SpE{float(i)});
    }
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<Position, SpE>().ForEach([&](Astra::Entity e, Position&, Astra::Mut<SpE> s)
    {
        if (e == es[3]) s.Write().v = 99.0f;
    });
    EXPECT_TRUE(reg.IsChanged<SpE>(es[3], since));
    EXPECT_FALSE(reg.IsChanged<SpE>(es[4], since));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<SpE>(es[3])->v, 99.0f);
}

TEST(StorageViewMut, UniversalMutMarksOnlyTheWrittenEntity)
{
    auto reg = MakeRegistry<TrackedPos>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i) es.push_back(reg->CreateEntity<Position>());
    const Astra::Tick since = reg->CurrentTick();
    reg->AdvanceTick();
    reg->CreateView<Position, TrackedPos>().ForEach([&](Astra::Entity e, Position&, Astra::Mut<TrackedPos> t)
    {
        if (e == es[5]) t->x = 4.0f;
    });
    EXPECT_TRUE(reg->IsChanged<TrackedPos>(es[5], since));
    EXPECT_FALSE(reg->IsChanged<TrackedPos>(es[6], since));
}

// Plan deviation 5 holds for every kind: a PRESENT non-const tracked Optional marks.
TEST(StorageViewMut, PresentTrackedSparseOptionalMarks)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 6; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        if (i % 2 == 0) reg.AddComponent(es.back(), SpE{});
    }
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<Position, Astra::Optional<SpE>>().ForEach([](Astra::Entity, Position&, SpE*) {});
    for (int i = 0; i < 6; i += 2) EXPECT_TRUE(reg.IsChanged<SpE>(es[i], since)) << i;
}

// Untracked: a non-const yield stamps the coarse source IsChanged reads; a const one does not.
TEST(StorageViewMut, UntrackedYieldsStampOnlyWhenNonConst)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.AddComponent(e, PosS{});
    Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<const Position, const PosS>().ForEach([](Astra::Entity, const Position&, const PosS&) {});
    EXPECT_FALSE(reg.IsChanged<PosS>(e, since));
    reg.CreateView<const Position, PosS>().ForEach([](Astra::Entity, const Position&, PosS&) {});
    EXPECT_TRUE(reg.IsChanged<PosS>(e, since));

    auto ureg = MakeRegistry<Health>();
    Astra::Entity u = ureg->CreateEntity<Position>();
    since = ureg->CurrentTick();
    ureg->AdvanceTick();
    ureg->CreateView<const Position, const Health>().ForEach([](Astra::Entity, const Position&, const Health&) {});
    EXPECT_FALSE(ureg->IsChanged<Health>(u, since));
    ureg->CreateView<const Position, Health>().ForEach([](Astra::Entity, const Position&, Health&) {});
    EXPECT_TRUE(ureg->IsChanged<Health>(u, since));
}
```

Append to `tests/Registry/IterationSafetyTest.cpp`. Add `#include <string>`, `#include <utility>`, `#include <vector>`, `#include "../Support/DiagnosticsTestGuards.hpp"` and `#include "StorageQueryComponents.hpp"` to its include block:

```cpp
// ============ Review Focus 2 (storage policies, spec s4.3) ============
// A sparse add or remove during iteration of a view that requires that sparse
// component is refused exactly like a table structural change: the Debug
// structural-mutation guard reports it. The walk must never corrupt or read past the
// dense arrays: elements are fetched by entity, word pointers are copied per chunk, and
// a required element that vanished mid-walk is skipped.
namespace
{
    struct GuardCapture { int count = 0; std::string message; };

    Astra::AssertAction CaptureAndContinue(const Astra::AssertContext& ctx, void* user) noexcept
    {
        auto* c = static_cast<GuardCapture*>(user);
        ++c->count;
        if (ctx.message) c->message = ctx.message;
        return Astra::AssertAction::Continue;
    }

    using Astra::Test::Position;
    using Astra::Test::PosS;
}

TEST(IterationSafety, SparseRemoveDuringRequiringViewTripsTheGuardAndStaysSafe)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 200; ++i)
    {
        es.push_back(reg.CreateEntityWith(Position{float(i), 0.0f, 0.0f}));
        reg.AddComponent(es.back(), PosS{float(i), 0.0f, 0.0f});
    }

    GuardCapture cap;
    size_t visits = 0;
    {
        Astra::Testing::ScopedAssertHandler guard(&CaptureAndContinue, &cap);
        reg.CreateView<Position, PosS>().ForEach([&](Astra::Entity e, Position& p, PosS& s)
        {
            ++visits;
            EXPECT_FLOAT_EQ(s.x, p.x);                                   // never a moved-in element
            if (int(p.x) % 2 == 0) (void)reg.RemoveComponent<PosS>(e);   // swap-and-pop mid-walk
        });
    }
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
    EXPECT_GE(cap.count, 1);
    EXPECT_NE(cap.message.find("Structural mutation"), std::string::npos);
#endif
    EXPECT_LE(visits, 200u);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
    for (int i = 0; i < 200; ++i)
    {
        const PosS* s = std::as_const(reg).GetComponent<PosS>(es[i]);
        if (i % 2 == 0)
            EXPECT_EQ(s, nullptr) << i;
        else
        {
            ASSERT_NE(s, nullptr) << i;
            EXPECT_FLOAT_EQ(s->x, float(i));                             // dense values intact after the swaps
        }
    }
}

TEST(IterationSafety, SparseAddDuringRequiringViewTripsTheGuard)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> holders, lacking;
    for (int i = 0; i < 100; ++i)
    {
        holders.push_back(reg.CreateEntityWith(Position{float(i), 0.0f, 0.0f}));
        reg.AddComponent(holders.back(), PosS{float(i), 0.0f, 0.0f});
        lacking.push_back(reg.CreateEntityWith(Position{-1.0f, 0.0f, 0.0f}));
    }

    GuardCapture cap;
    size_t next = 0;
    {
        Astra::Testing::ScopedAssertHandler guard(&CaptureAndContinue, &cap);
        reg.CreateView<Position, PosS>().ForEach([&](Astra::Entity, Position&, PosS&)
        {
            if (next < lacking.size()) (void)reg.AddComponent(lacking[next++], PosS{7.0f, 0.0f, 0.0f});
        });
    }
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
    EXPECT_GE(cap.count, 1);
#endif
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
    for (size_t k = 0; k < next; ++k) EXPECT_TRUE(reg.HasComponent<PosS>(lacking[k]));
    for (int i = 0; i < 100; ++i) EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(holders[i])->x, float(i));
}

// The sparse-driven walk (no Table argument, Task 28) re-reads Size()/Entities() each
// step, so erasing the visited element never reads past the dense arrays.
TEST(IterationSafety, SparseRemoveDuringSparseOnlyViewTripsTheGuardAndStaysSafe)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 150; ++i)
    {
        es.push_back(reg.CreateEntity());
        reg.AddComponent(es.back(), PosS{float(i), 0.0f, 0.0f});
    }
    GuardCapture cap;
    size_t visits = 0;
    {
        Astra::Testing::ScopedAssertHandler guard(&CaptureAndContinue, &cap);
        reg.CreateView<PosS>().ForEach([&](Astra::Entity e, PosS&)
        {
            ++visits;
            (void)reg.RemoveComponent<PosS>(e);
        });
    }
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
    EXPECT_GE(cap.count, 1);
#endif
    EXPECT_LE(visits, 150u);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
}

// Value edits and DEFERRED structural changes never trip the guard.
TEST(IterationSafety, SparseValueEditsAndDeferredRemovesDoNotTripTheGuard)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 50; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), PosS{});
    }
    GuardCapture cap;
    Astra::CommandBuffer cmd(&reg);
    {
        Astra::Testing::ScopedAssertHandler guard(&CaptureAndContinue, &cap);
        reg.CreateView<Position, PosS>().ForEach([&](Astra::Entity e, Position&, PosS& s)
        {
            s.y += 1.0f;
            cmd.RemoveComponent<PosS>(e);
        });
    }
    EXPECT_EQ(cap.count, 0);
    ASSERT_TRUE(cmd.Execute().IsOk());
    EXPECT_EQ((reg.CreateView<Position, PosS>().Size()), 0u);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
}
```

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewMut.*:IterationSafety.Sparse*`
Expected failures:
- `SparseMutMarksOnlyTheWrittenEntity` and `UniversalMutMarksOnlyTheWrittenEntity`: `IsChanged` is false for the written entity, because Task 24's `TicksFor` marks a scratch slot.
- `PresentTrackedSparseOptionalMarks`: false.
- `UntrackedYieldsStampOnlyWhenNonConst`: the non-const halves are false.
- The three guard tests: `cap.count == 0`, because the guard does not read the sparse counter yet.
`SparseValueEditsAndDeferredRemovesDoNotTripTheGuard` already passes.

- [ ] **Step 3: Add `StorageAccess.hpp`**

Create `include/Astra/Registry/StorageAccess.hpp`:

```cpp
#pragma once

#include <cstddef>

#include "../Component/Component.hpp"
#include "../Core/Tick.hpp"
#include "../Entity/Entity.hpp"
#include "ComponentStorage.hpp"

namespace Astra::Detail
{
    /**
     * Per-element ticks of a tracked, non-empty Sparse element, else nullptr (spec
     * s3.3). The dense data is stride-packed at Descriptor().size (Task 10), so
     * (element - DataAt(0)) / size is the dense position TicksAt takes -- the same
     * derivation as Registry::SparseTicksOf (Task 20). If SparseSet gains a
     * DenseIndexOf accessor, use it here instead.
     */
    inline EntityTicks* SparseTicksOf(SparseSet& set, Entity e) noexcept
    {
        const ComponentDescriptor& d = set.Descriptor();
        if (!d.isChangeTracked || d.size == 0)
            return nullptr;
        void* element = set.Get(e);
        if (!element)
            return nullptr;
        const size_t dense = static_cast<size_t>(static_cast<std::byte*>(element) -
                                                 static_cast<std::byte*>(set.DataAt(0))) / d.size;
        return set.TicksAt(dense);
    }
}
```

In `View.hpp`, add `#include "StorageAccess.hpp"` beside `#include "ComponentStorage.hpp"`.

- [ ] **Step 4: Wire `Mut<T>` and the store stamps**

In `View.hpp`'s storage-path block:

1. Replace the whole body of `TicksFor` (and its comment) with:

```cpp
        // Tick slot for a Mut<T> yield: the chunk's tick column (Table), the slot's ticks
        // (Universal), or the element's ticks (Sparse) -- what Registry::IsChanged reads.
        template<typename T>
        ASTRA_FORCEINLINE EntityTicks* TicksFor(Fetch<T>& f, [[maybe_unused]] Entity e, size_t i) const noexcept
        {
            if (f.ticks)
                return f.ticks + i;
            if (f.universal)
                return f.universal->Ticks(e);
            if (f.sparse)
                return Detail::SparseTicksOf(*f.sparse, e);
            return nullptr;
        }
```

2. Add these helpers after `StampChunkStorage`:

```cpp
        // Coarse write stamp of a non-const UNTRACKED Sparse yield: the per-component
        // version Registry::IsChanged reads for untracked Sparse (Task 20), stamped once
        // per visited chunk -- the Table column stamp's twin.
        template<typename T>
        ASTRA_FORCEINLINE void StampSparseCoarse([[maybe_unused]] ComponentStorage* storage, [[maybe_unused]] Tick now) const
        {
            if constexpr (Detail::IsMutableYield<T> && !IsChangeTrackedV<std::remove_const_t<T>>)
            {
                if (storage && m_kinds[PlanIndex<T>] == StorageKind::Sparse)
                    storage->StampCoarse(TypeID<std::remove_const_t<T>>::Value(), now);
            }
        }

        // Per-entity write stamps (Stamp walks only). A non-const UNTRACKED Universal
        // yield bumps its page version, the per-entity twin of the column stamp. A tracked
        // one is handed out as Mut<T>, which marks on use.
        template<typename T, typename F>
        ASTRA_FORCEINLINE void StampRequiredStore([[maybe_unused]] F& f, [[maybe_unused]] Entity e, [[maybe_unused]] Tick now) const noexcept
        {
            if constexpr (Detail::IsMutableYield<T> && !IsChangeTrackedV<std::remove_const_t<T>>)
            {
                if (f.universal)
                    f.universal->MarkWritten(e, now);
            }
        }

        // A PRESENT non-const Optional: plan deviation 5 for a tracked one (it is yielded
        // as a raw pointer, so it marks unconditionally), for every kind; an untracked
        // Universal one bumps its page. Untracked Sparse: StampSparseCoarse; untracked
        // Table: the chunk column stamp.
        template<typename T, typename F>
        ASTRA_FORCEINLINE void StampOptionalStore([[maybe_unused]] F& f, [[maybe_unused]] const void* present,
                                                  [[maybe_unused]] Entity e, [[maybe_unused]] size_t i,
                                                  [[maybe_unused]] Tick now) const noexcept
        {
            if constexpr (Detail::IsMutableYield<T>)
            {
                if (!present)
                    return;
                if (f.universal)
                {
                    f.universal->MarkWritten(e, now);
                    return;
                }
                if constexpr (IsChangeTrackedV<std::remove_const_t<T>>)
                {
                    if (f.sparse)
                    {
                        if (EntityTicks* t = Detail::SparseTicksOf(*f.sparse, e))
                            t->changed = now;
                    }
                    else if (f.ticks)
                        f.ticks[i].changed = now;
                }
            }
        }
```

3. In `InvokeStorageEntity`, insert after the `op` tuple and before the `y` tuple:

```cpp
            if constexpr (Stamp)
            {
                (StampRequiredStore<std::tuple_element_t<ReqIs, RequiredTypes>>(std::get<ReqIs>(req), e, now), ...);
                (StampOptionalStore<std::tuple_element_t<OptIs, OptionalTypes>>(std::get<OptIs>(opt), std::get<OptIs>(op), e, i, now), ...);
            }
```

4. In `VisitChunkStorage`, replace the stamp block (`if constexpr (Stamp) StampChunkStorage(chunk, cm, now, reqSeq, optSeq);`) with:

```cpp
            if constexpr (Stamp)
            {
                StampChunkStorage(chunk, cm, now, reqSeq, optSeq);
                ComponentStorage* storage = m_archetypeManager->GetComponentStorage();
                (StampSparseCoarse<std::tuple_element_t<ReqIs, RequiredTypes>>(storage, now), ...);
                (StampSparseCoarse<std::tuple_element_t<OptIs, OptionalTypes>>(storage, now), ...);
            }
```

- [ ] **Step 5: Extend the Debug guard to sparse membership changes**

In `ForEachBody`:

1. Directly after the existing capture `const uint32_t debugStartStructuralChangeCounter = ...;` (inside `#ifdef ASTRA_BUILD_DEBUG`), add:

```cpp
            // Sparse adds/removes never touch the archetype set, so they have their own
            // counter (Task 16); a sparse change inside the walk is refused like a table one.
            const uint32_t debugStartSparseCounter = m_archetypeManager->GetSparseStructuralCounter();
```

2. Replace the closing `ASTRA_ASSERT(...)` with:

```cpp
            ASTRA_ASSERT(m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire) == debugStartStructuralChangeCounter &&
                         m_archetypeManager->GetSparseStructuralCounter() == debugStartSparseCounter,
                "Structural mutation (create/destroy entity, add/remove component) detected during View::ForEach. "
                "Defer structural changes into a CommandBuffer and call Execute() after the loop.");
```

- [ ] **Step 6: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewMut.*:IterationSafety.*:StorageViewFilter.*:ChangeDetection*:View*`
Expected: all PASS.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/StorageAccess.hpp include/Astra/Registry/View.hpp tests/Registry/StorageViewMutTest.cpp tests/Registry/IterationSafetyTest.cpp
git commit -m "feat(storage): Mut<T> and write stamps for Sparse/Universal yields (element/slot ticks; coarse version for untracked Sparse, page version for untracked Universal); the Debug guard also compares the sparse structural counter -- a sparse add/remove inside ForEach is refused like a table one, and the walk stays memory-safe (s6.4, s4.3, Review Focus 2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 26: `Any` / `OneOf` with Sparse and Universal members (s6.3)

An archetype first gets a storage-aware verdict per group:
- Table members are counted from its mask; Universal members count as held by every entity;
- Sparse members defer the decision to a per-entity test.

Per chunk, the deferred part becomes words:
- **Any:** the OR of the Sparse members' presence words, added to `require`.
- **OneOf, nothing held at archetype level:** "exactly one Sparse member" (`ones & ~twos`), added to `require`.
- **OneOf, one member already held:** every Sparse member's presence words, added to `exclude`.

The same verdict also exists per entity (`EntityGroupsPass`), for the sparse-driven walk (Task 28) and `Get`/`Contains` (Task 29).

**Files:**
- Modify: `include/Astra/Registry/View.hpp`
- Create: `tests/Registry/StorageViewModifierTest.cpp`

**Interfaces:**
- Consumes: the plan (Task 22), `ChunkFilter` / `BuildChunkFilter` / `ExcludePresent` / `StoreArray` (Task 24).
- Produces (private): `GroupVerdict`, `GroupAtArchetype<G>`, the storage-aware `GroupsPass`, `BuildAnyGroupWords<G>`, `BuildOneOfGroupWords<G>`, `EntityHolds<T>(const EntityRecord*, Entity, const StoreArray&)`, `EntityGroupsPass(const EntityRecord*, Entity, const StoreArray&)`.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageViewModifierTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <random>
#include <set>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    template<typename V>
    std::set<Astra::Entity> Visited(V&& view)
    {
        std::set<Astra::Entity> s;
        view.ForEach([&](Astra::Entity e, auto&&...) { s.insert(e); });
        return s;
    }

    struct Fixture
    {
        Astra::Registry reg;
        Astra::Entity A, B, C, D, E, F;
        Fixture()
        {
            A = reg.CreateEntity<Position>();                 // nothing
            B = reg.CreateEntity<Position>();                 reg.AddComponent(B, PosS{});
            C = reg.CreateEntity<Position>();                 reg.AddComponent(C, SpB{});
            D = reg.CreateEntity<Position, Velocity>();       // Table member only
            E = reg.CreateEntity<Position>();                 reg.AddComponent(E, PosS{}); reg.AddComponent(E, SpB{});
            F = reg.CreateEntity<Position, Velocity>();       reg.AddComponent(F, PosS{});
        }
    };
}

TEST(StorageViewModifier, AnyWithSparseMembers)
{
    Fixture fx;
    using S = std::set<Astra::Entity>;
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::Any<PosS, SpB>>()), (S{fx.B, fx.C, fx.E, fx.F}));
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::Any<Velocity, PosS>>()), (S{fx.B, fx.D, fx.E, fx.F}));
    EXPECT_EQ((fx.reg.CreateView<Position, Astra::Any<PosS, SpB>>().Size()), 4u);
    EXPECT_EQ((fx.reg.CreateView<Astra::Any<SpB, SpTag>>().Size()), 2u);
}

TEST(StorageViewModifier, OneOfWithSparseMembers)
{
    Fixture fx;
    using S = std::set<Astra::Entity>;
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::OneOf<PosS, SpB>>()), (S{fx.B, fx.C, fx.F}));
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::OneOf<Velocity, PosS>>()), (S{fx.B, fx.D, fx.E}));
    EXPECT_EQ((fx.reg.CreateView<Position, Astra::OneOf<Velocity, PosS>>().Size()), 3u);
}

// Universal members are held by every entity: Any is always satisfied, OneOf requires
// every other member to be absent.
TEST(StorageViewModifier, UniversalGroupMembers)
{
    auto reg = MakeRegistry<Health>();
    Astra::Entity plain = reg->CreateEntity<Position>();
    Astra::Entity withS = reg->CreateEntity<Position>();
    reg->AddComponent(withS, PosS{});
    Astra::Entity withV = reg->CreateEntity<Position, Velocity>();
    using S = std::set<Astra::Entity>;
    EXPECT_EQ(Visited(reg->CreateView<Position, Astra::Any<Health, PosS>>()), (S{plain, withS, withV}));
    EXPECT_EQ(Visited(reg->CreateView<Position, Astra::OneOf<Health, PosS>>()), (S{plain, withV}));
    EXPECT_EQ(Visited(reg->CreateView<Position, Astra::OneOf<Health, Velocity>>()), (S{plain, withS}));
}

// Word-level correctness over many chunks against a brute-force HasComponent model.
TEST(StorageViewModifier, GroupsAgreeWithHasComponentAcrossChunks)
{
    Astra::Registry reg;
    std::mt19937 rng(0x5EED);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 3000; ++i)
    {
        Astra::Entity e = (rng() % 3 == 0) ? reg.CreateEntity<Position, Velocity>() : reg.CreateEntity<Position>();
        if (rng() % 2) reg.AddComponent(e, PosS{});
        if (rng() % 3 == 0) reg.AddComponent(e, SpB{});
        es.push_back(e);
    }
    std::set<Astra::Entity> any, one, anyV;
    for (Astra::Entity e : es)
    {
        const int s = int(reg.HasComponent<PosS>(e)) + int(reg.HasComponent<SpB>(e));
        if (s > 0) any.insert(e);
        if (s == 1) one.insert(e);
        if (reg.HasComponent<Velocity>(e) || reg.HasComponent<SpB>(e)) anyV.insert(e);
    }
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::Any<PosS, SpB>>()), any);
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::OneOf<PosS, SpB>>()), one);
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::Any<Velocity, SpB>>()), anyV);
    EXPECT_EQ((reg.CreateView<Position, Astra::OneOf<PosS, SpB>>().Size()), one.size());
}
```

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewModifier.*`
Expected: FAIL. Task 22's `GroupsPass` tests only archetype masks, so a group whose only present members are Sparse rejects every archetype. For example, `Any<PosS, SpB>` visits nothing, and the Universal groups are wrong.

- [ ] **Step 3: Make the archetype-level group verdict storage-aware**

In `View.hpp`, replace Task 22's `GroupsPass` (the whole function, with its comment) with:

```cpp
        // Archetype-level verdict of one Any/OneOf group (spec s6.3): how many members the
        // archetype is KNOWN to hold -- Table members in its mask, plus Universal members,
        // which every live entity holds -- and whether Sparse members leave the rest to a
        // per-entity test. For a pure-Table group this is exactly QueryBuilder's mask test.
        struct GroupVerdict { uint32_t known = 0; bool hasSparse = false; };

        template<typename T>
        void CountGroupMember(const ComponentMask& mask, GroupVerdict& v) const
        {
            switch (m_kinds[PlanIndex<T>])
            {
            case StorageKind::Table:     v.known += mask.Test(TypeID<std::remove_const_t<T>>::Value()) ? 1u : 0u; break;
            case StorageKind::Universal: ++v.known; break;
            case StorageKind::Sparse:    v.hasSparse = true; break;
            }
        }

        template<typename Group>
        GroupVerdict GroupAtArchetype([[maybe_unused]] const ComponentMask& mask) const
        {
            GroupVerdict v;
            [&]<typename... Ts>(std::tuple<Ts...>*) { (CountGroupMember<Ts>(mask, v), ...); }(static_cast<Group*>(nullptr));
            return v;
        }

        // Any: a member is known, or a Sparse member may supply it per entity.
        template<typename G>
        bool AnyGroupPasses(const ComponentMask& mask) const
        {
            const GroupVerdict v = GroupAtArchetype<G>(mask);
            return v.known > 0 || v.hasSparse;
        }
        // OneOf: exactly one known, or none known and a Sparse member may be the one.
        template<typename G>
        bool OneOfGroupPasses(const ComponentMask& mask) const
        {
            const GroupVerdict v = GroupAtArchetype<G>(mask);
            return v.known == 1 || (v.known == 0 && v.hasSparse);
        }
        template<typename... Gs>
        bool AnyGroupsPass([[maybe_unused]] const ComponentMask& mask, std::tuple<Gs...>*) const { return (AnyGroupPasses<Gs>(mask) && ...); }
        template<typename... Gs>
        bool OneOfGroupsPass([[maybe_unused]] const ComponentMask& mask, std::tuple<Gs...>*) const { return (OneOfGroupPasses<Gs>(mask) && ...); }

        ASTRA_NODISCARD bool GroupsPass(const ComponentMask& mask) const
        {
            return AnyGroupsPass(mask, static_cast<AnyGroupTypes*>(nullptr))
                && OneOfGroupsPass(mask, static_cast<OneOfGroupTypes*>(nullptr));
        }
```

- [ ] **Step 4: Add the per-chunk group words**

1. In `ChunkFilter`, add after `nRequire`:

```cpp
            // Owned words for Any/OneOf groups whose verdict is per entity (Task 26).
            struct GroupScratch
            {
                std::array<SmallVector<uint64_t, 16>, NGroups == 0 ? 1 : NGroups> words;
                SmallVector<uint64_t, 16> twos;    // OneOf "seen twice" scratch
            };
            struct NoGroupScratch {};
            [[no_unique_address]] std::conditional_t<(NGroups > 0), GroupScratch, NoGroupScratch> groups;
```

2. Add these members after `ExcludePresent`:

```cpp
        // OR of a group's Sparse members' presence words into `out`.
        template<typename T>
        void OrSparsePresence(ArchetypeChunk* chunk, size_t nw, uint64_t* out, bool& any) const
        {
            if (m_kinds[PlanIndex<T>] != StorageKind::Sparse)
                return;
            const StoreRef& s = m_stores[PlanIndex<T>];
            if (s.slot < 0)
                return;
            const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(s.slot));
            if (!b || b->presentCount == 0)
                return;
            for (size_t w = 0; w < nw; ++w)
                out[w] |= b->present[w];
            any = true;
        }

        // "Exactly one" accumulation over a group's Sparse members: twos |= ones & m; ones |= m.
        template<typename T>
        void CountSparsePresence(ArchetypeChunk* chunk, size_t nw, uint64_t* ones, uint64_t* twos, bool& any) const
        {
            if (m_kinds[PlanIndex<T>] != StorageKind::Sparse)
                return;
            const StoreRef& s = m_stores[PlanIndex<T>];
            if (s.slot < 0)
                return;
            const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(s.slot));
            if (!b || b->presentCount == 0)
                return;
            for (size_t w = 0; w < nw; ++w)
            {
                const uint64_t m = b->present[w];
                twos[w] |= ones[w] & m;
                ones[w] |= m;
            }
            any = true;
        }

        // Any group: nothing to add when a Table/Universal member is known here; else the
        // OR of the Sparse members' presence (false when no member has a block here).
        template<typename Group>
        bool BuildAnyGroupWords(ArchetypeChunk* chunk, const ComponentMask& mask, size_t count, ChunkFilter& f, size_t g) const
        {
            if (GroupAtArchetype<Group>(mask).known > 0)
                return true;
            auto& words = f.groups.words[g];
            const size_t nw = (count + 63) >> 6;
            words.assign(nw, 0ull);
            bool any = false;
            [&]<typename... Ts>(std::tuple<Ts...>*) { (OrSparsePresence<Ts>(chunk, nw, words.data(), any), ...); }(static_cast<Group*>(nullptr));
            if (!any)
                return false;
            f.require[f.nRequire++] = words.data();
            return true;
        }

        // OneOf group: one member known here => every Sparse member must be absent
        // (exclude their presence); none known => exactly one Sparse member per entity.
        template<typename Group>
        bool BuildOneOfGroupWords(ArchetypeChunk* chunk, const ComponentMask& mask, size_t count, ChunkFilter& f, size_t g) const
        {
            const GroupVerdict v = GroupAtArchetype<Group>(mask);
            if (!v.hasSparse)
                return v.known == 1;
            if (v.known > 1)
                return false;
            if (v.known == 1)
            {
                bool pass = true;
                [&]<typename... Ts>(std::tuple<Ts...>*) { ((pass = pass && ExcludePresent<std::remove_const_t<Ts>>(chunk, count, f)), ...); }(static_cast<Group*>(nullptr));
                return pass;
            }
            const size_t nw = (count + 63) >> 6;
            auto& ones = f.groups.words[g];
            ones.assign(nw, 0ull);
            f.groups.twos.assign(nw, 0ull);
            bool any = false;
            [&]<typename... Ts>(std::tuple<Ts...>*) { (CountSparsePresence<Ts>(chunk, nw, ones.data(), f.groups.twos.data(), any), ...); }(static_cast<Group*>(nullptr));
            if (!any)
                return false;
            for (size_t w = 0; w < nw; ++w)
                ones[w] &= ~f.groups.twos[w];
            f.require[f.nRequire++] = ones.data();
            return true;
        }
```

3. In `BuildChunkFilter`, insert before `return pass;`:

```cpp
            if constexpr (NGroups > 0)
            {
                if (!pass)
                    return false;
                const ComponentMask& mask = archetype->GetMask();
                [&]<typename... Gs>(std::tuple<Gs...>*)
                {
                    size_t g = 0;   // a failed group short-circuits the rest, so the index never matters after it
                    ((pass = pass && BuildAnyGroupWords<Gs>(chunk, mask, count, f, g++)), ...);
                }(static_cast<AnyGroupTypes*>(nullptr));
                [&]<typename... Gs>(std::tuple<Gs...>*)
                {
                    size_t g = std::tuple_size_v<AnyGroupTypes>;
                    ((pass = pass && BuildOneOfGroupWords<Gs>(chunk, mask, count, f, g++)), ...);
                }(static_cast<OneOfGroupTypes*>(nullptr));
            }
```

- [ ] **Step 5: Add the per-entity group verdict (used by Tasks 28-29)**

Add after `BuildOneOfGroupWords`:

```cpp
        // Per-entity membership of one named type, any kind: the archetype mask (Table),
        // always (Universal: a live entity holds every universal), the set (Sparse).
        template<typename T>
        ASTRA_NODISCARD bool EntityHolds(const EntityRecord* rec, Entity e, const StoreArray& st) const
        {
            switch (m_kinds[PlanIndex<T>])
            {
            case StorageKind::Table:     return rec->archetype->GetMask().Test(TypeID<std::remove_const_t<T>>::Value());
            case StorageKind::Universal: return true;
            case StorageKind::Sparse:
            {
                const StoreRef& s = st[PlanIndex<T>];
                return s.sparse && s.sparse->Contains(e);
            }
            }
            return false;
        }

        template<typename G>
        ASTRA_NODISCARD uint32_t EntityGroupCount(const EntityRecord* rec, Entity e, const StoreArray& st) const
        {
            uint32_t n = 0;
            [&]<typename... Ts>(std::tuple<Ts...>*) { ((n += EntityHolds<Ts>(rec, e, st) ? 1u : 0u), ...); }(static_cast<G*>(nullptr));
            return n;
        }
        template<typename... Gs>
        ASTRA_NODISCARD bool EntityAnyGroups([[maybe_unused]] const EntityRecord* rec, [[maybe_unused]] Entity e,
                                             [[maybe_unused]] const StoreArray& st, std::tuple<Gs...>*) const
        {
            return ((EntityGroupCount<Gs>(rec, e, st) > 0) && ...);
        }
        template<typename... Gs>
        ASTRA_NODISCARD bool EntityOneOfGroups([[maybe_unused]] const EntityRecord* rec, [[maybe_unused]] Entity e,
                                               [[maybe_unused]] const StoreArray& st, std::tuple<Gs...>*) const
        {
            return ((EntityGroupCount<Gs>(rec, e, st) == 1) && ...);
        }

        // Every Any/OneOf group, decided for one entity.
        ASTRA_NODISCARD bool EntityGroupsPass(const EntityRecord* rec, Entity e, const StoreArray& st) const
        {
            return EntityAnyGroups(rec, e, st, static_cast<AnyGroupTypes*>(nullptr))
                && EntityOneOfGroups(rec, e, st, static_cast<OneOfGroupTypes*>(nullptr));
        }
```

- [ ] **Step 6: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewModifier.*:StorageViewFilter.*:StorageViewPlan.*:View*:Query*`
Expected: all PASS. This includes `StorageViewPlan.PureTableViewMatchingIsUnchanged`, whose Any/OneOf now go through the storage-aware verdict.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/View.hpp tests/Registry/StorageViewModifierTest.cpp
git commit -m "feat(storage): Any/OneOf with Sparse and Universal members -- storage-aware archetype verdict (Universal held by all), per-chunk group words (Any: OR of presence; OneOf: exactly-one or exclude-all), per-entity verdict for the sparse-driven and random-access paths (s6.3)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 27: `Changed<T>` / `Added<T>` for Sparse and Universal (s6.3)

The storage path keeps the Table change semantics: a chunk-level reject first, then a per-entity tier appended as one more `exclude` word set. Each term reads the source `Registry::IsChanged` / `IsAdded` reads for its kind:

| Term kind | Chunk reject | Per entity |
|---|---|---|
| Table, untracked | column version | none |
| Table, tracked | column version | the chunk's tick column |
| Sparse, untracked | `ComponentStorage::CoarseVersion(id)` (per component, so it can reject the whole walk) | none |
| Sparse, tracked | none | the element's ticks (`Detail::SparseTicksOf`) |
| Universal | none | `UniversalColumn::ChangedTick` / `AddedTick` (tracked: exact; untracked: page version) |

**Files:**
- Modify: `include/Astra/Registry/View.hpp`
- Create: `tests/Registry/StorageViewChangeTest.cpp`

**Interfaces:**
- Consumes: `ComponentStorage::CoarseVersion` (Task 20), `UniversalColumn::AddedTick / ChangedTick` (Task 8), `Detail::SparseTicksOf` (Task 25), `Registry::Modified<T>` for every kind (Tasks 14, 20).
- Produces (private): `ChangeChunkPassesStorage`, `ExcludeNotNewerStorage<UseAdded, T>`, `BuildChangeExcludedStorage<UseAdded>`, `ChunkFilter::changeExcluded`.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageViewChangeTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <set>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    using S = std::set<Astra::Entity>;
    struct FakeCtx { Astra::Tick last; Astra::Tick LastRun() const noexcept { return last; } };
}

TEST(StorageViewChange, TrackedSparseChangedAndAddedArePerEntity)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 6; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), SpE{float(i)});
    }
    const Astra::Tick t0 = reg.CurrentTick();
    reg.AdvanceTick();
    ASSERT_TRUE(reg.Modified<SpE>(es[1]));
    Astra::Entity late = reg.CreateEntity<Position>();
    reg.AddComponent(late, SpE{});

    S changed, added, changedWithTable;
    reg.CreateView<const SpE, Astra::Changed<SpE>>().Since(t0).ForEach([&](Astra::Entity e, const SpE&) { changed.insert(e); });
    reg.CreateView<const SpE, Astra::Added<SpE>>().ForEach(FakeCtx{t0}, [&](Astra::Entity e, const SpE&) { added.insert(e); });
    reg.CreateView<const Position, const SpE, Astra::Changed<SpE>>().Since(t0).ForEach(
        [&](Astra::Entity e, const Position&, const SpE&) { changedWithTable.insert(e); });
    EXPECT_EQ(changed, (S{es[1], late}));
    EXPECT_EQ(added, (S{late}));
    EXPECT_EQ(changedWithTable, (S{es[1], late}));

    // A disabled member is still filtered out under Changed<T> (enabled filter first).
    ASSERT_TRUE(reg.SetEnabled<SpE>(es[1], false));
    S again;
    reg.CreateView<const SpE, Astra::Changed<SpE>>().Since(t0).ForEach([&](Astra::Entity e, const SpE&) { again.insert(e); });
    EXPECT_EQ(again, (S{late}));
}

// Untracked Sparse: the per-component coarse version -- all members or none.
TEST(StorageViewChange, UntrackedSparseIsCoarse)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 4; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), PosS{});
    }
    const Astra::Tick t0 = reg.CurrentTick();
    reg.AdvanceTick();
    size_t n = 0;
    reg.CreateView<const Position, const PosS, Astra::Changed<PosS>>().Since(t0).ForEach([&](Astra::Entity, const Position&, const PosS&) { ++n; });
    EXPECT_EQ(n, 0u);
    ASSERT_TRUE(reg.Modified<PosS>(es[2]));
    reg.CreateView<const Position, const PosS, Astra::Changed<PosS>>().Since(t0).ForEach([&](Astra::Entity, const Position&, const PosS&) { ++n; });
    EXPECT_EQ(n, 4u);
}

TEST(StorageViewChange, TrackedUniversalIsPerEntity)
{
    auto reg = MakeRegistry<TrackedPos>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 5; ++i) es.push_back(reg->CreateEntity<Position>());
    const Astra::Tick t0 = reg->CurrentTick();
    reg->AdvanceTick();
    ASSERT_TRUE(reg->Modified<TrackedPos>(es[0]));
    Astra::Entity late = reg->CreateEntity<Position>();

    S changed, added;
    reg->CreateView<const Position, const TrackedPos, Astra::Changed<TrackedPos>>().Since(t0).ForEach(
        [&](Astra::Entity e, const Position&, const TrackedPos&) { changed.insert(e); });
    reg->CreateView<const TrackedPos, Astra::Added<TrackedPos>>().Since(t0).ForEach(
        [&](Astra::Entity e, const TrackedPos&) { added.insert(e); });
    EXPECT_EQ(changed, (S{es[0], late}));
    EXPECT_EQ(added, (S{late}));
}

// Untracked Universal: page-coarse -- every live slot of a written page answers.
TEST(StorageViewChange, UntrackedUniversalIsPageCoarse)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 3; ++i) es.push_back(reg->CreateEntity<Position>());
    const Astra::Tick t0 = reg->CurrentTick();
    reg->AdvanceTick();
    size_t n = 0;
    auto count = [&] {
        n = 0;
        reg->CreateView<const Health, Astra::Changed<Health>>().Since(t0).ForEach([&](Astra::Entity, const Health&) { ++n; });
        return n;
    };
    EXPECT_EQ(count(), 0u);
    ASSERT_TRUE(reg->Modified<Health>(es[0]));
    EXPECT_EQ(count(), 3u);    // ids 0..2 share one 1024-slot page
}
```

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewChange.*`
Expected: FAIL. `VisitChunkStorage` ignores `since`, so every member is visited: `changed`/`added` contain every holder, and the untracked counts are 4 and 3 before any write.

- [ ] **Step 3: Add the change tiers to the storage path**

In `View.hpp`:

1. In `ChunkFilter`, add after the `groups` member:

```cpp
            // Per-entity change tier (Task 27): SET bit == the entity fails a Changed/Added term.
            struct NoChangeScratch {};
            [[no_unique_address]] std::conditional_t<HasChangeFilter, SmallVector<uint64_t, 64>, NoChangeScratch> changeExcluded;
```

2. Add these members to the storage-path block, after `BuildChunkFilter`:

```cpp
        // Chunk tier of one Changed/Added term (spec s6.3): a Table term rejects on its
        // column version, an untracked Sparse term on the per-component coarse version
        // (Task 20). Tracked Sparse and every Universal term are decided per entity.
        template<typename T>
        bool ChangeTermChunkPasses(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick since) const
        {
            using Bare = std::remove_const_t<T>;
            switch (m_kinds[PlanIndex<Bare>])
            {
            case StorageKind::Table:
                return IsNewer(chunk->GetColumnVersion(cm.idToColumn[TypeID<Bare>::Value()]), since);
            case StorageKind::Sparse:
                if constexpr (IsChangeTrackedV<Bare>)
                    return true;
                else
                {
                    ComponentStorage* storage = m_archetypeManager->GetComponentStorage();
                    return storage && IsNewer(storage->CoarseVersion(TypeID<Bare>::Value()), since);
                }
            case StorageKind::Universal:
                return true;
            }
            return true;
        }
        template<typename... Ts>
        bool AllChangeTermsPass([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                [[maybe_unused]] Tick since, std::tuple<Ts...>*) const
        {
            return (ChangeTermChunkPasses<Ts>(chunk, cm, since) && ...);
        }
        bool ChangeChunkPassesStorage(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick since) const
        {
            return AllChangeTermsPass(chunk, cm, since, static_cast<ChangedTypes*>(nullptr))
                && AllChangeTermsPass(chunk, cm, since, static_cast<AddedTypes*>(nullptr));
        }

        // Per-entity tier of one term: set the bit of every entity whose tick is NOT newer
        // than `since` (an absent element fails; the presence words exclude it anyway).
        template<bool UseAdded, typename T>
        void ExcludeNotNewerStorage([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                    [[maybe_unused]] size_t count, [[maybe_unused]] Tick since,
                                    [[maybe_unused]] uint64_t* excluded, [[maybe_unused]] bool& anyExcluded) const
        {
            using Bare = std::remove_const_t<T>;
            const Entity* es = chunk->GetEntities().data();
            switch (m_kinds[PlanIndex<Bare>])
            {
            case StorageKind::Table:
                if constexpr (IsChangeTrackedV<Bare>)
                    ExcludeNotNewer<UseAdded>(chunk->GetTicks(cm.idToColumn[TypeID<Bare>::Value()]), count, since, excluded, anyExcluded);
                break;
            case StorageKind::Sparse:
                if constexpr (IsChangeTrackedV<Bare>)
                {
                    SparseSet* set = m_stores[PlanIndex<Bare>].sparse;
                    for (size_t i = 0; i < count; ++i)
                    {
                        const EntityTicks* t = set ? Detail::SparseTicksOf(*set, es[i]) : nullptr;
                        const bool fails = !t || !IsNewer(UseAdded ? t->added : t->changed, since);
                        excluded[i >> 6] |= static_cast<uint64_t>(fails) << (i & 63);
                        anyExcluded |= fails;
                    }
                }
                break;
            case StorageKind::Universal:
            {
                UniversalColumn* col = m_stores[PlanIndex<Bare>].universal;
                for (size_t i = 0; i < count; ++i)
                {
                    const Tick t = col ? (UseAdded ? col->AddedTick(es[i]) : col->ChangedTick(es[i])) : Tick{0};
                    const bool fails = !IsNewer(t, since);
                    excluded[i >> 6] |= static_cast<uint64_t>(fails) << (i & 63);
                    anyExcluded |= fails;
                }
                break;
            }
            }
        }
        template<bool UseAdded, typename... Ts>
        void BuildChangeExcludedStorage([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                        [[maybe_unused]] size_t count, [[maybe_unused]] Tick since,
                                        [[maybe_unused]] uint64_t* excluded, [[maybe_unused]] bool& anyExcluded, std::tuple<Ts...>*) const
        {
            (ExcludeNotNewerStorage<UseAdded, Ts>(chunk, cm, count, since, excluded, anyExcluded), ...);
        }
```

3. In `VisitChunkStorage`:
   - Insert after `const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();`:

```cpp
            if constexpr (HasChangeFilter)
            {
                // Chunk reject first (spec s2.2 of the change-detection design): before the
                // enabled reject and before the stamp -- a rejected chunk is not "visited".
                if (!ChangeChunkPassesStorage(chunk, cm, since))
                    return;
            }
```

   - Insert after the `if (!BuildChunkFilter(archetype, chunk, count, f)) return;` statement:

```cpp
            if constexpr (HasChangeFilter)
            {
                // Per-entity change tier, unioned into the exclude sets (not a reject: the
                // chunk is still visited, Ruling B).
                f.changeExcluded.assign((count + 63) >> 6, 0ull);
                bool anyExcluded = false;
                BuildChangeExcludedStorage<false>(chunk, cm, count, since, f.changeExcluded.data(), anyExcluded, static_cast<ChangedTypes*>(nullptr));
                BuildChangeExcludedStorage<true >(chunk, cm, count, since, f.changeExcluded.data(), anyExcluded, static_cast<AddedTypes*>(nullptr));
                if (anyExcluded)
                    f.exclude[f.nExclude++] = f.changeExcluded.data();
            }
```

`MaxWordSets` already reserves the extra exclude slot (`+ 2`).

- [ ] **Step 4: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewChange.*:ChangeDetection*:StorageView*`
Expected: all PASS.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Registry/View.hpp tests/Registry/StorageViewChangeTest.cpp
git commit -m "feat(storage): Changed<T>/Added<T> over Sparse and Universal -- chunk tier (column version; per-component coarse version for untracked Sparse) then a per-entity excluded word set from element/slot ticks or the universal page version, the same sources Registry::IsChanged reads (s6.3)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 28: The sparse-driven driver and the parallel paths (s6.2, s6.5)

A view with a required Sparse argument and no required Table-kind argument walks the dense array of its smallest required sparse set. `With<>`, `Changed<>` and `Added<>` count as required.
- Each candidate is checked per entity:
  - the record's archetype against the plan's masks;
  - every other constraint through `EntityPasses`: Sparse presence and `Not`, the enabled filters, Any/OneOf, and change terms.
- Visit order is the dense order, which is deterministic for a given operation sequence.
- The walk is index-based and re-reads `Size()` and `Entities()` every step (s4.3).
- If a required Sparse type has no member at all, nothing matches.

Parallel walks resolve the stores on the calling thread before dispatch:
- archetype-driven work items stay `(Archetype*, chunkIndex)`, and a Universal member routes `ParallelForEachChunkImpl` to `VisitChunkStorage`;
- sparse-driven walks partition the dense range into fixed 1024-entry work items. `ParallelForEachWithContext` stamps the flat item index into each sub-context.

**Files:**
- Modify: `include/Astra/Registry/View.hpp`
- Create: `tests/Registry/StorageViewDriverTest.cpp`

**Interfaces:**
- Consumes: `EntityHolds` / `EntityGroupsPass` (Task 26), `ExcludeNotNewer` (pre-existing), `Detail::SparseTicksOf` (Task 25), `ComponentStorage::CoarseVersion`, `UniversalColumn::AddedTick / ChangedTick`, `IWorkScheduler::ParallelFor`.
- Produces (private): `EntityPasses(const EntityRecord*, Entity, const StoreArray&, Tick) const`, `SparseDriver`, `PickSparseDriver()`, `VisitEntityStorage<Stamp>`, `VisitSparseRange<Stamp>`, `ParallelSparseDriven`, `ParallelSparseDrivenWithContext`, `SPARSE_WORK_ITEM = 1024`.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageViewDriverTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <atomic>
#include <mutex>
#include <set>
#include <span>
#include <vector>
#include "../Support/TestWorkerPool.hpp"
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    using S = std::set<Astra::Entity>;

    template<typename V>
    S Visited(V&& view)
    {
        S s;
        view.ForEach([&](Astra::Entity e, auto&&...) { s.insert(e); });
        return s;
    }

    // 5000 entities over three archetypes; PosS on every 7th (creation order), SpB on every 11th.
    struct World
    {
        Astra::Registry reg;
        std::vector<Astra::Entity> all, posS, spB;
        explicit World(const Astra::Registry::Config& cfg = {}) : reg(cfg)
        {
            for (int i = 0; i < 5000; ++i)
            {
                Astra::Entity e = (i % 3 == 0) ? reg.CreateEntity<Position>()
                                : (i % 3 == 1) ? reg.CreateEntity<Velocity>()
                                               : reg.CreateEntity();
                all.push_back(e);
                if (i % 7 == 0)  { reg.AddComponent(e, PosS{float(i), 0.0f, 0.0f}); posS.push_back(e); }
                if (i % 11 == 0) { reg.AddComponent(e, SpB{i}); spB.push_back(e); }
            }
        }
    };
}

// s6.2: no Table argument => the sparse set drives; order is its dense order.
TEST(StorageViewDriver, SparseDrivenVisitsTheSetInDenseOrder)
{
    World w;
    std::vector<Astra::Entity> order;
    w.reg.CreateView<PosS>().ForEach([&](Astra::Entity e, PosS& s)
    {
        order.push_back(e);
        EXPECT_FLOAT_EQ(s.x, float(7 * (order.size() - 1)));   // the k-th holder was created as entity 7k
    });
    EXPECT_EQ(order, w.posS);                          // no erase yet: dense order == insertion order
    EXPECT_EQ(w.reg.CreateView<const PosS>().Size(), w.posS.size());
}

// The smallest required set drives; the others are checked per entity.
TEST(StorageViewDriver, SmallestRequiredSetDrives)
{
    World w;
    std::vector<Astra::Entity> order, expected;
    for (Astra::Entity e : w.spB)
        if (w.reg.HasComponent<PosS>(e)) expected.push_back(e);
    w.reg.CreateView<const PosS, const SpB>().ForEach([&](Astra::Entity e, const PosS&, const SpB&) { order.push_back(e); });
    EXPECT_EQ(order, expected);                        // SpB (fewer members) drove: its dense order
}

TEST(StorageViewDriver, TableModifiersAndFiltersOnTheSparseDrivenPath)
{
    World w;
    S notVel, optPos, any;
    for (Astra::Entity e : w.posS)
    {
        if (!w.reg.HasComponent<Velocity>(e)) notVel.insert(e);
        if (w.reg.HasComponent<Velocity>(e) || w.reg.HasComponent<SpB>(e)) any.insert(e);
    }
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::Not<Velocity>>()), notVel);
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::Any<Velocity, SpB>>()), any);

    size_t withPos = 0, visits = 0;
    w.reg.CreateView<PosS, Astra::Optional<Position>>().ForEach([&](Astra::Entity e, PosS&, Position* p)
    {
        ++visits;
        EXPECT_EQ(p != nullptr, w.reg.HasComponent<Position>(e));
        withPos += (p != nullptr);
    });
    EXPECT_EQ(visits, w.posS.size());
    EXPECT_GT(withPos, 0u);

    for (size_t k = 0; k < w.posS.size(); k += 2) w.reg.AddComponent(w.posS[k], SpTag{});
    EXPECT_EQ((w.reg.CreateView<PosS, Astra::With<SpTag>>().Size()), (w.posS.size() + 1) / 2);

    for (size_t k = 0; k < 40; ++k) w.reg.AddComponent(w.all[k], SpE{});
    for (size_t k = 0; k < 40; k += 4) ASSERT_TRUE(w.reg.SetEnabled<SpE>(w.all[k], false));
    EXPECT_EQ(Visited(w.reg.CreateView<const SpE>()).size(), 30u);
    EXPECT_EQ(Visited(w.reg.CreateView<Astra::IncludeDisabled<SpE>>()).size(), 40u);
}

TEST(StorageViewDriver, AnEmptyRequiredSetMatchesNothing)
{
    World w;
    size_t n = 0;
    w.reg.CreateView<PosS, SpTag>().ForEach([&](Astra::Entity, PosS&, SpTag&) { ++n; });   // SpTag never added
    EXPECT_EQ(n, 0u);
    EXPECT_EQ((w.reg.CreateView<PosS, SpTag>().Size()), 0u);
}

// s6.5: sparse-driven views partition the dense range into fixed-size work items.
TEST(StorageViewDriver, ParallelSparseDrivenVisitsEachMemberOnce)
{
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    Astra::Registry reg(cfg);
    std::vector<Astra::Entity> es(20000);
    ASSERT_EQ(reg.CreateEntities<Position>(es.size(), std::span{es}), es.size());
    for (size_t i = 0; i < es.size(); i += 5) reg.AddComponent(es[i], PosS{});
    for (size_t i = 0; i < es.size(); i += 5) reg.AddComponent(es[i + 1], PosS{});   // 8000 members

    std::atomic<size_t> visits{0};
    reg.CreateView<PosS>().ParallelForEach([&](Astra::Entity, PosS& s)
    {
        s.y += 1.0f;
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visits.load(), 8000u);
    for (size_t i = 0; i < es.size(); ++i)
        if (reg.HasComponent<PosS>(es[i])) ASSERT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(es[i])->y, 1.0f) << i;
}

// Archetype-driven parallel walk with a Sparse filter and a Universal yield.
TEST(StorageViewDriver, ParallelArchetypeDrivenWithSparseAndUniversal)
{
    auto reg = MakeRegistry<Health>(std::make_shared<Astra::Testing::TestWorkerPool>());
    std::vector<Astra::Entity> es(20000);
    ASSERT_EQ(reg->CreateEntities<Position>(es.size(), std::span{es}), es.size());
    for (size_t i = 0; i < es.size(); i += 2) reg->AddComponent(es[i], PosS{});

    std::atomic<size_t> visits{0};
    reg->CreateView<Position, PosS, Health>().ParallelForEach([&](Astra::Entity, Position&, PosS&, Health& h)
    {
        h.current += 1;
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visits.load(), 10000u);

    std::atomic<size_t> all{0};
    reg->CreateView<Position, Health>().ParallelForEach([&](Astra::Entity, Position&, Health&) { all.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(all.load(), es.size());
    for (size_t i = 0; i < es.size(); ++i)
        ASSERT_EQ(std::as_const(*reg).GetComponent<Health>(es[i])->current, i % 2 == 0 ? 101 : 100) << i;
}

TEST(StorageViewDriver, ParallelForEachWithContextSparseDrivenStampsFlatItemIndices)
{
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    World w(cfg);
    std::mutex mu;
    std::set<uint32_t> items;
    std::atomic<size_t> visits{0};
    const size_t n = w.reg.CreateView<PosS>().ParallelForEachWithContext(
        [&](uint32_t idx) { std::lock_guard lock(mu); items.insert(idx); return idx; },
        [&](Astra::Entity, PosS&, uint32_t) { visits.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(visits.load(), w.posS.size());
    EXPECT_EQ(n, items.size());
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(*items.begin(), 0u);
    EXPECT_EQ(*items.rbegin(), uint32_t(n - 1));
}
```

`Health` defaults to `current = 100`, hence 101 / 100.

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewDriver.*`
Expected failures:
- `SparseDrivenVisitsTheSetInDenseOrder` and `SmallestRequiredSetDrives`: the archetype-driven walk visits archetype by archetype, not in dense order.
- The parallel tests: `ParallelForEachSince` never resolves the stores, so the sparse filter skips every chunk (0 visits).
- The Universal parallel walk crashes in `ForEachChunkStamped` on the null `Health` column.

- [ ] **Step 3: Add the per-entity predicate**

In `View.hpp`'s storage-path block, after `EntityGroupsPass` (Task 26), add:

```cpp
        // Enabled-only filter of one required enableable T, for one entity.
        template<typename T>
        ASTRA_NODISCARD bool EntityEnabled(const EntityRecord* rec, Entity e, const StoreArray& st) const
        {
            using Bare = std::remove_const_t<T>;
            const StoreRef& s = st[PlanIndex<Bare>];
            switch (m_kinds[PlanIndex<Bare>])
            {
            case StorageKind::Table:
                return !rec->chunk->IsDisabled(rec->archetype->GetColumnMeta().idToColumn[TypeID<Bare>::Value()],
                                               rec->location.GetEntityIndex());
            case StorageKind::Sparse:    return !(s.sparse && s.sparse->IsDisabled(e));
            case StorageKind::Universal: return !(s.universal && s.universal->IsDisabled(e));
            }
            return true;
        }

        // One Changed/Added term for one entity: the chunk tier and the per-entity tier
        // of VisitChunkStorage folded together (Task 27's table of sources).
        template<bool UseAdded, typename T>
        ASTRA_NODISCARD bool EntityChangeTermPasses(const EntityRecord* rec, Entity e, const StoreArray& st, Tick since) const
        {
            using Bare = std::remove_const_t<T>;
            const StoreRef& s = st[PlanIndex<Bare>];
            switch (m_kinds[PlanIndex<Bare>])
            {
            case StorageKind::Table:
            {
                const int col = rec->archetype->GetColumnMeta().idToColumn[TypeID<Bare>::Value()];
                if (!IsNewer(rec->chunk->GetColumnVersion(col), since))
                    return false;
                if constexpr (IsChangeTrackedV<Bare>)
                {
                    const EntityTicks& t = rec->chunk->GetTicks(col)[rec->location.GetEntityIndex()];
                    return IsNewer(UseAdded ? t.added : t.changed, since);
                }
                else
                    return true;
            }
            case StorageKind::Sparse:
                if constexpr (IsChangeTrackedV<Bare>)
                {
                    const EntityTicks* t = s.sparse ? Detail::SparseTicksOf(*s.sparse, e) : nullptr;
                    return t && IsNewer(UseAdded ? t->added : t->changed, since);
                }
                else
                {
                    ComponentStorage* storage = m_archetypeManager->GetComponentStorage();
                    return storage && IsNewer(storage->CoarseVersion(TypeID<Bare>::Value()), since);
                }
            case StorageKind::Universal:
                return s.universal && IsNewer(UseAdded ? s.universal->AddedTick(e) : s.universal->ChangedTick(e), since);
            }
            return true;
        }

        // Every per-entity constraint (spec s6.2). The caller has already checked the
        // record's archetype with MatchesArchetype. Used by the sparse-driven walk and by
        // Get()/Contains() (Task 29).
        ASTRA_NODISCARD bool EntityPasses(const EntityRecord* rec, Entity e, const StoreArray& st, [[maybe_unused]] Tick since) const
        {
            bool ok = true;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((ok = ok && EntityHolds<Ts>(rec, e, st)), ...);
            }(static_cast<SparseRequiredTypes*>(nullptr));
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((ok = ok && !(m_kinds[PlanIndex<Ts>] == StorageKind::Sparse && EntityHolds<Ts>(rec, e, st))), ...);
            }(static_cast<SparseExcludedTypes*>(nullptr));
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((ok = ok && EntityEnabled<Ts>(rec, e, st)), ...);
            }(static_cast<EnabledRequiredFilter*>(nullptr));
            ok = ok && EntityGroupsPass(rec, e, st);
            if constexpr (HasChangeFilter)
            {
                [&]<typename... Ts>(std::tuple<Ts...>*)
                {
                    ((ok = ok && EntityChangeTermPasses<false, Ts>(rec, e, st, since)), ...);
                }(static_cast<ChangedTypes*>(nullptr));
                [&]<typename... Ts>(std::tuple<Ts...>*)
                {
                    ((ok = ok && EntityChangeTermPasses<true, Ts>(rec, e, st, since)), ...);
                }(static_cast<AddedTypes*>(nullptr));
            }
            return ok;
        }
```

- [ ] **Step 4: Add the sparse driver and the per-entity visit**

Continue the block:

```cpp
        // ---- Sparse-driven walk (spec s6.2) ----
        static constexpr size_t SPARSE_WORK_ITEM = 1024;   // dense entries per parallel work item (s6.5)

        // The required Sparse set with the fewest members, when no Table-kind argument is
        // required. `empty`: a required Sparse type has no member at all (nothing matches).
        struct SparseDriver { SparseSet* set = nullptr; bool empty = false; };

        template<typename T>
        void ConsiderDriver(SparseDriver& d) const
        {
            if (m_kinds[PlanIndex<T>] != StorageKind::Sparse)
                return;                                       // declared Universal here: never a driver
            SparseSet* s = m_stores[PlanIndex<T>].sparse;
            if (!s || s->Size() == 0)
            {
                d.empty = true;
                return;
            }
            if (!d.set || s->Size() < d.set->Size())
                d.set = s;
        }

        ASTRA_NODISCARD SparseDriver PickSparseDriver() const
        {
            SparseDriver d;
            if constexpr (std::tuple_size_v<SparseRequiredTypes> > 0)
            {
                if (m_matchRequired.None())                  // no Table-kind required/With/Changed/Added
                {
                    [&]<typename... Ts>(std::tuple<Ts...>*)
                    {
                        (ConsiderDriver<std::remove_const_t<Ts>>(d), ...);
                    }(static_cast<SparseRequiredTypes*>(nullptr));
                }
            }
            if (d.empty)
                d.set = nullptr;
            return d;
        }

        // One candidate entity: archetype match, per-entity constraints, fetch, call.
        template<bool Stamp, typename Func, size_t... ReqIs, size_t... OptIs>
        void VisitEntityStorageImpl(Entity e, Func& func, Tick now, Tick since,
                                    std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq)
        {
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return;
            if (!MatchesArchetype(rec->archetype->GetMask()) || !EntityPasses(rec, e, m_stores, since))
                return;
            ArchetypeChunk* chunk = rec->chunk;
            const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
            const size_t i = rec->location.GetEntityIndex();
            if constexpr (Stamp)
            {
                StampChunkStorage(chunk, cm, now, reqSeq, optSeq);
                ComponentStorage* storage = m_archetypeManager->GetComponentStorage();
                (StampSparseCoarse<std::tuple_element_t<ReqIs, RequiredTypes>>(storage, now), ...);
                (StampSparseCoarse<std::tuple_element_t<OptIs, OptionalTypes>>(storage, now), ...);
            }
            std::tuple<Fetch<std::tuple_element_t<ReqIs, RequiredTypes>>...> req{
                MakeFetch<std::tuple_element_t<ReqIs, RequiredTypes>>(chunk, cm, m_stores)... };
            std::tuple<Fetch<std::tuple_element_t<OptIs, OptionalTypes>>...> opt{
                MakeFetch<std::tuple_element_t<OptIs, OptionalTypes>>(chunk, cm, m_stores)... };
            InvokeStorageEntity<Stamp>(e, i, req, opt, func, now, reqSeq, optSeq);
        }

        // Dense range [lo, hi) of the driving set. Index-based, re-reading Size() and
        // Entities() every step: a sparse add/remove made inside the callback (refused by
        // the Debug guard, s4.3) may swap-and-pop or reallocate the dense arrays, and this
        // walk never holds a pointer across a call.
        template<bool Stamp, typename Func>
        void VisitSparseRange(SparseSet* set, size_t lo, size_t hi, Func& func, Tick now, Tick since)
        {
            for (size_t d = lo; d < hi && d < set->Size(); ++d)
                VisitEntityStorageImpl<Stamp>(set->Entities()[d], func, now, since,
                                              std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{},
                                              std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{});
        }
```

Continue the block with the parallel sparse-driven forms:

```cpp
        // Sparse-driven ParallelForEach: fixed SPARSE_WORK_ITEM dense ranges. Each entity
        // owns its own slot in every store, so workers write disjoint elements; table
        // column stamps are same-value stores, the benign pattern of the chunk walk.
        template<typename Func>
        void ParallelSparseDriven(SparseSet* set, Func& adapted, Tick since)
        {
            const size_t n = set->Size();
            if (!m_scheduler || n < MIN_ENTITIES_FOR_PARALLEL)
            {
                ForEachSince(since, adapted);
                return;
            }
            const Tick now = m_archetypeManager->CurrentTick();
            const size_t items = (n + SPARSE_WORK_ITEM - 1) / SPARSE_WORK_ITEM;
            m_scheduler->ParallelFor(items, 1, [&](size_t begin, size_t end, uint32_t /*worker*/)
            {
                for (size_t w = begin; w < end; ++w)
                    VisitSparseRange<true>(set, w * SPARSE_WORK_ITEM, std::min(n, (w + 1) * SPARSE_WORK_ITEM), adapted, now, since);
            });
        }

        // Sparse-driven ParallelForEachWithContext: the FLAT work-item index `w` is the
        // sub-context's iteration index (deterministic: the dense order is). Returns the
        // number of work items, identical on the inline and the dispatch paths.
        template<typename Factory, typename Body>
        size_t ParallelSparseDrivenWithContext(SparseSet* set, Tick since, Factory& factory, Body& body)
        {
            const size_t n = set->Size();
            if (n == 0)
                return 0;
            const size_t items = (n + SPARSE_WORK_ITEM - 1) / SPARSE_WORK_ITEM;
            const Tick now = m_archetypeManager->CurrentTick();
            auto runItem = [&](size_t w)
            {
                auto sub = factory(static_cast<uint32_t>(w));
                auto wrapped = [&body, &sub](Astra::Entity e, auto&&... comps)
                {
                    body(e, std::forward<decltype(comps)>(comps)..., sub);
                };
                VisitSparseRange<true>(set, w * SPARSE_WORK_ITEM, std::min(n, (w + 1) * SPARSE_WORK_ITEM), wrapped, now, since);
            };
            if (!m_scheduler || n < MIN_ENTITIES_FOR_PARALLEL || items < 2)
            {
                for (size_t w = 0; w < items; ++w)
                    runItem(w);
                return items;
            }
            m_scheduler->ParallelFor(items, 1, [&](size_t begin, size_t end, uint32_t /*worker*/)
            {
                for (size_t w = begin; w < end; ++w)
                    runItem(w);
            });
            return items;
        }
```

- [ ] **Step 5: Select the driver in every walk**

1. Replace `ForEachBody`'s body with:

```cpp
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            EnsureArchetypes();
            if (UsesStoragePath())
                ResolveStores();

#ifdef ASTRA_BUILD_DEBUG
            // Captured AFTER EnsureArchetypes() so its own (legitimate) refresh
            // of the counter is never mistaken for an in-loop structural change.
            const uint32_t debugStartStructuralChangeCounter =
                m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            // Sparse adds/removes never touch the archetype set, so they have their own
            // counter (Task 16); a sparse change inside the walk is refused like a table one.
            const uint32_t debugStartSparseCounter = m_archetypeManager->GetSparseStructuralCounter();
#endif

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;

            auto adapted = MakeEntityOptionalAdapter(func);
            bool walked = false;
            if constexpr (HasSparseArg)
            {
                // s6.2: no Table-kind required argument and a required Sparse one => the
                // smallest required set drives; an empty required set matches nothing.
                const SparseDriver d = PickSparseDriver();
                if (d.empty)
                    walked = true;
                else if (d.set)
                {
                    VisitSparseRange<Stamp>(d.set, 0, std::numeric_limits<size_t>::max(), adapted, m_archetypeManager->CurrentTick(), since);
                    walked = true;
                }
            }
            if (!walked)
            {
                for (Archetype* archetype : m_archetypes)
                {
                    ForEachImpl<Stamp>(archetype, adapted, since, RequiredTypes{}, OptionalTypes{});
                }
            }

#ifdef ASTRA_BUILD_DEBUG
            ASTRA_ASSERT(m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire) == debugStartStructuralChangeCounter &&
                         m_archetypeManager->GetSparseStructuralCounter() == debugStartSparseCounter,
                "Structural mutation (create/destroy entity, add/remove component) detected during View::ForEach. "
                "Defer structural changes into a CommandBuffer and call Execute() after the loop.");
#endif
```

`VisitSparseRange` re-reads `Size()` at every step, so a set that shrinks inside the callback ends the walk early, and one that grows is walked to its new end. Add `#include <limits>` to `View.hpp`'s standard includes.

2. In `ParallelForEachSince`, insert after `auto adapted = MakeEntityOptionalAdapter(func);`:

```cpp
            if (UsesStoragePath())
                ResolveStores();                 // on the calling thread, before any worker reads m_stores
            if constexpr (HasSparseArg)
            {
                const SparseDriver d = PickSparseDriver();
                if (d.empty)
                    return;
                if (d.set)
                {
                    ParallelSparseDriven(d.set, adapted, since);
                    return;
                }
            }
```

3. In `ParallelForEachWithContext(Tick since, ...)`, insert after `if (m_archetypes.empty()) ASTRA_UNLIKELY return 0;`:

```cpp
            if (UsesStoragePath())
                ResolveStores();
            if constexpr (HasSparseArg)
            {
                const SparseDriver d = PickSparseDriver();
                if (d.empty)
                    return 0;
                if (d.set)
                    return ParallelSparseDrivenWithContext(d.set, since, factory, body);
            }
```

4. In `ParallelForEachChunkImpl`, at the top of the `if constexpr (!HasChunkFilter)` branch (before `(void)since;`), add:

```cpp
                if (m_anyUniversal) ASTRA_UNLIKELY
                {
                    const auto& chunks = archetype->GetChunks();
                    if (chunkIndex < chunks.size())
                        VisitChunkStorage<true>(archetype, chunks[chunkIndex].get(), func, m_archetypeManager->CurrentTick(), since,
                                                std::make_index_sequence<sizeof...(Required)>{},
                                                std::make_index_sequence<sizeof...(Optional)>{});
                    return;
                }
```

The `else` branch already reaches `VisitChunkFiltered`, which dispatches storage-path views (Task 24).

- [ ] **Step 6: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewDriver.*:StorageView*:IterationSafety.*:ParallelIteration*:SystemContext*`
Expected: all PASS. `IterationSafety.SparseRemoveDuringSparseOnlyViewTripsTheGuardAndStaysSafe` now runs through the sparse-driven walk.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/View.hpp tests/Registry/StorageViewDriverTest.cpp
git commit -m "feat(storage): sparse-driven driver -- no Table-kind required argument => walk the smallest required sparse set's dense array (per-entity archetype + EntityPasses checks, index-based, deterministic dense order); parallel paths resolve stores before dispatch, partition sparse-driven views into 1024-entry work items, route Universal views through VisitChunkStorage (s6.2, s6.5)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 29: `Get` / `Single` / `Contains` and range-for over storage-path views (s6.5)

The random-access surface (`Contains`, `Get`, and `Single`, which ends in `Get`) decides visibility per entity through `EntityPasses`. It fetches Sparse/Universal elements from their stores, applies the same write stamps as `ForEach`, and hands out `Mut<T>` for tracked elements. An `Optional` tag keeps `Get()`'s pre-existing contract: a present tag yields its static instance.

Range-for over a storage-path view steps through matching runs: `ViewIterator` receives `ViewIterHooks` from the view.
- **Per chunk:** the match mask, built with the same `BuildChunkFilter` + `WriteMatchWords`.
- **Per entity:** the element pointers, needed only when a yielded component is Sparse or Universal.
- **Pure-Table views:** no hooks, and the pre-existing walk is unchanged except for one extra index increment per step.

A required Sparse argument no longer meets a range-for refusal. The enableable refusal (`HasRequiredFilter`) and the tracked-yield refusal are left exactly as they are.

**Files:**
- Modify: `include/Astra/Registry/ViewIterator.hpp`
- Modify: `include/Astra/Registry/View.hpp`
- Create: `tests/Registry/StorageViewAccessTest.cpp`

**Interfaces:**
- Consumes: `EntityPasses` / `EntityHolds` / `EntityEnabled` (Tasks 26, 28), `Fetch` / `MakeFetch` / `TicksFor` / the stamp helpers (Tasks 24-25), `Detail::WriteMatchWords` (Task 23).
- Produces:
  ```cpp
  struct ViewIterHooks {                                   // ViewIterator.hpp
    const void* view;
    bool (*chunkMask)(const void* view, Archetype*, ArchetypeChunk*, std::vector<uint64_t>& words);
    bool (*resolve)(const void* view, Archetype*, ArchetypeChunk*, size_t index, void** out);
    bool perEntity;
  };
  ViewIterable<Cs...>::Iterator(Archetype* const*, size_t, Tick now, const ViewIterHooks* hooks = nullptr);
  ```
  `View` private: `VisibleRecordWith`, `GetStorageImpl`, `IterPerEntity`, `IterChunkMask`, `IterResolve`, `m_iterHooks`.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageViewAccessTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    using S = std::set<Astra::Entity>;

    // range-for and ForEach visit the same entity set (s6.5, D2).
    template<typename V>
    void ExpectRangeForMatchesForEach(V view)
    {
        S viaForEach, viaRange;
        view.ForEach([&](Astra::Entity e, auto&&...) { viaForEach.insert(e); });
        for (auto t : view) viaRange.insert(std::get<0>(t));
        EXPECT_EQ(viaRange, viaForEach);
        EXPECT_EQ(viaRange.size(), view.Size());
    }
}

TEST(StorageViewAccess, ContainsGetAndSingleAreStorageAware)
{
    Astra::Registry reg;
    Astra::Entity a = reg.CreateEntity<Position>();
    reg.AddComponent(a, PosS{1.0f, 2.0f, 3.0f});
    Astra::Entity b = reg.CreateEntity<Position>();

    auto view = reg.CreateView<Position, PosS>();
    EXPECT_TRUE(view.Contains(a));
    EXPECT_FALSE(view.Contains(b));
    auto ra = view.Get(a);
    ASSERT_TRUE(ra.IsOk());
    auto [pa, sa] = *ra.GetValue();
    ASSERT_NE(sa, nullptr);
    EXPECT_FLOAT_EQ(sa->y, 2.0f);
    EXPECT_EQ(static_cast<const PosS*>(sa), std::as_const(reg).GetComponent<PosS>(a));
    EXPECT_TRUE(view.Get(b).IsErr());

    ASSERT_TRUE(view.Single().IsOk());
    reg.AddComponent(b, PosS{});
    EXPECT_EQ(*view.Single().GetError(), Astra::QueryError::MultipleMatched);

    // Optional: a present Sparse tag yields its static instance on Get(); an absent one null.
    reg.AddComponent(a, SpTag{});
    auto ov = reg.CreateView<Position, Astra::Optional<SpTag>, Astra::Optional<SpB>>();
    auto oa = ov.Get(a);
    ASSERT_TRUE(oa.IsOk());
    EXPECT_NE(std::get<1>(*oa.GetValue()), nullptr);
    EXPECT_EQ(std::get<2>(*oa.GetValue()), nullptr);
    EXPECT_EQ(std::get<1>(*ov.Get(b).GetValue()), nullptr);

    // Enabled-only filtering per entity.
    reg.AddComponent(a, SpE{});
    ASSERT_TRUE(reg.SetEnabled<SpE>(a, false));
    EXPECT_FALSE((reg.CreateView<Position, const SpE>().Contains(a)));
    EXPECT_TRUE((reg.CreateView<Position, Astra::IncludeDisabled<SpE>>().Contains(a)));
}

TEST(StorageViewAccess, GetOnUniversalAndTrackedMut)
{
    auto reg = MakeRegistry<Health>();
    Astra::Entity e = reg->CreateEntity<Position>();
    auto r = reg->CreateView<Position, Health>().Get(e);
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(static_cast<const Health*>(std::get<1>(*r.GetValue())), std::as_const(*reg).GetComponent<Health>(e));

    Astra::Registry treg;
    Astra::Entity t = treg.CreateEntity<Position>();
    treg.AddComponent(t, SpE{});
    const Astra::Tick since = treg.CurrentTick();
    treg.AdvanceTick();
    auto rt = treg.CreateView<SpE>().Get(t);
    ASSERT_TRUE(rt.IsOk());
    std::get<0>(*rt.GetValue()).Write().v = 3.0f;          // Mut<SpE>: marks on use
    EXPECT_TRUE(treg.IsChanged<SpE>(t, since));
    EXPECT_FLOAT_EQ(std::as_const(treg).GetComponent<SpE>(t)->v, 3.0f);
}

TEST(StorageViewAccess, RangeForMatchesForEachOnStorageViews)
{
    Astra::Registry reg;
    for (int i = 0; i < 2000; ++i)
    {
        Astra::Entity e = (i % 3 == 0) ? reg.CreateEntity<Position, Velocity>() : reg.CreateEntity<Position>();
        if (i % 4 == 0) reg.AddComponent(e, PosS{float(i), 0.0f, 0.0f});
        if (i % 5 == 0) reg.AddComponent(e, SpTag{});
    }
    ExpectRangeForMatchesForEach(reg.CreateView<Position, PosS>());                     // per-entity stepping
    ExpectRangeForMatchesForEach(reg.CreateView<Position, Astra::With<SpTag>>());       // run stepping
    ExpectRangeForMatchesForEach(reg.CreateView<Position, Astra::Not<PosS>>());
    ExpectRangeForMatchesForEach(reg.CreateView<PosS>());
    ExpectRangeForMatchesForEach(reg.CreateView<Position, Astra::Any<PosS, SpTag>>());
    ExpectRangeForMatchesForEach(reg.CreateView<Position, SpTag>());

    auto ureg = MakeRegistry<Health>();
    for (int i = 0; i < 1500; ++i) (void)ureg->CreateEntity<Position>();
    ExpectRangeForMatchesForEach(ureg->CreateView<Position, Health>());
}

TEST(StorageViewAccess, RangeForWritesPersistAndStamp)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 300; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        if (i % 2 == 0) reg.AddComponent(es.back(), PosS{});
    }
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    for (auto [e, pos, s] : reg.CreateView<Position, PosS>())
        s.y = 5.0f;
    for (int i = 0; i < 300; i += 2)
    {
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(es[i])->y, 5.0f);
        EXPECT_TRUE(reg.IsChanged<PosS>(es[i], since));   // untracked: coarse stamp through the iterator
    }
}
```

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewAccess.*`
Expected: FAIL.
- `Contains(b)` is true: archetype matching only.
- `Get(a)` yields a null `PosS*`, and the `sa->y` check fails after `ASSERT_NE`.
- Range-for over a Sparse or Universal yield crashes on a null column base.
The crash ends the run. That is the expected red.

- [ ] **Step 3: Teach `ViewIterator` run-stepping**

In `include/Astra/Registry/ViewIterator.hpp`:

1. Add `#include <bit>`, `#include <cstdint>`, `#include <utility>` and `#include <vector>` to the includes.
2. Insert directly after `struct ViewSentinel {};`:

```cpp
    /**
     * Storage-policy hooks for range-for (spec 2026-10-01 s6.5). A View naming a Sparse
     * type, or whose registry declares one of its types Universal, hands the iterator
     * these. Per chunk they give the match mask (presence / disabled-mirror / group words);
     * per entity, the element pointers of the yielded components. A pure-Table view
     * passes none, and the iterator walks exactly as before.
     */
    struct ViewIterHooks
    {
        const void* view = nullptr;
        // Fills `words` ((count + 63) / 64, tail cleared) with the chunk's match mask; false = skip the chunk.
        bool (*chunkMask)(const void* view, Archetype* archetype, ArchetypeChunk* chunk, std::vector<uint64_t>& words) = nullptr;
        // Writes each yielded component's element pointer for slot `index` into out[]; false = skip the entity.
        bool (*resolve)(const void* view, Archetype* archetype, ArchetypeChunk* chunk, size_t index, void** out) = nullptr;
        // Step one entity at a time through `resolve` (a yielded component is Sparse or Universal).
        bool perEntity = false;
    };
```

3. In `class Iterator`, make these changes:

   a. Replace the constructor with:

```cpp
            Iterator(Archetype* const* archetypes, size_t archetypeCount, Tick now, const ViewIterHooks* hooks = nullptr) noexcept
                : m_archetypes(archetypes)
                , m_archetypeCount(archetypeCount)
                , m_now(now)
                , m_hooks(hooks)
            {
                if (m_archetypeCount > 0)
                {
                    FindFirstValidPosition();
                }
            }
```

   b. Replace `operator++()` with:

```cpp
            // Hot path: one more index increment than before (m_compIndex) and the
            // same single compare; m_runEnd == the chunk's count when no hooks are set.
            ASTRA_FORCEINLINE Iterator& operator++() noexcept
            {
                ++m_entityIndex;
                ++m_compIndex;
                if (m_entityIndex >= m_runEnd) [[unlikely]]
                {
                    NextRun();
                }
                return *this;
            }
```

   c. Replace `FindFirstValidPosition`, `AdvanceChunk` and `CacheChunkState` with:

```cpp
            void FindFirstValidPosition() noexcept
            {
                while (m_archetypeIndex < m_archetypeCount)
                {
                    Archetype* archetype = m_archetypes[m_archetypeIndex];
                    const auto& chunks = archetype->GetChunks();

                    while (m_chunkIndex < chunks.size())
                    {
                        if (chunks[m_chunkIndex]->GetCount() > 0 && EnterChunk(archetype, m_chunkIndex))
                            return;
                        ++m_chunkIndex;
                    }

                    ++m_archetypeIndex;
                    m_chunkIndex = 0;
                }

                // End state
                m_entityIndex = 0;
                m_chunkEntityCount = 0;
            }

            void AdvanceChunk() noexcept
            {
                ++m_chunkIndex;
                FindFirstValidPosition();
            }

            // Past the current run: the next run of this chunk (hooks), else the next chunk.
            void NextRun() noexcept
            {
                if (m_hooks && SeekRun(m_entityIndex))
                    return;
                AdvanceChunk();
            }

            // Enter a non-empty chunk. False when no entity of it is visible (hooks only).
            bool EnterChunk(Archetype* archetype, size_t chunkIndex) noexcept
            {
                ArchetypeChunk* chunk = archetype->GetChunks()[chunkIndex].get();
                m_chunkEntityCount = chunk->GetCount();
                m_entities = chunk->GetEntities().data();
                m_componentArrays = std::tuple{chunk->GetComponentArray<std::remove_const_t<Components>>()...};
                if (m_hooks)
                {
                    m_archetype = archetype;
                    m_chunk = chunk;
                    if (!m_hooks->chunkMask(m_hooks->view, archetype, chunk, m_words) || !SeekRun(0))
                    {
                        m_chunkEntityCount = 0;
                        return false;
                    }
                    StampChunk(archetype, chunk);    // only a chunk with a visible entity is stamped
                    return true;
                }
                StampChunk(archetype, chunk);
                m_entityIndex = 0;
                m_compIndex = 0;
                m_runEnd = m_chunkEntityCount;
                return true;
            }

            // Coarse change-detection stamp (spec §3.3 row 1): every non-const component
            // column present in this chunk. A Sparse or Universal yield has no column here.
            // Its store stamp happens in the view's resolve hook. All-const iteration
            // compiles to nothing.
            ASTRA_FORCEINLINE void StampChunk([[maybe_unused]] Archetype* archetype, [[maybe_unused]] ArchetypeChunk* chunk) noexcept
            {
                if constexpr ((Detail::IsMutableYield<Components> || ...))
                {
                    const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
                    ((Detail::IsMutableYield<Components>
                        ? StampColumnIfPresent(chunk, cm.idToColumn[TypeID<std::remove_const_t<Components>>::Value()])
                        : void()), ...);
                }
            }
            ASTRA_FORCEINLINE void StampColumnIfPresent(ArchetypeChunk* chunk, int col) noexcept
            {
                if (col >= 0)
                    chunk->StampColumn(col, m_now);
            }

            // First index >= from (within this chunk) whose match bit equals `set`; the
            // count when there is none. The words are tail-cleared (WriteMatchWords).
            size_t NextBit(size_t from, bool set) const noexcept
            {
                const size_t n = m_chunkEntityCount;
                while (from < n)
                {
                    const size_t w = from >> 6;
                    uint64_t word = set ? m_words[w] : ~m_words[w];
                    word &= ~uint64_t(0) << (from & 63);
                    if (word)
                    {
                        const size_t idx = (w << 6) + static_cast<size_t>(std::countr_zero(word));
                        return idx < n ? idx : n;
                    }
                    from = (w + 1) << 6;
                }
                return n;
            }

            // Position on the first visible entity at or after `from` (hooks mode). Run
            // mode: the whole run [i, end) with the chunk arrays. Per-entity mode: one
            // entity, with its element pointers; an entity whose element vanished mid-walk
            // (reported by the Debug guard) is skipped.
            bool SeekRun(size_t from) noexcept
            {
                const size_t n = m_chunkEntityCount;
                for (size_t i = NextBit(from, true); i < n; i = NextBit(i + 1, true))
                {
                    m_entityIndex = i;
                    if (!m_hooks->perEntity)
                    {
                        m_runEnd = NextBit(i, false);
                        m_compIndex = i;
                        return true;
                    }
                    void* ptrs[sizeof...(Components) == 0 ? 1 : sizeof...(Components)] = {};
                    if (!m_hooks->resolve(m_hooks->view, m_archetype, m_chunk, i, ptrs))
                        continue;
                    SetEntityPointers(ptrs, std::index_sequence_for<Components...>{});
                    m_runEnd = i + 1;
                    m_compIndex = 0;
                    return true;
                }
                return false;
            }

            template<size_t... Is>
            void SetEntityPointers([[maybe_unused]] void** ptrs, std::index_sequence<Is...>) noexcept
            {
                ((std::get<Is>(m_componentArrays) = static_cast<std::remove_const_t<Components>*>(ptrs[Is])), ...);
            }
```

   d. In `DerefComponent`, replace `return array[m_entityIndex];` with `return array[m_compIndex];`.

   e. Add these members after `std::tuple<std::remove_const_t<Components>*...> m_componentArrays{};`:

```cpp
            // Storage-policy run-stepping (hooks mode); m_runEnd/m_compIndex are also the
            // plain walk's bound and component index (== m_chunkEntityCount / m_entityIndex).
            Archetype* m_archetype = nullptr;
            ArchetypeChunk* m_chunk = nullptr;
            std::vector<uint64_t> m_words;
            size_t m_runEnd = 0;
            size_t m_compIndex = 0;
```

   f. Add `const ViewIterHooks* m_hooks = nullptr;   // null = plain Table walk` to the "View-level state" group, directly after `Tick m_now`. That keeps the constructor's member-initializer order equal to the declaration order (no `-Wreorder`).

- [ ] **Step 4: Storage-aware random access and the range-for hooks in `View`**

In `View.hpp`:

1. Add these members to the storage-path block, after `ParallelSparseDrivenWithContext`:

```cpp
        // ---- Random access (spec s6.5): Get / Single / Contains ----

        // The record iff `e` is alive, matches at archetype level, and passes every
        // per-entity constraint. `st` receives the stores the caller then fetches through.
        ASTRA_NODISCARD const EntityRecord* VisibleRecordWith(Entity e, StoreArray& st) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return nullptr;
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
            if (!rec || !rec->chunk)
                return nullptr;
            if (!MatchesArchetype(rec->archetype->GetMask()))
                return nullptr;
            ResolveStoresInto(st);
            return EntityPasses(rec, e, st, Tick{0}) ? rec : nullptr;   // Tick unused: change views refuse Get
        }

        // Get()'s Optional: a present tag yields its static instance (the pre-existing
        // Table contract via ArchetypeChunk::GetComponent); otherwise the ForEach rule.
        template<size_t K, typename F>
        std::remove_const_t<std::tuple_element_t<K, OptionalTypes>>* GetOptionalAt(F& f, const EntityRecord* rec, Entity e,
                                                                                   size_t i, const StoreArray& st) const
        {
            using Bare = std::remove_const_t<std::tuple_element_t<K, OptionalTypes>>;
            if constexpr (std::is_empty_v<Bare>)
            {
                if (!EntityHolds<Bare>(rec, e, st))
                    return nullptr;
                if constexpr (IsEnableableV<Bare>)
                {
                    if (!EntityEnabled<Bare>(rec, e, st))
                        return nullptr;
                }
                static Bare s_emptyInstance{};
                return &s_emptyInstance;
            }
            else
            {
                return f.OptionalAt(e, i);
            }
        }

        template<size_t K, typename F>
        AccessElement<std::tuple_element_t<K, RequiredTypes>> AccessFrom([[maybe_unused]] F& f,
                                                                         std::remove_const_t<std::tuple_element_t<K, RequiredTypes>>* p,
                                                                         [[maybe_unused]] Entity e, [[maybe_unused]] size_t i,
                                                                         [[maybe_unused]] Tick now) const
        {
            using R = std::tuple_element_t<K, RequiredTypes>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
                return Mut<R>(p, TicksFor(f, e, i), now);   // marks on use, with the tick current at Get()
            else
                return p;
        }

        // Get() on the storage path: visibility per entity, the same write stamps a
        // ForEach visit applies (a non-const request is a write, spec s3.3), Mut<T> for
        // tracked elements.
        template<size_t... ReqIs, size_t... OptIs>
        Result<AccessTuple, QueryError> GetStorageImpl(Entity e, std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq) const
        {
            StoreArray st{};
            const EntityRecord* rec = VisibleRecordWith(e, st);
            if (!rec) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::NotMatched);
            ArchetypeChunk* chunk = rec->chunk;
            const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
            const size_t i = rec->location.GetEntityIndex();
            const Tick now = m_archetypeManager->CurrentTick();
            ComponentStorage* storage = m_archetypeManager->GetComponentStorage();

            StampChunkStorage(chunk, cm, now, reqSeq, optSeq);
            (StampSparseCoarse<std::tuple_element_t<ReqIs, RequiredTypes>>(storage, now), ...);
            (StampSparseCoarse<std::tuple_element_t<OptIs, OptionalTypes>>(storage, now), ...);

            std::tuple<Fetch<std::tuple_element_t<ReqIs, RequiredTypes>>...> req{
                MakeFetch<std::tuple_element_t<ReqIs, RequiredTypes>>(chunk, cm, st)... };
            std::tuple<Fetch<std::tuple_element_t<OptIs, OptionalTypes>>...> opt{
                MakeFetch<std::tuple_element_t<OptIs, OptionalTypes>>(chunk, cm, st)... };
            std::tuple<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>*...> rp{ std::get<ReqIs>(req).At(e, i)... };
            std::tuple<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>*...> op{
                GetOptionalAt<OptIs>(std::get<OptIs>(opt), rec, e, i, st)... };
            (StampRequiredStore<std::tuple_element_t<ReqIs, RequiredTypes>>(std::get<ReqIs>(req), e, now), ...);
            (StampOptionalStore<std::tuple_element_t<OptIs, OptionalTypes>>(std::get<OptIs>(opt), std::get<OptIs>(op), e, i, now), ...);
            return Result<AccessTuple, QueryError>::Ok(AccessTuple{
                AccessFrom<ReqIs>(std::get<ReqIs>(req), std::get<ReqIs>(rp), e, i, now)...,
                static_cast<std::tuple_element_t<OptIs, OptionalTypes>*>(std::get<OptIs>(op))... });
        }

        // ---- Range-for hooks (spec s6.5) ----

        // Step one entity at a time when a non-empty yielded component lives in a store.
        ASTRA_NODISCARD bool IterPerEntity() const noexcept
        {
            bool per = false;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((per = per || (!std::is_empty_v<std::remove_const_t<Ts>> && m_kinds[PlanIndex<Ts>] != StorageKind::Table)), ...);
            }(static_cast<RequiredTypes*>(nullptr));
            return per;
        }

        static bool IterChunkMask(const void* self, Archetype* archetype, ArchetypeChunk* chunk, std::vector<uint64_t>& words)
        {
            const View* v = static_cast<const View*>(self);
            const size_t count = chunk->GetCount();
            if (count == 0)
                return false;
            ChunkFilter f;
            if (!v->BuildChunkFilter(archetype, chunk, count, f))
                return false;
            words.assign((count + 63) >> 6, 0ull);
            Detail::WriteMatchWords(f.exclude, f.nExclude, f.require, f.nRequire, count, words.data());
            return true;
        }

        template<size_t... ReqIs>
        bool IterResolveImpl(Archetype* archetype, ArchetypeChunk* chunk, size_t index, [[maybe_unused]] void** out,
                             std::index_sequence<ReqIs...>) const
        {
            const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
            const Entity e = chunk->GetEntities()[index];
            std::tuple<Fetch<std::tuple_element_t<ReqIs, RequiredTypes>>...> req{
                MakeFetch<std::tuple_element_t<ReqIs, RequiredTypes>>(chunk, cm, m_stores)... };
            bool ok = true;
            ((out[ReqIs] = std::get<ReqIs>(req).At(e, index), ok = ok && out[ReqIs] != nullptr), ...);
            if (ok)
            {
                // Range-for yields plain T& (tracked non-const yields are refused), so an
                // untracked Sparse/Universal write is stamped here, like ForEach's visit.
                const Tick now = m_archetypeManager->CurrentTick();
                (StampRequiredStore<std::tuple_element_t<ReqIs, RequiredTypes>>(std::get<ReqIs>(req), e, now), ...);
                ComponentStorage* storage = m_archetypeManager->GetComponentStorage();
                (StampSparseCoarse<std::tuple_element_t<ReqIs, RequiredTypes>>(storage, now), ...);
            }
            return ok;
        }

        static bool IterResolve(const void* self, Archetype* archetype, ArchetypeChunk* chunk, size_t index, void** out)
        {
            return static_cast<const View*>(self)->IterResolveImpl(archetype, chunk, index, out,
                std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{});
        }
```

2. Add the data member `ViewIterHooks m_iterHooks{};   // handed to range-for iterators by begin()` after `m_stores`.

- [ ] **Step 5: Route `Get` / `Contains` / range-for**

1. At the top of `VisibleRecord`'s body, add:

```cpp
            if (UsesStoragePath())
            {
                StoreArray st{};
                return VisibleRecordWith(e, st);
            }
```

2. In `Get(Entity e) const`, directly after its `static_assert`, add:

```cpp
            if (UsesStoragePath())
                return GetStorageImpl(e, std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{},
                                      std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{});
```

The pre-existing body below stays the pure-Table path. On a `HasSparseArg` view it still compiles but is never reached. `Single()` needs no change: it counts through `ForEachBody<false>` and materialises through `Get`.

3. In `begin()`, replace everything from `EnsureArchetypes();` to the end of the function with:

```cpp
            EnsureArchetypes();
            // The iterator carries the current tick so it can apply the coarse
            // change-detection stamp on every chunk it enters (non-const yields only).
            if (UsesStoragePath())
            {
                // s6.5: a Sparse argument (or a Universal member here) is walked in matching
                // runs through the view's hooks; the required-enableable refusal above is
                // unchanged and a required Sparse argument does not trigger it.
                ResolveStores();
                m_iterHooks = ViewIterHooks{ this, &View::IterChunkMask, &View::IterResolve, IterPerEntity() };
                return Iterator(m_archetypes.data(), m_archetypes.size(), m_archetypeManager->CurrentTick(), &m_iterHooks);
            }
            return Iterator(m_archetypes.data(), m_archetypes.size(), m_archetypeManager->CurrentTick());
```

`end()` is unchanged: its refusals are the enableable and tracked-yield ones.

- [ ] **Step 6: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageViewAccess.*:ViewIterator*:View*:StorageView*:Enableable*`
Expected: all PASS, including every pre-existing `ViewIterator*` test (the plain walk).

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/ViewIterator.hpp include/Astra/Registry/View.hpp tests/Registry/StorageViewAccessTest.cpp
git commit -m "feat(storage): Get/Single/Contains decide per entity and fetch from the stores (write stamps, Mut<T>); range-for steps matching runs through ViewIterHooks -- a required Sparse argument no longer meets a refusal, the enableable refusal is unchanged (s6.5)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 30: Reflection APIs and Relations filters over every kind (s3.3 reflection row, s6.5)

Sparse and Universal members are absent from the archetype mask, so each reflection API now falls back to the store when the mask misses: `GetComponentByHash`, `HasComponentByHash`, `GetEntityComponents`, `InspectEntity`, and `GetComponentByName` (which delegates to the first).

`Relations` holds only the `ArchetypeManager`, whose typed `HasComponent` / `GetComponent` / `GetComponentMut` see Table columns only. Tasks 9/11 routed the typed calls in `Registry`, not in the manager. `Relations` therefore resolves a per-object kind plan when constructed, like `View`. Its filters and yields go through two storage-aware helpers. A non-const yield is a write, as on the Table arm.

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (the five reflection APIs; private `StoredElementByID`)
- Modify: `include/Astra/Registry/StorageAccess.hpp` (`EffectiveKindOf`, `HasInStorage`, `GetInStorage`)
- Modify: `include/Astra/Registry/Relations.hpp`
- Create: `tests/Registry/StorageReflectionTest.cpp`

**Interfaces:**
- Consumes: `ComponentStorage::KindOf` (Task 4: Universal from the config mask, safe for unregistered ids), `SparseSlotOf / Sparse / Universal / StampCoarse`, `UniversalColumn::IsLive / Get / MarkWritten`, `SparseSet::Contains / Get`, `Detail::SparseTicksOf` (Task 25), `EmptyComponentSentinel()`.
- Produces:
  ```cpp
  namespace Astra::Detail {
    template<typename T> StorageKind EffectiveKindOf(const ComponentStorage* storage) noexcept;
    template<typename T> bool HasInStorage(ArchetypeManager& am, Entity e, StorageKind kind);
    template<typename T> std::remove_const_t<T>* GetInStorage(ArchetypeManager& am, Entity e, StorageKind kind); // non-const T: a write
  }
  void* Registry::StoredElementByID(Entity, ComponentID, const ComponentDescriptor&, bool& present) const;   // private
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/StorageReflectionTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    template<typename T>
    bool ListsComponent(const std::vector<const Astra::ComponentDescriptor*>& descs)
    {
        return std::any_of(descs.begin(), descs.end(), [](const Astra::ComponentDescriptor* d) { return d->id == Astra::TypeID<T>::Value(); });
    }

    template<typename T, typename Infos>
    const void* InspectedData(const Infos& infos, bool& found)
    {
        for (const auto& info : infos)
            if (info.descriptor->id == Astra::TypeID<T>::Value()) { found = true; return info.data; }
        found = false;
        return nullptr;
    }
}

TEST(StorageReflection, SparseMembersAreVisibleToEveryReflectionApi)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.AddComponent(e, PosS{1.0f, 2.0f, 3.0f});
    reg.AddComponent(e, SpTag{});
    Astra::Entity f = reg.CreateEntity<Position>();
    const void* sparse = std::as_const(reg).GetComponent<PosS>(e);

    EXPECT_EQ(reg.GetComponentByHash(e, Astra::TypeID<PosS>::Hash()), sparse);
    EXPECT_EQ(reg.GetComponentByName(e, Astra::TypeID<PosS>::Name()), sparse);
    EXPECT_EQ(reg.GetComponentByHash(e, Astra::TypeID<SpTag>::Hash()), Astra::EmptyComponentSentinel());
    EXPECT_TRUE(reg.HasComponentByHash(e, Astra::TypeID<PosS>::Hash()));
    EXPECT_TRUE(reg.HasComponentByHash(e, Astra::TypeID<SpTag>::Hash()));
    EXPECT_FALSE(reg.HasComponentByHash(f, Astra::TypeID<PosS>::Hash()));
    EXPECT_EQ(reg.GetComponentByHash(f, Astra::TypeID<PosS>::Hash()), nullptr);

    const auto descs = reg.GetEntityComponents(e);
    EXPECT_TRUE(ListsComponent<Position>(descs));
    EXPECT_TRUE(ListsComponent<PosS>(descs));
    EXPECT_TRUE(ListsComponent<SpTag>(descs));
    EXPECT_FALSE(ListsComponent<PosS>(reg.GetEntityComponents(f)));

    bool found = false;
    const auto infos = reg.InspectEntity(e);
    EXPECT_EQ(InspectedData<PosS>(infos, found), sparse);
    EXPECT_TRUE(found);
    EXPECT_EQ(InspectedData<SpTag>(infos, found), nullptr);   // a tag has no data
    EXPECT_TRUE(found);

    ASSERT_TRUE(reg.RemoveComponent<PosS>(e));
    EXPECT_EQ(reg.GetComponentByHash(e, Astra::TypeID<PosS>::Hash()), nullptr);
    EXPECT_FALSE(reg.HasComponentByHash(e, Astra::TypeID<PosS>::Hash()));
}

TEST(StorageReflection, UniversalMembersAreVisibleAndStaleHandlesMiss)
{
    auto reg = MakeRegistry<Health>();
    Astra::Entity e = reg->CreateEntity<Position>();
    const void* h = std::as_const(*reg).GetComponent<Health>(e);
    EXPECT_EQ(reg->GetComponentByHash(e, Astra::TypeID<Health>::Hash()), h);
    EXPECT_TRUE(reg->HasComponentByHash(e, Astra::TypeID<Health>::Hash()));
    EXPECT_TRUE(ListsComponent<Health>(reg->GetEntityComponents(e)));
    bool found = false;
    EXPECT_EQ(InspectedData<Health>(reg->InspectEntity(e), found), h);
    EXPECT_TRUE(found);

    reg->DestroyEntity(e);
    Astra::Entity recycled = reg->CreateEntity<Position>();   // same id, version + 1 (T1-C2)
    EXPECT_EQ(reg->GetComponentByHash(e, Astra::TypeID<Health>::Hash()), nullptr);
    EXPECT_FALSE(reg->HasComponentByHash(e, Astra::TypeID<Health>::Hash()));
    EXPECT_NE(reg->GetComponentByHash(recycled, Astra::TypeID<Health>::Hash()), nullptr);
}

TEST(StorageReflection, RelationsFiltersAndYieldsSeeSparseMembers)
{
    Astra::Registry reg;
    Astra::Entity parent = reg.CreateEntity<Velocity>();
    std::vector<Astra::Entity> kids;
    for (int i = 0; i < 6; ++i)
    {
        Astra::Entity kid = reg.CreateEntity<Velocity>();
        if (i % 2 == 0) reg.AddComponent(kid, PosS{float(i), 0.0f, 0.0f});
        if (i == 1) reg.AddComponent(kid, SpTag{});
        reg.SetParent(kid, parent);
        kids.push_back(kid);
    }
    EXPECT_EQ(reg.GetRelations<PosS>(parent).GetChildren().size(), 3u);
    EXPECT_EQ(reg.GetRelations<Astra::Not<PosS>>(parent).GetChildren().size(), 3u);
    EXPECT_EQ((reg.GetRelations<Astra::Any<PosS, SpTag>>(parent).GetChildren().size()), 4u);
    EXPECT_EQ((reg.GetRelations<Astra::OneOf<PosS, Velocity>>(parent).GetChildren().size()), 3u);

    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    float sum = 0.0f;
    reg.GetRelations<PosS>(parent).ForEachChild([&](Astra::Entity, PosS& s) { sum += s.x; s.y = 9.0f; });
    EXPECT_FLOAT_EQ(sum, 0.0f + 2.0f + 4.0f);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(kids[2])->y, 9.0f);
    EXPECT_TRUE(reg.IsChanged<PosS>(kids[2], since));          // a non-const yield is a write
}

TEST(StorageReflection, RelationsSeeUniversalMembers)
{
    auto reg = MakeRegistry<Health>();
    Astra::Entity parent = reg->CreateEntity<Velocity>();
    for (int i = 0; i < 4; ++i)
    {
        Astra::Entity kid = reg->CreateEntity<Velocity>();
        reg->SetParent(kid, parent);
    }
    EXPECT_EQ(reg->GetRelations<Health>(parent).GetChildren().size(), 4u);
    EXPECT_EQ(reg->GetRelations<Astra::Not<Health>>(parent).GetChildren().size(), 0u);
    int visits = 0;
    reg->GetRelations<Health>(parent).ForEachChild([&](Astra::Entity, Health& h) { h.current = 1; ++visits; });
    EXPECT_EQ(visits, 4);
}
```

- [ ] **Step 2: Regenerate, build, and verify the failure**

Run premake, build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageReflection.*`
Expected: FAIL.
- `GetComponentByHash` returns `nullptr` for the Sparse and Universal members, and `HasComponentByHash` returns false.
- `GetEntityComponents` and `InspectEntity` omit them.
- The Relations filters count 0 holders, because `ArchetypeManager::HasComponent` tests the mask only.

- [ ] **Step 3: Storage-aware reflection in `Registry`**

In `include/Astra/Registry/Registry.hpp`:

1. Add this private member to the private section, beside the Sparse funnel helpers (Task 16):

```cpp
        // Storage-aware membership and element of one REGISTERED component id (spec s3.3,
        // reflection row). Sparse/Universal members are in no archetype mask, so every
        // reflection API asks the store when the mask misses. Sets `present`; returns the
        // data pointer, or EmptyComponentSentinel() for a present tag. Table ids: nullptr.
        // A stale handle misses (both stores compare the full handle, Review Focus 1).
        void* StoredElementByID(Entity entity, ComponentID id, const ComponentDescriptor& desc, bool& present) const
        {
            present = false;
            ComponentStorage* storage = m_componentStorage.get();
            if (!storage || id >= MAX_COMPONENTS)
                return nullptr;
            switch (storage->KindOf(id))
            {
            case StorageKind::Sparse:
            {
                if (storage->SparseSlotOf(id) < 0)
                    return nullptr;                       // never stored in this registry
                SparseSet* set = storage->Sparse(id);
                if (!set || !set->Contains(entity))
                    return nullptr;
                present = true;
                return desc.size == 0 ? EmptyComponentSentinel() : set->Get(entity);
            }
            case StorageKind::Universal:
            {
                UniversalColumn* col = storage->Universal(id);
                if (!col || !col->IsLive(entity))
                    return nullptr;
                present = true;
                return desc.size == 0 ? EmptyComponentSentinel() : col->Get(entity);
            }
            case StorageKind::Table:
                break;
            }
            return nullptr;
        }
```

2. `GetComponentByHash`: replace

```cpp
            if (!record->archetype->GetMask().Test(componentId))
                return nullptr;                     // entity does not have the component
```

with

```cpp
            if (!record->archetype->GetMask().Test(componentId))
            {
                // Not in the mask: a Sparse/Universal member lives in its store (s3.3).
                bool present = false;
                return StoredElementByID(entity, componentId, *desc, present);
            }
```

3. `HasComponentByHash`: replace its final `return record->archetype->GetMask().Test(componentId);` with:

```cpp
            if (record->archetype->GetMask().Test(componentId))
                return true;
            const ComponentDescriptor* desc = m_componentRegistry->GetComponentDescriptor(componentId);
            if (!desc)
                return false;
            bool present = false;
            (void)StoredElementByID(entity, componentId, *desc, present);
            return present;
```

4. `GetEntityComponents`: replace the `ForEachComponent` lambda body with:

```cpp
                if (mask.Test(id))
                {
                    result.push_back(&desc);
                    return;
                }
                bool present = false;
                (void)StoredElementByID(entity, id, desc, present);
                if (present)
                    result.push_back(&desc);
```

5. `InspectEntity`: in the `ForEachComponent` lambda, replace `if (!mask.Test(id)) return;` with:

```cpp
                if (!mask.Test(id))
                {
                    bool present = false;
                    void* stored = StoredElementByID(entity, id, desc, present);
                    if (!present)
                        return;
                    result.push_back(ComponentInfo{ &desc, desc.meta, desc.size > 0 ? stored : nullptr });   // a tag has no data
                    return;
                }
```

`GetComponentByName` delegates to `GetComponentByHash` and needs no edit. `m_componentStorage` is a `std::shared_ptr` (Task 5), so `m_componentStorage->` reaches the non-const store accessors from this const member.

- [ ] **Step 4: Storage-aware typed helpers for `Relations`**

Add `#include "../Archetype/ArchetypeManager.hpp"`, `#include "../Core/TypeID.hpp"` and `#include <type_traits>` to `include/Astra/Registry/StorageAccess.hpp`. Then append inside `namespace Astra::Detail`:

```cpp
    // Effective kind of T in the registry owning `storage`: Universal when its Config
    // declares T (KindOf answers that from the config mask, registered or not),
    // otherwise T's trait.
    template<typename T>
    ASTRA_NODISCARD StorageKind EffectiveKindOf(const ComponentStorage* storage) noexcept
    {
        using Bare = std::remove_const_t<T>;
        if (storage && storage->KindOf(TypeID<Bare>::Value()) == StorageKind::Universal)
            return StorageKind::Universal;
        return StorageOfV<Bare> == Storage::Sparse ? StorageKind::Sparse : StorageKind::Table;
    }

    // Typed presence through the manager, any kind (Relations filters, spec s6.5).
    template<typename T>
    ASTRA_NODISCARD bool HasInStorage(ArchetypeManager& am, Entity e, StorageKind kind)
    {
        using Bare = std::remove_const_t<T>;
        ComponentStorage* storage = am.GetComponentStorage();
        const ComponentID id = TypeID<Bare>::Value();
        switch (kind)
        {
        case StorageKind::Universal:
        {
            UniversalColumn* col = storage ? storage->Universal(id) : nullptr;
            return col && col->IsLive(e);
        }
        case StorageKind::Sparse:
        {
            if (!storage || storage->SparseSlotOf(id) < 0)
                return false;
            SparseSet* set = storage->Sparse(id);
            return set && set->Contains(e);
        }
        case StorageKind::Table:
            break;
        }
        return am.HasComponent<Bare>(e);
    }

    // Typed fetch through the manager, any kind (Relations yields). A non-const T is a
    // WRITE, as on the Table arm (GetComponentMut): Universal through MarkWritten,
    // Sparse through the coarse version + the element's ticks -- the stamps
    // Registry::GetComponent applies (Tasks 14, 20). A present tag yields a static instance.
    template<typename T>
    ASTRA_NODISCARD std::remove_const_t<T>* GetInStorage(ArchetypeManager& am, Entity e, StorageKind kind)
    {
        using Bare = std::remove_const_t<T>;
        constexpr bool Write = Detail::IsMutableYield<T>;
        ComponentStorage* storage = am.GetComponentStorage();
        const ComponentID id = TypeID<Bare>::Value();
        if (kind == StorageKind::Universal)
        {
            UniversalColumn* col = storage ? storage->Universal(id) : nullptr;
            if (!col || !col->IsLive(e))
                return nullptr;
            if constexpr (std::is_empty_v<Bare>)
            {
                static Bare s_emptyInstance{};
                return &s_emptyInstance;
            }
            else
            {
                if constexpr (Write)
                    col->MarkWritten(e, am.CurrentTick());
                return static_cast<Bare*>(col->Get(e));
            }
        }
        if (kind == StorageKind::Sparse)
        {
            if (!storage || storage->SparseSlotOf(id) < 0)
                return nullptr;
            SparseSet* set = storage->Sparse(id);
            if (!set || !set->Contains(e))
                return nullptr;
            if constexpr (std::is_empty_v<Bare>)
            {
                static Bare s_emptyInstance{};
                return &s_emptyInstance;
            }
            else
            {
                if constexpr (Write)
                {
                    const Tick now = am.CurrentTick();
                    storage->StampCoarse(id, now);
                    if (EntityTicks* t = SparseTicksOf(*set, e))
                        t->changed = now;
                }
                return static_cast<Bare*>(set->Get(e));
            }
        }
        if constexpr (Write)
            return am.template GetComponentMut<Bare>(e);
        else
            return am.template GetComponent<Bare>(e);
    }
```

- [ ] **Step 5: Route `Relations` through the helpers**

In `include/Astra/Registry/Relations.hpp`:

1. Add `#include <array>` and `#include "StorageAccess.hpp"`.
2. After the `static_assert` on `WithComponents`, add the per-object plan:

```cpp
        // Storage policies (spec 2026-10-01 s6.5): effective kind of every named type,
        // resolved once per Relations object (Universal is a per-registry fact). The
        // manager's typed HasComponent/GetComponent see Table columns only, so filters and
        // yields dispatch on this. Store POINTERS are never cached (module unload, Task 36).
        using PlanTypes = Detail::QueryPlanTypes<QueryArgs...>;
        static constexpr size_t NPlanSlots = std::tuple_size_v<PlanTypes> == 0 ? 1 : std::tuple_size_v<PlanTypes>;
        std::array<StorageKind, NPlanSlots> m_kinds{};

        template<typename T>
        ASTRA_NODISCARD StorageKind KindOfT() const noexcept
        {
            return m_kinds[Detail::TupleIndexOf<std::remove_const_t<T>, PlanTypes>::value];
        }

        void ResolveKinds() noexcept
        {
            m_kinds.fill(StorageKind::Table);
            const ComponentStorage* storage = m_archetypeManager ? m_archetypeManager->GetComponentStorage() : nullptr;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((m_kinds[Detail::TupleIndexOf<Ts, PlanTypes>::value] = Detail::EffectiveKindOf<Ts>(storage)), ...);
            }(static_cast<PlanTypes*>(nullptr));
        }
```

   `m_kinds` is a data member. If the class declares members only at the bottom (it does: `m_archetypeManager` and the others), move the `std::array<StorageKind, NPlanSlots> m_kinds{};` line there, after `m_scheduler`. Keep the aliases and the two functions here.

3. In the constructor body (currently `{}`), call `ResolveKinds();`.
4. Replace `FetchYield`'s body with:

```cpp
            return Detail::GetInStorage<C>(*m_archetypeManager, entity, KindOfT<C>());
```

   The doc comment above it stays true. A non-const request is still a write for every kind: Table through `GetComponentMut`, Sparse/Universal through their store stamps.

5. Replace the `m_archetypeManager->HasComponent<Ts>(entity)` calls with `Detail::HasInStorage<Ts>(*m_archetypeManager, entity, KindOfT<Ts>())`. There are four sites: `HasAllRequired` (`&&` fold), `HasAnyExcluded` (`||` fold), `HasAnyInGroup` (`||` fold) and `CountInGroup` (`? 1 : 0` fold).

- [ ] **Step 6: Run the tests**

Build, then `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageReflection.*:Relations*:RegistryTest.*:Reflection*:IterationSafety.*`
Expected: all PASS, including the existing `RegistryTest.GetComponentByHashTagAgreesWithHas` and every `Relations*` test.

Full run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`. Expected: all PASS. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp include/Astra/Registry/StorageAccess.hpp include/Astra/Registry/Relations.hpp tests/Registry/StorageReflectionTest.cpp
git commit -m "feat(storage): reflection APIs (GetComponentByHash/ByName, HasComponentByHash, GetEntityComponents, InspectEntity) include Sparse and Universal members; Relations filters and yields dispatch on a per-object storage plan (s3.3, s6.5)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---


## Chunk E: serialization + module unload (Tasks 31-36)

Covers spec s8.1-s8.3 (format v6, the ComponentStorage block, storage-agnostic load in both directions, v5 compatibility) and s4.4 (module-unload teardown).

**Shape of the chunk**
- Task 31: the v6 writer. `BINARY_FORMAT_VERSION` 6, `ComponentStorage::Serialize`, the new Save order, and a reader stub that accepts only an empty block.
- Task 32: the block reader for Sparse/Universal-now components, the post-load fix-ups (declared Universal but absent -> default + `init`; chunk presence derived from the stores), and refusal of corrupt blocks.
- Task 33: Table-in-archive, now Sparse/Universal. The column is diverted out of `Archetype::Deserialize`; archetypes whose masks collapse are merged. Pins Review Focus 4 (LZ4) and v5 archives.
- Task 34: Sparse/Universal-in-archive, now Table. Values are staged, then applied by `ApplyMigratedTableComponents` (one batched move per destination archetype).
- Task 35: the unload mechanism. `ComponentRegistry` store-release hooks fire before any slot changes; `ComponentStorage::ReleaseComponent`.
- Task 36: `Registry` subscribes to those hooks and tears its stores down, using Chunk C's `ArchetypeChunk::ReleaseSparseBlock` for the chunk side. Pins Review Focus 5.

**Test-only storage override.** A component's Table/Sparse class is a compile-time trait. So one C++ type cannot be Table when saved and Sparse when loaded inside one test binary, unless its descriptor is overridden.
- `tests/Support/StorageOverride.hpp` (Task 31) installs a registry-built descriptor with `storage` rewritten through `ComponentRegistry::InstallOwned`. Every load path is hash- and descriptor-driven, so this reproduces a real build change exactly.
- An overridden type is driven only through by-ID and by-hash APIs (`AddComponentByID`, `GetComponentByHash`, `HasComponentByHash`, `SetEnabledByID`/`IsEnabledByID`) and `ValidateStorageInvariants()`. The typed API dispatches on the trait.
- No new test component types are minted (TypeID budget, `tests/TestComponents.hpp`). The chunk reuses `Position`, `Name`, `Health`, `Hierarchy` (enableable), `Player` (tag) and `RelocationCanary` (live counter).

**Names this chunk relies on from Chunks A-D** (see the notes at the end):
- Confirmed by the coordinator:
  - `Registry::m_componentStorage` is a `std::shared_ptr<ComponentStorage>`, rebuilt from the caller's `Config` in `LoadInternal` (Chunk A Task 5).
  - `UniversalSpec` is at namespace scope in `ComponentStorage.hpp`, with fields `id`, `init`, `user` and `registerComponent`.
  - Chunk C provides `ArchetypeChunk::ReleaseSparseBlock`.
  - Enableable Table tags have bit-only columns, so test `desc.size`, never `GetComponentArrayByID`.
  - The entity index accessor is `Entity::GetID()`.
  - Ticks and coarse versions are not persisted.
- Assumed:
  - `Registry::GetComponentStorage()` exists (Task 5; returns `ComponentStorage*`).
  - `ComponentStorage::KindOf` reads `ComponentDescriptor::storage` from the `ComponentRegistry` plus the universal list.
  - `AcquireSparseSlot` is idempotent.

---
### Task 31: Format v6, the ComponentStorage block writer, and the Save order

**Files:**
- Create: `include/Astra/Serialization/StorageMigration.hpp` (format constants + disabled-bitmap I/O; extended in Tasks 33-34)
- Create: `include/Astra/Registry/ComponentStorageSerialization.hpp` (`ComponentStorage::Serialize`; Task 32 adds the load helpers)
- Modify: `include/Astra/Serialization/BinaryArchive.hpp:78-97` (version history + constant)
- Modify: `include/Astra/Registry/ComponentStorage.hpp` (drop any stub bodies of `Serialize`/`Deserialize`, keep the declarations)
- Modify: `include/Astra/Registry/Registry.hpp` (include, both `Save` overloads, `LoadInternal`, the out-of-line `ComponentStorage::Deserialize` stub after the class)
- Create: `tests/Support/StorageOverride.hpp`
- Test: `tests/Serialization/ComponentStorageFormatTest.cpp` (new file: run premake)

**Interfaces:**
- Consumes (Chunks A-C): `ComponentStorage::{KindOf, SparseSlotOf, Sparse, Universal}`, `SparseSet::{Size, Entities, DataAt, IsDisabled, Descriptor}`, `UniversalColumn::{LiveCount, ForEachLive, IsDisabled, Descriptor}`, `Registry::Config::AddUniversal`, `ComponentDescriptor::storage`.
- Produces: `BINARY_FORMAT_VERSION == 6`; `ComponentStorage::Serialize(BinaryWriter&) const` (canonical); `namespace Astra::StorageFormat { KIND_SPARSE=1, KIND_UNIVERSAL=2, FLAG_HAS_DISABLED=0x01, FLAG_HAS_VALUES=0x02, FLAG_MASK, BLOCK_HEADER_BYTES=18, DisabledWordCount, WriteDisabledBitmap, ReadDisabledBitmap }`; `Astra::TestSupport::{ScopedDefaultTypeContext, InstallWithStorage<T>}`.

**Block layout (spec s8.2, v6)**

```
uint32 storedCount
per stored component (ascending hash):
  uint64 hash; uint32 version; uint8 kind (1 Sparse | 2 Universal); uint8 flags (0x01 hasDisabled, 0x02 hasValues); uint32 elementCount
  payload: inline (None) or one WriteCompressedBlock frame (LZ4):
    elementCount x Entity (ascending GetID())
    if hasValues:   elementCount x serializeVersioned
    if hasDisabled: uint32 popcount + DisabledWordCount(elementCount) x uint64
```

How this differs from the spec text:
- `version` is `uint32`, not `uint16`. It matches `ComponentDescriptor::version` and the archetype descriptor record.
- `hasValues` is recorded explicitly, so a tag block carries presence only. That is the IM-9 rule: the archive says what it carries.
- Empty stores are not written.
- Ticks and chunk presence blocks are not written (s8.2).

- [ ] **Step 1: Write the test-support header and the failing tests**

`tests/Support/StorageOverride.hpp`:

```cpp
#pragma once

#include <memory>
#include <string>
#include <gtest/gtest.h>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>

namespace Astra::TestSupport
{
    // ComponentRegistry::InstallOwned refuses a foreign TypeContext; install the
    // module default for the scope and restore whatever slot was there before
    // (ComponentModule tests assert the slot starts null).
    class ScopedDefaultTypeContext
    {
    public:
        ScopedDefaultTypeContext() : m_previous(Astra::Detail::CurrentTypeContextSlot())
        {
            Astra::SetTypeContext(&Astra::DefaultTypeContext());
        }
        ~ScopedDefaultTypeContext() { Astra::SetTypeContext(m_previous); }
        ScopedDefaultTypeContext(const ScopedDefaultTypeContext&) = delete;
        ScopedDefaultTypeContext& operator=(const ScopedDefaultTypeContext&) = delete;

    private:
        Astra::TypeContext* m_previous;
    };

    // Installs T's registry-built descriptor with `storage` overridden, so ONE
    // C++ type can be Table in one ComponentRegistry and Sparse in another: the
    // only way a single test binary reproduces "the storage class changed between
    // save and load" (spec s8.3). The trait is unchanged, so tests drive an
    // overridden type only through by-ID / by-hash APIs, never the typed API.
    // Call it BEFORE constructing a Registry over `creg` or loading into it.
    template<typename T>
    inline Astra::ComponentID InstallWithStorage(Astra::ComponentRegistry& creg, Astra::Storage storage)
    {
        static int s_image = 0;   // stand-in module token, one per T
        const Astra::ComponentID id = Astra::TypeID<T>::Value();
        Astra::ComponentDescriptor desc = Astra::ComponentRegistry::MakeDescriptor<T>(id, nullptr);
        desc.storage = storage;
        const std::string name(Astra::TypeID<T>::Name());
        desc.name = name.c_str();   // InstallOwned copies it before returning
        const uint32_t owner = creg.OpenModuleId("StorageOverride");
        EXPECT_NE(creg.InstallOwned(id, owner, desc, &s_image), Astra::InstallResult::Refused);
        return id;
    }
}
```

`tests/Serialization/ComponentStorageFormatTest.cpp`:

```cpp
#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../TestComponents.hpp"
#include "../Support/StorageOverride.hpp"
#include "Astra/Registry/Registry.hpp"

namespace
{
    using namespace Astra::Test;

    struct BlockHeader { uint64_t hash; uint32_t version; uint8_t kind; uint8_t flags; uint32_t count; };

    BlockHeader ReadBlockHeader(Astra::BinaryReader& r)
    {
        BlockHeader h{};
        r(h.hash)(h.version)(h.kind)(h.flags)(h.count);
        return h;
    }

    // Five entities with Position; Health universal (registry Config); Hierarchy
    // Sparse through a storage override, added out of id order so the dense order
    // differs from the ascending order the block must use.
    struct FormatWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::ComponentID hierId = 0;
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> e;

        FormatWorld()
        {
            creg->RegisterComponents<Position, Health>();
            hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
            Astra::Registry::Config cfg;
            cfg.AddUniversal<Health>();
            reg = std::make_unique<Astra::Registry>(creg, cfg);
            for (int i = 0; i < 5; ++i) e.push_back(reg->CreateEntity<Position>());
            reg->GetComponent<Health>(e[2])->current = 7;
            reg->GetComponent<Health>(e[2])->max = 9;
            for (int i : {4, 1, 3})
            {
                Hierarchy h{};
                h.parent = 100u + static_cast<uint32_t>(i);
                EXPECT_TRUE(reg->AddComponentByID(e[i], hierId, &h, sizeof(h)));
            }
            EXPECT_TRUE(reg->SetEnabledByID(e[3], hierId, false));
        }
    };

    std::vector<Astra::Entity> AscendingById(std::vector<Astra::Entity> v)
    {
        std::sort(v.begin(), v.end(), [](Astra::Entity a, Astra::Entity b) { return a.GetID() < b.GetID(); });
        return v;
    }
}

TEST(ComponentStorageFormat, BlockIsSortedByHashWithAscendingEntitiesValuesAndDisabledBitmap)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    FormatWorld world;

    std::vector<std::byte> bytes;
    {
        Astra::BinaryWriter w(bytes);   // memory mode, CompressionMode::None
        world.reg->GetComponentStorage()->Serialize(w);
        ASSERT_FALSE(w.HasError());
    }

    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    uint32_t stored = 0;
    r(stored);
    ASSERT_EQ(stored, 2u);

    const uint64_t hierHash = Astra::TypeID<Hierarchy>::Hash();
    const uint64_t healthHash = Astra::TypeID<Health>::Hash();
    uint64_t previous = 0;
    for (uint32_t b = 0; b < stored; ++b)
    {
        const BlockHeader h = ReadBlockHeader(r);
        ASSERT_FALSE(r.HasError());
        if (b > 0) EXPECT_GT(h.hash, previous) << "blocks must be sorted by hash";
        previous = h.hash;
        EXPECT_EQ(h.version, 1u);

        if (h.hash == hierHash)
        {
            EXPECT_EQ(h.kind, Astra::StorageFormat::KIND_SPARSE);
            EXPECT_EQ(h.flags, Astra::StorageFormat::FLAG_HAS_VALUES | Astra::StorageFormat::FLAG_HAS_DISABLED);
            ASSERT_EQ(h.count, 3u);
            const auto expected = AscendingById({world.e[4], world.e[1], world.e[3]});
            for (uint32_t i = 0; i < 3; ++i) { Astra::Entity got; r(got); EXPECT_EQ(got, expected[i]); }
            for (uint32_t i = 0; i < 3; ++i)
            {
                Hierarchy v{};
                ASSERT_TRUE(r.ReadVersionedComponent(v).IsOk());
                const uint32_t ordinal = (expected[i] == world.e[1]) ? 1u : (expected[i] == world.e[3]) ? 3u : 4u;
                EXPECT_EQ(v.parent, 100u + ordinal);
            }
            uint32_t disabledCount = 0;
            uint64_t word = 0;
            r(disabledCount)(word);
            EXPECT_EQ(disabledCount, 1u);
            const size_t e3 = static_cast<size_t>(std::find(expected.begin(), expected.end(), world.e[3]) - expected.begin());
            EXPECT_EQ(word, uint64_t{1} << e3);
        }
        else
        {
            ASSERT_EQ(h.hash, healthHash);
            EXPECT_EQ(h.kind, Astra::StorageFormat::KIND_UNIVERSAL);
            EXPECT_EQ(h.flags, Astra::StorageFormat::FLAG_HAS_VALUES);   // Health is not enableable
            ASSERT_EQ(h.count, 5u);
            const auto expected = AscendingById(world.e);
            for (uint32_t i = 0; i < 5; ++i) { Astra::Entity got; r(got); EXPECT_EQ(got, expected[i]); }
            for (uint32_t i = 0; i < 5; ++i)
            {
                Health v{};
                ASSERT_TRUE(r.ReadVersionedComponent(v).IsOk());
                EXPECT_EQ(v.current, expected[i] == world.e[2] ? 7 : 100);
            }
        }
    }
    EXPECT_EQ(r.GetRemaining(), 0u);
}
```

Append to the same file:

```cpp
// s8.1: header, EntityManager, ArchetypeManager, ComponentStorage, RelationshipGraph,
// ResourceStorage, checksum. Parse a None-mode Save up to the block and check it
// sits right after the archetypes.
TEST(ComponentStorageFormat, SaveWritesTheBlockBetweenArchetypesAndRelationsAtVersion6)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    FormatWorld world;
    Astra::Registry::SaveConfig sc;
    sc.compressionMode = Astra::CompressionMode::None;
    auto saved = world.reg->Save(sc);
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte>& bytes = *saved.GetValue();

    uint16_t version = 0;
    std::memcpy(&version, bytes.data() + 5, sizeof(version));   // BinaryHeader: magic[5], then version
    EXPECT_EQ(version, 6u);
    EXPECT_EQ(Astra::BINARY_FORMAT_VERSION, 6u);

    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    ASSERT_TRUE(r.ReadHeader().IsOk());
    auto em = Astra::EntityManager::Deserialize(r);
    ASSERT_TRUE(em.IsOk());
    Astra::ArchetypeManager scratch(world.creg, Astra::ArchetypeChunkPool::Config{}, &(*em.GetValue())->GetRecordTable());
    ASSERT_TRUE(scratch.Deserialize(r));

    uint32_t stored = 0;
    r(stored);
    EXPECT_EQ(stored, 2u);
    uint64_t firstHash = 0;
    r(firstHash);
    EXPECT_EQ(firstHash, std::min(Astra::TypeID<Hierarchy>::Hash(), Astra::TypeID<Health>::Hash()));
}
```

- [ ] **Step 2: Regenerate, build, run, and watch it fail**

The new test file needs premake. From `D:\dev\starworks\Astra-storage` in PowerShell:

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: a compile error (error C2039): `'Serialize': is not a member of 'Astra::ComponentStorage'` (no earlier task declares it), and/or `'StorageFormat': is not a member of 'Astra'`.

- [ ] **Step 3: Implement**

`include/Astra/Serialization/BinaryArchive.hpp`: append to the history comment, after the v5 paragraph, and bump the constant.

```cpp
     * v6: ComponentStorage block between the archetype records and the
     *     relationship graph (storage policies, spec 2026-10-01 s8): one entry
     *     per non-empty Sparse set / Universal column, sorted by type hash --
     *     {hash, version, kind, flags, count} then entities (ascending id),
     *     values (per-element serializeVersioned), disabled bitmap; the payload
     *     is one WriteCompressedBlock frame per component under LZ4. Archetype
     *     records are unchanged. Loads are storage-agnostic: v5 and older
     *     archives (no block, every component a Table column) load unchanged.
     */
    inline constexpr uint16_t BINARY_FORMAT_VERSION = 6;
```

(The snippet replaces the comment's closing `*/` line and the `= 5` constant.)

`include/Astra/Serialization/StorageMigration.hpp` (new):

```cpp
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "BinaryReader.hpp"
#include "BinaryWriter.hpp"

namespace Astra::StorageFormat
{
    // Format v6 ComponentStorage block (spec 2026-10-01 s8.2).
    inline constexpr uint8_t KIND_SPARSE    = 1;
    inline constexpr uint8_t KIND_UNIVERSAL = 2;

    // The flags byte records what the archive CARRIES (the IM-9 rule), so a reader
    // never infers stream layout from its own build's descriptor.
    inline constexpr uint8_t FLAG_HAS_DISABLED = 0x01;   // a disabled bitmap follows the values
    inline constexpr uint8_t FLAG_HAS_VALUES   = 0x02;   // per-element serializeVersioned values (absent for tags)
    inline constexpr uint8_t FLAG_MASK         = FLAG_HAS_DISABLED | FLAG_HAS_VALUES;

    // hash(8) + version(4) + kind(1) + flags(1) + elementCount(4)
    inline constexpr size_t BLOCK_HEADER_BYTES = 18;

    // The same section shape Archetype::SerializeColumn writes for an enableable
    // column: uint32 popcount, then max(1, count) bits rounded up to whole words.
    ASTRA_NODISCARD inline size_t DisabledWordCount(size_t count) noexcept
    {
        return (std::max<size_t>(1, count) + 63) / 64;
    }

    inline void WriteDisabledBitmap(BinaryWriter& w, const std::vector<uint8_t>& disabled)
    {
        std::vector<uint64_t> words(DisabledWordCount(disabled.size()), 0);
        uint32_t count = 0;
        for (size_t i = 0; i < disabled.size(); ++i)
        {
            if (!disabled[i]) continue;
            words[i >> 6] |= uint64_t{1} << (i & 63);
            ++count;
        }
        w(count);
        for (const uint64_t word : words) w(word);
    }

    // Refuse-not-trust, the same rule DeserializeColumn applies: the popcount must
    // match, and no bit may sit at or beyond `count`.
    ASTRA_NODISCARD inline bool ReadDisabledBitmap(BinaryReader& r, size_t count, std::vector<uint8_t>& out)
    {
        const size_t wordCount = DisabledWordCount(count);
        uint32_t diskCount = 0;
        r(diskCount);
        if (r.HasError() || r.CountExceedsRemaining(wordCount, sizeof(uint64_t)))
            return false;

        out.assign(count, 0);
        uint32_t popcount = 0;
        for (size_t w = 0; w < wordCount; ++w)
        {
            uint64_t word = 0;
            r(word);
            if (r.HasError())
                return false;
            popcount += static_cast<uint32_t>(std::popcount(word));
            for (uint64_t bits = word; bits != 0; bits &= bits - 1)
            {
                const size_t i = w * 64 + static_cast<size_t>(std::countr_zero(bits));
                if (i >= count)
                    return false;   // a bit beyond the live element count
                out[i] = 1;
            }
        }
        return popcount == diskCount;
    }
}
```

`include/Astra/Registry/ComponentStorageSerialization.hpp` (new):

```cpp
#pragma once

#include <algorithm>
#include <numeric>
#include <span>
#include <vector>

#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/StorageMigration.hpp"
#include "ComponentStorage.hpp"

namespace Astra
{
    namespace Detail
    {
        // One non-empty store, flattened into the block's element order.
        struct StoredComponent
        {
            const ComponentDescriptor* desc = nullptr;
            uint8_t kind = 0;
            std::vector<Entity> entities;     // ascending GetID()
            std::vector<void*> values;        // parallel to entities
            std::vector<uint8_t> disabled;    // parallel to entities
        };

        inline void WriteStoredPayload(BinaryWriter& w, const StoredComponent& c, uint8_t flags)
        {
            for (const Entity e : c.entities) w(e);
            if (flags & StorageFormat::FLAG_HAS_VALUES)
                for (void* value : c.values) c.desc->serializeVersioned(w, value);
            if (flags & StorageFormat::FLAG_HAS_DISABLED)
                StorageFormat::WriteDisabledBitmap(w, c.disabled);
        }
    }

    // Format v6 ComponentStorage block (spec s8.2; layout in BinaryArchive.hpp's
    // v6 note). Deterministic: blocks sorted by hash, elements by entity id.
    inline void ComponentStorage::Serialize(BinaryWriter& w) const
    {
        // Sparse()/Universal() are the lazily-creating accessors. They are only
        // reached for ids this storage already holds (a sparse slot exists, or the
        // id is declared universal), so a save never creates a store.
        auto& self = const_cast<ComponentStorage&>(*this);
        std::vector<Detail::StoredComponent> stored;

        for (size_t raw = 0; raw < MAX_COMPONENTS; ++raw)
        {
            const auto id = static_cast<ComponentID>(raw);
            const StorageKind kind = KindOf(id);
            if (kind == StorageKind::Sparse)
            {
                if (SparseSlotOf(id) < 0) continue;
                SparseSet* set = self.Sparse(id);
                if (!set || set->Size() == 0) continue;

                Detail::StoredComponent c;
                c.desc = &set->Descriptor();
                c.kind = StorageFormat::KIND_SPARSE;
                const Entity* dense = set->Entities();
                std::vector<size_t> order(set->Size());
                std::iota(order.begin(), order.end(), size_t{0});
                std::sort(order.begin(), order.end(),
                          [dense](size_t a, size_t b) { return dense[a].GetID() < dense[b].GetID(); });
                for (const size_t d : order)
                {
                    c.entities.push_back(dense[d]);
                    c.values.push_back(set->DataAt(d));
                    c.disabled.push_back(set->IsDisabled(dense[d]) ? 1 : 0);
                }
                stored.push_back(std::move(c));
            }
            else if (kind == StorageKind::Universal)
            {
                UniversalColumn* column = self.Universal(id);
                if (!column || column->LiveCount() == 0) continue;

                Detail::StoredComponent c;
                c.desc = &column->Descriptor();
                c.kind = StorageFormat::KIND_UNIVERSAL;
                column->ForEachLive([&](Entity e, const void* value)   // ascending index
                {
                    c.entities.push_back(e);
                    c.values.push_back(const_cast<void*>(value));
                    c.disabled.push_back(column->IsDisabled(e) ? 1 : 0);
                });
                stored.push_back(std::move(c));
            }
        }

        std::sort(stored.begin(), stored.end(),
                  [](const Detail::StoredComponent& a, const Detail::StoredComponent& b) { return a.desc->hash < b.desc->hash; });

        w(static_cast<uint32_t>(stored.size()));
        for (const Detail::StoredComponent& c : stored)
        {
            const ComponentDescriptor& desc = *c.desc;
            uint8_t flags = 0;
            if (desc.size > 0)      flags |= StorageFormat::FLAG_HAS_VALUES;
            if (desc.isEnableable)  flags |= StorageFormat::FLAG_HAS_DISABLED;

            w(desc.hash);
            w(desc.version);
            w(c.kind);
            w(flags);
            w(static_cast<uint32_t>(c.entities.size()));

            if (w.GetCompressionMode() == CompressionMode::LZ4)
            {
                // Per-component block, the per-column precedent (Archetype::Serialize):
                // plain bytes into a checksum-off sub-writer, compressed once here.
                std::vector<std::byte> payload;
                {
                    BinaryWriter sub(payload);
                    sub.SetChecksumEnabled(false);
                    Detail::WriteStoredPayload(sub, c, flags);
                    sub.Flush();
                }
                w.WriteCompressedBlock(payload.data(), payload.size());
            }
            else
            {
                Detail::WriteStoredPayload(w, c, flags);
            }
        }
    }
}
```

`include/Astra/Registry/ComponentStorage.hpp`:
- No earlier task declares `Serialize`/`Deserialize`. Add these canonical DECLARATIONS to the public section (declaration only: `Serialize` is defined in `ComponentStorageSerialization.hpp`, and `Deserialize` out of line after `class Registry` in `Registry.hpp`, step 5 below, because its body needs the complete `Registry`):
  ```cpp
  void Serialize(BinaryWriter& w) const;
  bool Deserialize(BinaryReader& r, class Registry& registry);
  ```
- Forward-declare `class BinaryWriter; class BinaryReader; class Registry;` in `namespace Astra` at the top of the header (or include `../Serialization/BinaryArchive.hpp`) so the declarations compile.

`include/Astra/Registry/Registry.hpp`:

1. Includes. Directly after the existing `#include "ComponentStorage.hpp"` (Chunk A), add:
   ```cpp
   #include "ComponentStorageSerialization.hpp"
   ```

2. Accessor. `Registry::GetComponentStorage()` already exists (Task 5) and returns `ComponentStorage*`. Add nothing; tests reach the store as `reg.GetComponentStorage()->...`.

3. Save order (s8.1). In BOTH `Save` overloads, replace
   ```cpp
               m_archetypeManager->Serialize(writer);
               m_relationshipGraph->Serialize(writer);
   ```
   with
   ```cpp
               m_archetypeManager->Serialize(writer);
               m_componentStorage->Serialize(writer);   // format v6 ComponentStorage block (s8.1)
               m_relationshipGraph->Serialize(writer);
   ```

4. `LoadInternal`. Directly after the `if (!registry->m_archetypeManager->Deserialize(reader)) { ... }` block, add:
   ```cpp
               // Format v6 (s8.1): the ComponentStorage block sits between the
               // archetype records and the relationship graph. v5 and older have none.
               if (reader.GetVersion() >= 6)
               {
                   if (!registry->m_componentStorage->Deserialize(reader, *registry))
                   {
                       return Result<std::unique_ptr<Registry>, SerializationError>::Err(
                           reader.HasError() ? reader.GetError() : SerializationError::UnknownComponent);
                   }
               }
   ```

5. The reader stub. At the end of the file, between the closing `};` of `class Registry` and the namespace's closing `}`, add the out-of-line definition. It lives here, not in `ComponentStorageSerialization.hpp`, because Task 32's body needs the complete `Registry` (`IsValid`, `GetComponentRegistry`).
   ```cpp
       // Format v6 block reader. Task 31 accepts only an empty block (every
       // registry before this chunk could hold no stored components in an
       // archive); Task 32 replaces this with the real reader.
       inline bool ComponentStorage::Deserialize(BinaryReader& r, Registry& registry)
       {
           (void)registry;
           uint32_t stored = 0;
           r(stored);
           return !r.HasError() && stored == 0;
       }
   ```

- [ ] **Step 4: Build and run the new tests, then the full suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ComponentStorageFormat.*:FormatV2.*:LoadRobustness.*:RegistryTest.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- `[  PASSED  ]` for the filter. The two new tests pass. `FormatV2.HeaderVersionMatchesCurrentFormatVersion` passes against 6. The Table-only round trips pass, because their block is `storedCount = 0`.
- The full run is green. Record the `Randomizing tests' orders with a seed of N` line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Serialization/BinaryArchive.hpp include/Astra/Serialization/StorageMigration.hpp include/Astra/Registry/ComponentStorageSerialization.hpp include/Astra/Registry/ComponentStorage.hpp include/Astra/Registry/Registry.hpp tests/Support/StorageOverride.hpp tests/Serialization/ComponentStorageFormatTest.cpp
git commit -m @'
feat(storage): format v6 -- the ComponentStorage block (sorted by hash; entities ascending, per-element versioned values, disabled bitmap; one LZ4 block per component) written between the archetypes and the relationship graph

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---
### Task 32: Block reader for components now Sparse/Universal, universal defaults on load, derived presence, and refusals

**Files:**
- Modify: `include/Astra/Registry/ComponentStorageSerialization.hpp` (add `Detail::AcquireLoadTarget`, `Detail::SetLoadedDisabled`)
- Modify: `include/Astra/Registry/Registry.hpp` (replace the Task 31 stub with the real `ComponentStorage::Deserialize` + `Detail::ReadStoredPayload`; private `FinishStorageLoad`, `FillMissingUniversals`, `DeriveStoragePresence`; call `FinishStorageLoad()` in `LoadInternal`)
- Test: `tests/Serialization/ComponentStorageFormatTest.cpp` (append)

**Interfaces:**
- Consumes: Task 31 (`StorageFormat::*`, `ComponentStorage::Serialize`); `ComponentStorage::{KindOf, AcquireSparseSlot, SparseSlotOf, Sparse, Universal}`; `SparseSet::{Contains, Get, Insert, SetDisabled, IsDisabled, Entities, Size}`; `UniversalColumn::{IsLive, Get, Construct, SetDisabled, IsDisabled, ForEachLive, Descriptor}`; `ArchetypeChunk::{SetSparsePresent, SetSparseDisabled}`; `Registry::Config::universal` (`UniversalSpec{id, init, user}`); `Registry::ValidateStorageInvariants()`.
- Produces:
  - `bool ComponentStorage::Deserialize(BinaryReader&, Registry&)` (canonical), real for Sparse- and Universal-now components. Table-now refuses until Task 34.
  - `void* Detail::AcquireLoadTarget(ComponentStorage&, const ComponentDescriptor&, Entity)`: returns `EmptyComponentSentinel()` for a tag and `nullptr` to refuse.
  - `void Detail::SetLoadedDisabled(ComponentStorage&, ComponentID, Entity)`.
  - Private `bool Registry::FinishStorageLoad()`, called after the checksum in `LoadInternal`. It fills Universal components that are declared but absent (default construction, then `init`, s8.3 bullet 3), then derives the chunk presence and disabled mirror from the stores (s8.2: presence is never serialized).

- [ ] **Step 1: Write the failing tests** (append to `tests/Serialization/ComponentStorageFormatTest.cpp`)

```cpp
namespace
{
    void MintName(Astra::Entity e, void* component, void* user)
    {
        static_cast<Name*>(component)->value = "minted-" + std::to_string(e.GetID());
        ++*static_cast<int*>(user);
    }

    // Position + Name registered; Hierarchy (enableable) and Player (tag) Sparse.
    struct StorageWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::ComponentID hierId = 0;
        Astra::ComponentID playerId = 0;
        StorageWorld()
        {
            creg->RegisterComponents<Position, Name>();
            hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
            playerId = Astra::TestSupport::InstallWithStorage<Player>(*creg, Astra::Storage::Sparse);
        }
    };

    Astra::Registry::Config UniversalNameConfig()
    {
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>();
        return cfg;
    }

    void WriteBlockHeader(Astra::BinaryWriter& w, uint64_t hash, uint8_t kind, uint8_t flags, uint32_t count)
    {
        w(hash);
        w(uint32_t{1});   // descriptor version
        w(kind);
        w(flags);
        w(count);
    }
}

TEST(ComponentStorageFormat, SparseUniversalAndSparseTagRoundTripInBothCompressionModes)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        StorageWorld save;
        Astra::Registry reg(save.creg, UniversalNameConfig());
        std::vector<Astra::Entity> es;
        for (int i = 0; i < 70; ++i)   // > 64: the disabled bitmap spans two words
        {
            const Astra::Entity e = reg.CreateEntity<Position>();
            reg.GetComponent<Name>(e)->value = "name-" + std::to_string(i);
            if (i % 3 == 0)
            {
                Hierarchy h{};
                h.parent = static_cast<uint32_t>(i);
                ASSERT_TRUE(reg.AddComponentByID(e, save.hierId, &h, sizeof(h)));
            }
            if (i % 6 == 0) ASSERT_TRUE(reg.SetEnabledByID(e, save.hierId, false));
            if (i % 5 == 0)
            {
                Player tag{};
                ASSERT_TRUE(reg.AddComponentByID(e, save.playerId, &tag, sizeof(tag)));
            }
            es.push_back(e);
        }
        Astra::Registry::SaveConfig sc;
        sc.compressionMode = mode;
        auto saved = reg.Save(sc);
        ASSERT_TRUE(saved.IsOk());

        StorageWorld load;
        auto loaded = Astra::Registry::Load(*saved.GetValue(), load.creg, UniversalNameConfig());
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& out = **loaded.GetValue();
        EXPECT_EQ(out.ValidateStorageInvariants(), "");

        const uint64_t hierHash = Astra::TypeID<Hierarchy>::Hash();
        const uint64_t playerHash = Astra::TypeID<Player>::Hash();
        for (int i = 0; i < 70; ++i)
        {
            const Astra::Entity e = es[i];
            ASSERT_TRUE(out.IsValid(e));
            ASSERT_NE(out.GetComponent<Name>(e), nullptr);
            EXPECT_EQ(out.GetComponent<Name>(e)->value, "name-" + std::to_string(i));
            ASSERT_EQ(out.HasComponentByHash(e, hierHash), i % 3 == 0) << "entity " << i;
            if (i % 3 == 0)
            {
                const auto* h = static_cast<const Hierarchy*>(out.GetComponentByHash(e, hierHash));
                ASSERT_NE(h, nullptr);
                EXPECT_EQ(h->parent, static_cast<uint32_t>(i));
                EXPECT_EQ(out.IsEnabledByID(e, load.hierId), i % 6 != 0) << "entity " << i;
            }
            EXPECT_EQ(out.HasComponentByHash(e, playerHash), i % 5 == 0) << "entity " << i;
        }
    }
}
```

```cpp
// s8.3 bullet 3: Name is declared Universal by the loading registry, and the archive
// never held it. Every entity gets default construction and then the registry's init.
TEST(ComponentStorageFormat, DeclaredUniversalAbsentFromTheArchiveIsDefaultedThenInitialised)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Name>();
    Astra::Registry reg(saveCreg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 10; ++i) es.push_back(reg.CreateEntity<Position>());
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loadCreg = std::make_shared<Astra::ComponentRegistry>();
    loadCreg->RegisterComponents<Position, Name>();
    int inits = 0;
    Astra::Registry::Config cfg;
    cfg.AddUniversal<Name>(&MintName, &inits);
    auto loaded = Astra::Registry::Load(*saved.GetValue(), loadCreg, cfg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(inits, 10);
    for (const Astra::Entity e : es)
    {
        ASSERT_NE(out.GetComponent<Name>(e), nullptr);
        EXPECT_EQ(out.GetComponent<Name>(e)->value, "minted-" + std::to_string(e.GetID()));
    }
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}

TEST(ComponentStorageFormat, BlockForAnUnregisteredComponentIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld save;
    Astra::Registry reg(save.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Hierarchy h{};
    ASSERT_TRUE(reg.AddComponentByID(e, save.hierId, &h, sizeof(h)));
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loadCreg = std::make_shared<Astra::ComponentRegistry>();
    loadCreg->RegisterComponents<Position, Name>();   // no Hierarchy
    EXPECT_TRUE(Astra::Registry::Load(*saved.GetValue(), loadCreg).IsErr());
}

TEST(ComponentStorageFormat, BlockNamingADeadEntityIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity live = reg.CreateEntity<Position>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        w(uint32_t{1});
        WriteBlockHeader(w, Astra::TypeID<Player>::Hash(), Astra::StorageFormat::KIND_SPARSE, 0, 1);
        w(Astra::Entity(live.GetID() + 1000, 1));   // never created in `reg`
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg));
}

TEST(ComponentStorageFormat, BlocksOutOfHashOrderAreRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    const uint64_t hierHash = Astra::TypeID<Hierarchy>::Hash();
    const uint64_t playerHash = Astra::TypeID<Player>::Hash();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        auto writeOne = [&](uint64_t hash)
        {
            if (hash == hierHash)
            {
                WriteBlockHeader(w, hash, Astra::StorageFormat::KIND_SPARSE,
                                 Astra::StorageFormat::FLAG_HAS_VALUES | Astra::StorageFormat::FLAG_HAS_DISABLED, 1);
                w(e);
                Hierarchy h{};
                w.WriteVersionedComponent(h);
                Astra::StorageFormat::WriteDisabledBitmap(w, {0});
            }
            else
            {
                WriteBlockHeader(w, hash, Astra::StorageFormat::KIND_SPARSE, 0, 1);
                w(e);
            }
        };
        w(uint32_t{2});
        writeOne(std::max(hierHash, playerHash));   // descending: invalid
        writeOne(std::min(hierHash, playerHash));
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg));
}

TEST(ComponentStorageFormat, DisabledBitmapPopcountMismatchIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        w(uint32_t{1});
        WriteBlockHeader(w, Astra::TypeID<Hierarchy>::Hash(), Astra::StorageFormat::KIND_SPARSE,
                         Astra::StorageFormat::FLAG_HAS_VALUES | Astra::StorageFormat::FLAG_HAS_DISABLED, 1);
        w(e);
        Hierarchy h{};
        w.WriteVersionedComponent(h);
        w(uint32_t{2});     // claims two disabled elements...
        w(uint64_t{1});     // ...but one bit is set
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg));
}
```

- [ ] **Step 2: Build and run, and watch them fail**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ComponentStorageFormat.*
```

Expected FAIL:
- `SparseUniversalAndSparseTagRoundTrip...`: `loaded.IsOk()` is false. The Task 31 stub refuses a non-empty block.
- `DeclaredUniversalAbsent...`: `inits` is `0`, or `GetComponent<Name>` returns `nullptr`. Nothing fills the column on load.
- `BlockNamingADeadEntityIsRefused` and `BlocksOutOfHashOrderAreRefused` happen to pass already, because the stub refuses every non-empty block. `DisabledBitmapPopcountMismatchIsRefused` passes the same way. All three must still pass after Step 3.

- [ ] **Step 3: Implement**

`include/Astra/Registry/ComponentStorageSerialization.hpp`: inside `namespace Astra::Detail`, after `WriteStoredPayload`, add:

```cpp
        // Where a loaded element of `desc` for `e` lives in the LOADING registry's
        // storage (spec s8.3): the existing element, or a freshly default-constructed
        // one that the caller deserializes over. EmptyComponentSentinel() for a tag;
        // nullptr refuses the load (slot cap exhausted, or the kind is Table).
        inline void* AcquireLoadTarget(ComponentStorage& storage, const ComponentDescriptor& desc, Entity e)
        {
            switch (storage.KindOf(desc.id))
            {
            case StorageKind::Sparse:
            {
                if (storage.AcquireSparseSlot(desc.id) < 0)
                    return nullptr;   // s3.2: the sparse-slot cap refuses
                SparseSet* set = storage.Sparse(desc.id);
                if (!set)
                    return nullptr;
                void* element = set->Contains(e) ? set->Get(e) : set->Insert(e);
                return desc.size == 0 ? EmptyComponentSentinel() : element;
            }
            case StorageKind::Universal:
            {
                UniversalColumn* column = storage.Universal(desc.id);
                if (!column)
                    return nullptr;
                void* element = column->IsLive(e) ? column->Get(e) : column->Construct(e);
                return desc.size == 0 ? EmptyComponentSentinel() : element;
            }
            default:
                return nullptr;
            }
        }

        inline void SetLoadedDisabled(ComponentStorage& storage, ComponentID id, Entity e)
        {
            switch (storage.KindOf(id))
            {
            case StorageKind::Sparse:
                if (SparseSet* set = storage.Sparse(id)) set->SetDisabled(e, true);
                break;
            case StorageKind::Universal:
                if (UniversalColumn* column = storage.Universal(id)) column->SetDisabled(e, true);
                break;
            default:
                break;
            }
        }
```

`include/Astra/Registry/Registry.hpp`, the end of the file: replace the Task 31 stub `ComponentStorage::Deserialize` with:

```cpp
    namespace Detail
    {
        // One stored component's payload (spec s8.2), read into whatever storage the
        // LOADING registry assigns (s8.3). Entities are untrusted: each must be live
        // in the registry being loaded, ascending and unique.
        inline bool ReadStoredPayload(BinaryReader& r, ComponentStorage& storage, const Registry& registry,
                                      const ComponentDescriptor& desc, uint8_t flags, uint32_t count)
        {
            if (r.CountExceedsRemaining(count, sizeof(Entity)))
                return false;
            std::vector<Entity> entities(count);
            for (Entity& e : entities) r(e);
            if (r.HasError())
                return false;
            for (size_t i = 0; i < entities.size(); ++i)
            {
                if (!registry.IsValid(entities[i]))
                    return false;
                if (i > 0 && entities[i].GetID() <= entities[i - 1].GetID())
                    return false;
            }

            if (storage.KindOf(desc.id) == StorageKind::Table)
                return false;   // archived Sparse/Universal, Table here: staged by Task 34

            const bool hasValues = (flags & StorageFormat::FLAG_HAS_VALUES) != 0;
            for (const Entity e : entities)
            {
                void* target = AcquireLoadTarget(storage, desc, e);
                if (!target)
                    return false;
                if (hasValues && !desc.deserializeVersioned(r, target))
                    return false;
            }

            if (flags & StorageFormat::FLAG_HAS_DISABLED)
            {
                std::vector<uint8_t> disabled;
                if (!StorageFormat::ReadDisabledBitmap(r, entities.size(), disabled))
                    return false;
                if (desc.isEnableable)   // else: consumed, validated, dropped (IM-9)
                    for (size_t i = 0; i < entities.size(); ++i)
                        if (disabled[i]) SetLoadedDisabled(storage, desc.id, entities[i]);
            }
            return !r.HasError();
        }
    }

    // Format v6 block reader (spec s8.2, s8.3). Defined after Registry is complete:
    // it validates every entity against the registry being loaded.
    inline bool ComponentStorage::Deserialize(BinaryReader& r, Registry& registry)
    {
        uint32_t stored = 0;
        r(stored);
        if (r.HasError() || r.CountExceedsRemaining(stored, StorageFormat::BLOCK_HEADER_BYTES))
            return false;
        const ComponentRegistry* components = registry.GetComponentRegistry();
        if (!components)
            return false;

        uint64_t previousHash = 0;
        for (uint32_t b = 0; b < stored; ++b)
        {
            uint64_t hash = 0;
            uint32_t version = 0;   // informational: per-element versions drive migration
            uint8_t kind = 0;
            uint8_t flags = 0;
            uint32_t count = 0;
            r(hash)(version)(kind)(flags)(count);
            if (r.HasError())
                return false;
            if (b > 0 && hash <= previousHash)
                return false;   // sorted, unique (s8.2)
            previousHash = hash;
            if (kind != StorageFormat::KIND_SPARSE && kind != StorageFormat::KIND_UNIVERSAL)
                return false;
            if ((flags & ~StorageFormat::FLAG_MASK) != 0 || count == 0)
                return false;   // the writer never emits unknown flags or empty blocks

            const ComponentDescriptor* desc = components->GetComponentDescriptorByHash(hash);
            if (!desc)
                return false;   // unregistered here: LoadInternal maps to UnknownComponent
            if ((flags & StorageFormat::FLAG_HAS_VALUES) && desc->size == 0)
                return false;   // data archived for a type that is a tag in this build

            if (r.GetCompressionMode() == CompressionMode::LZ4)
            {
                auto block = r.ReadCompressedBlock();
                if (block.IsErr())
                    return false;
                const auto& bytes = *block.GetValue();
                BinaryReader sub(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
                sub.SetChecksumEnabled(false);
                if (!Detail::ReadStoredPayload(sub, *this, registry, *desc, flags, count) || sub.GetRemaining() != 0)
                    return false;
            }
            else if (!Detail::ReadStoredPayload(r, *this, registry, *desc, flags, count))
            {
                return false;
            }
        }
        return !r.HasError();
    }
```

`include/Astra/Registry/Registry.hpp`, the private section. Add these after `LoadInternal`, before the data members:

```cpp
        // Post-load storage fix-ups (spec s8.2/s8.3), run once the checksum verifies.
        // Task 34 prepends ApplyMigratedTableComponents.
        bool FinishStorageLoad()
        {
            if (!FillMissingUniversals())
                return false;
            return DeriveStoragePresence();
        }

        // s8.3 bullet 3 + s4.2 order: an entity the archive restored without a
        // declared Universal component gets default construction, then the
        // registry's init (Arcane: a fresh GUID). Explicitly archived values are
        // never re-initialised.
        bool FillMissingUniversals()
        {
            for (const auto& spec : m_config.universal)
            {
                UniversalColumn* column = m_componentStorage->Universal(spec.id);
                if (!column)
                    continue;   // declared but unregistered: creation skips it the same way
                for (Archetype* archetype : m_archetypeManager->GetArchetypes())
                {
                    for (const auto& chunk : archetype->GetChunks())
                    {
                        if (!chunk) continue;
                        const auto& entities = chunk->GetEntities();
                        for (size_t i = 0; i < chunk->GetCount(); ++i)
                        {
                            const Entity e = entities[i];
                            if (column->IsLive(e)) continue;
                            void* value = column->Construct(e);
                            if (spec.init) spec.init(e, value, spec.user);
                        }
                    }
                }
            }
            return true;
        }

        // Chunk presence blocks are never serialized (s8.2): rebuild P1 (present
        // bits) and P3 (disabled mirror) from the stores, now that every entity is
        // located. A stored entity without a location is a corrupt archive.
        bool DeriveStoragePresence()
        {
            ComponentStorage& storage = *m_componentStorage;
            for (size_t raw = 0; raw < MAX_COMPONENTS; ++raw)
            {
                const auto id = static_cast<ComponentID>(raw);
                const StorageKind kind = storage.KindOf(id);
                if (kind == StorageKind::Sparse)
                {
                    const int slot = storage.SparseSlotOf(id);
                    if (slot < 0) continue;
                    SparseSet* set = storage.Sparse(id);
                    if (!set) continue;
                    const Entity* dense = set->Entities();
                    for (size_t d = 0; d < set->Size(); ++d)
                    {
                        const EntityRecord* rec = m_archetypeManager->GetEntityRecord(dense[d]);
                        if (!rec || !rec->chunk)
                            return false;
                        const size_t index = rec->location.GetEntityIndex();
                        rec->chunk->SetSparsePresent(static_cast<uint16_t>(slot), index, true);
                        if (set->IsDisabled(dense[d]))
                            rec->chunk->SetSparseDisabled(static_cast<uint16_t>(slot), index, true);
                    }
                }
                else if (kind == StorageKind::Universal)
                {
                    UniversalColumn* column = storage.Universal(id);
                    if (!column || !column->Descriptor().isEnableable) continue;
                    bool ok = true;
                    column->ForEachLive([&](Entity e, const void*)
                    {
                        if (!ok || !column->IsDisabled(e)) return;
                        const int slot = storage.AcquireSparseSlot(id);   // s5.1: an enableable universal mirrors through a slot
                        const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
                        if (slot < 0 || !rec || !rec->chunk) { ok = false; return; }
                        rec->chunk->SetSparseDisabled(static_cast<uint16_t>(slot), rec->location.GetEntityIndex(), true);
                    });
                    if (!ok)
                        return false;
                }
            }
            return true;
        }
```

`LoadInternal`: directly after the `VerifyChecksum` block, and before the final `return ...Ok(...)`, add:

```cpp
            // s8.3 fix-ups: universal defaults + init, then the chunk presence
            // mirror derived from the stores.
            if (!registry->FinishStorageLoad())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::CorruptedData);
            }
```

- [ ] **Step 4: Build and run the new tests, then the full suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ComponentStorageFormat.*:LoadRobustness.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- All `ComponentStorageFormat.*` pass.
- The `LoadRobustness` truncation and corruption sweeps stay green. The block reader is bounded by `CountExceedsRemaining` and `ReadCompressedBlock`.
- The full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Registry/ComponentStorageSerialization.hpp include/Astra/Registry/Registry.hpp tests/Serialization/ComponentStorageFormatTest.cpp
git commit -m @'
feat(storage): load the ComponentStorage block into whatever Sparse/Universal storage the loading registry assigns; declared-but-absent universals get default + init; chunk presence is derived on load; corrupt blocks (dead/unsorted entities, unsorted hashes, bad bitmaps, unknown types) are refused

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---
### Task 33: Table-in-archive, now Sparse/Universal: diversion out of `Archetype::Deserialize`, archetype merge, LZ4 (Review Focus 4), v5 archives

**Files:**
- Modify: `include/Astra/Serialization/StorageMigration.hpp` (add `StorageLoadHooks`)
- Modify: `include/Astra/Archetype/Archetype.hpp` (`Deserialize` at ~996-1310: hooks parameter, archived-column list, relaxed mask check, divert branch; new private static `DivertColumn`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`Deserialize` at ~936-1105: hooks parameter, root re-map for v2, duplicate collection; new private `MergeDuplicateArchetypes`)
- Modify: `include/Astra/Registry/Registry.hpp` (private static load hooks; `LoadInternal` passes them)
- Test: `tests/Serialization/StorageMigrationTest.cpp` (new file: run premake)

**Interfaces:**
- Consumes: Task 32 (`Detail::AcquireLoadTarget`, `Detail::SetLoadedDisabled`, `FinishStorageLoad`); `StorageFormat::ReadDisabledBitmap` (Task 31).
- Produces:
  - `struct StorageLoadHooks { void* ctx; bool (*isDiverted)(void*, ComponentID); void* (*acquire)(void*, const ComponentDescriptor&, Entity); void (*setDisabled)(void*, ComponentID, Entity); bool IsDiverted(ComponentID) const; }`.
  - `Archetype::Deserialize(reader, registryDescriptors, pool = nullptr, tickSource = nullptr, const StorageLoadHooks* hooks = nullptr)`.
  - `ArchetypeManager::Deserialize(reader, const StorageLoadHooks* hooks = nullptr)`.
  - Every existing caller (null hooks) behaves exactly as before.

**Rules (spec s8.3 bullet 1):**
- A descriptor is diverted when the loading registry's `KindOf(id) != Table`.
- A diverted column is excluded from the rebuilt mask and from `Initialize`.
- Its values are deserialized straight into the store, keyed by the chunk's entities. That happens inside the LZ4 sub-block when the archive is compressed.
- The archive-integrity check becomes `disk mask popcount == number of ARCHIVED descriptors`. The rebuilt mask counts only Table descriptors. This is how the spec's "mask count == Table descriptors" is resolved: the disk mask still counts every archived component, so comparing it with the Table subset would refuse every migrating archive.
- Archetypes whose masks become equal are merged by one batched move once every record is located.

- [ ] **Step 1: Write the failing tests**

`tests/Serialization/StorageMigrationTest.cpp`:

```cpp
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../TestComponents.hpp"
#include "../Support/StorageOverride.hpp"
#include "Astra/Registry/Registry.hpp"

namespace
{
    using namespace Astra::Test;
    constexpr int kEntities = 128;

    std::string LongName(int i) { return "entity-name-with-a-long-and-very-repetitive-suffix-" + std::to_string(i); }

    void MintName(Astra::Entity e, void* component, void* user)
    {
        static_cast<Name*>(component)->value = "minted-" + std::to_string(e.GetID());
        ++*static_cast<int*>(user);
    }

    // Save side, all Table: Position everywhere, Name on even entities.
    struct NamedWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> entities;
        NamedWorld()
        {
            creg->RegisterComponents<Position, Name>();
            reg = std::make_unique<Astra::Registry>(creg);
            for (int i = 0; i < kEntities; ++i)
            {
                const Astra::Entity e = reg->CreateEntity<Position>();
                if (i % 2 == 0) reg->AddComponent<Name>(e, LongName(i));
                entities.push_back(e);
            }
        }
    };

    std::vector<std::byte> SaveWith(const Astra::Registry& reg, Astra::CompressionMode mode)
    {
        Astra::Registry::SaveConfig sc;
        sc.compressionMode = mode;
        auto saved = reg.Save(sc);
        EXPECT_TRUE(saved.IsOk());
        return saved.IsOk() ? std::move(*saved.GetValue()) : std::vector<std::byte>{};
    }

    // Writes `reg` exactly as a format-v5 Astra did: header stamped 5, entity
    // manager, archetypes, relationship graph, empty resource block, and NO
    // ComponentStorage block. Valid for registries whose components are all
    // Table (v5 had no other storage) and that hold no resources.
    std::vector<std::byte> SaveAsV5(const Astra::Registry& reg, Astra::CompressionMode mode)
    {
        std::vector<std::byte> buf;
        {
            Astra::BinaryWriter::Config wc;
            wc.compressionMode = mode;
            Astra::BinaryWriter w(buf, wc);
            Astra::BinaryHeader header;
            header.version = 5;
            header.entityCount = static_cast<uint32_t>(reg.Size());
            header.archetypeCount = static_cast<uint32_t>(reg.GetArchetypeManager()->GetArchetypeCount());
            w.WriteHeader(header);
            reg.GetEntityManager().Serialize(w);
            reg.GetArchetypeManager()->Serialize(w);
            reg.GetRelationshipGraph().Serialize(w);
            w(uint32_t{0});   // ResourceStorage::Serialize of an empty store
            w.FinalizeHeader();
            EXPECT_FALSE(w.HasError());
        }
        return buf;
    }

    size_t ArchetypesWithMask(Astra::Registry& reg, const Astra::ComponentMask& mask)
    {
        size_t n = 0;
        for (Astra::Archetype* a : reg.GetArchetypeManager()->GetArchetypes())
            if (a->GetMask() == mask) ++n;
        return n;
    }

    Astra::ComponentMask MaskOf(std::initializer_list<Astra::ComponentID> ids)
    {
        Astra::ComponentMask m;
        for (const auto id : ids) m.Set(id);
        return m;
    }

    const Name* NameByHash(Astra::Registry& reg, Astra::Entity e)
    {
        return static_cast<const Name*>(reg.GetComponentByHash(e, Astra::TypeID<Name>::Hash()));
    }
}
```

```cpp
// Review Focus 4 (PINNED): an LZ4 archive in which Name was a Table column loads
// into a registry where Name is Sparse. The values must be read out of the
// column's compressed block into the sparse set. The {Position, Name} archetype
// must collapse onto {Position} without a duplicate, and P1-P3 must hold.
TEST(StorageMigration, Lz4TableColumnIsDivertedIntoTheSparseSet)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    const auto lz4 = SaveWith(*world.reg, Astra::CompressionMode::LZ4);
    const auto none = SaveWith(*world.reg, Astra::CompressionMode::None);
    ASSERT_EQ(static_cast<uint8_t>(lz4[offsetof(Astra::BinaryHeader, compressionMode)]),
              static_cast<uint8_t>(Astra::CompressionMode::LZ4));
    ASSERT_LT(lz4.size(), none.size()) << "precondition: the Name column must really be compressed";

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(lz4, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(out.Size(), static_cast<size_t>(kEntities));
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
    EXPECT_EQ(ArchetypesWithMask(out, MaskOf({Astra::TypeID<Position>::Value()})), 1u)
        << "the diverted {Position, Name} archetype must merge into {Position}";
    for (int i = 0; i < kEntities; ++i)
    {
        const Astra::Entity e = world.entities[i];
        const Astra::EntityRecord* rec = out.GetArchetypeManager()->GetEntityRecord(e);
        ASSERT_NE(rec, nullptr);
        EXPECT_FALSE(rec->archetype->GetMask().Test(nameId)) << "entity " << i;
        ASSERT_EQ(out.HasComponentByHash(e, Astra::TypeID<Name>::Hash()), i % 2 == 0) << "entity " << i;
        if (i % 2 == 0)
        {
            const Name* n = NameByHash(out, e);
            ASSERT_NE(n, nullptr);
            EXPECT_EQ(n->value, LongName(i));
        }
    }

    // The migrated set behaves like any other sparse set afterwards.
    Name extra("added-after-load");
    ASSERT_TRUE(out.AddComponentByID(world.entities[1], nameId, &extra, sizeof(extra)));
    ASSERT_TRUE(out.RemoveComponentByID(world.entities[0], nameId));
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}

TEST(StorageMigration, InlineEnableableTableColumnKeepsItsDisabledStateAsSparse)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Hierarchy>();
    Astra::Registry reg(saveCreg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 70; ++i)
    {
        const Astra::Entity e = reg.CreateEntity<Position, Hierarchy>();
        reg.GetComponent<Hierarchy>(e)->parent = static_cast<uint32_t>(i);
        if (i % 4 == 1) ASSERT_TRUE(reg.SetEnabled<Hierarchy>(e, false));
        es.push_back(e);
    }
    const auto bytes = SaveWith(reg, Astra::CompressionMode::None);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(bytes, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(out.ValidateStorageInvariants(), "");
    for (int i = 0; i < 70; ++i)
    {
        const auto* h = static_cast<const Hierarchy*>(out.GetComponentByHash(es[i], Astra::TypeID<Hierarchy>::Hash()));
        ASSERT_NE(h, nullptr) << "entity " << i;
        EXPECT_EQ(h->parent, static_cast<uint32_t>(i));
        EXPECT_EQ(out.IsEnabledByID(es[i], hierId), i % 4 != 1) << "entity " << i;
    }
}

TEST(StorageMigration, TableColumnBecomesUniversalAndOnlyEntitiesWithoutItAreInitialised)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        const auto bytes = SaveWith(*world.reg, mode);
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position, Name>();
        int inits = 0;
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>(&MintName, &inits);
        auto loaded = Astra::Registry::Load(bytes, creg, cfg);
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& out = **loaded.GetValue();

        EXPECT_EQ(inits, kEntities / 2) << "init runs only where no value was archived (s4.2, s8.3)";
        EXPECT_EQ(ArchetypesWithMask(out, MaskOf({Astra::TypeID<Position>::Value()})), 1u);
        for (int i = 0; i < kEntities; ++i)
        {
            const Astra::Entity e = world.entities[i];
            const Name* n = out.GetComponent<Name>(e);
            ASSERT_NE(n, nullptr) << "entity " << i;
            EXPECT_EQ(n->value, i % 2 == 0 ? LongName(i) : "minted-" + std::to_string(e.GetID()));
        }
        EXPECT_EQ(out.ValidateStorageInvariants(), "");
    }
}
```

```cpp
// An archetype whose only component is diverted collapses onto the root.
TEST(StorageMigration, ArchetypeLeftWithNoTableComponentsMergesIntoTheRoot)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Name>();
    Astra::Registry reg(saveCreg);
    const Astra::Entity bare = reg.CreateEntity();
    const Astra::Entity named = reg.CreateEntity<Name>();
    reg.GetComponent<Name>(named)->value = "only-a-name";
    const auto bytes = SaveWith(reg, Astra::CompressionMode::None);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(bytes, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(ArchetypesWithMask(out, Astra::ComponentMask{}), 1u);
    EXPECT_TRUE(out.GetArchetypeManager()->GetEntityRecord(named)->archetype->GetMask().None());
    EXPECT_FALSE(out.HasComponentByHash(bare, Astra::TypeID<Name>::Hash()));
    ASSERT_NE(NameByHash(out, named), nullptr);
    EXPECT_EQ(NameByHash(out, named)->value, "only-a-name");
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}

// s8.3: "v5 and older archives load through the same path". All of a v5
// archive's components are Table columns.
TEST(StorageMigration, FormatV5ArchivesLoadIntoUniversalSparseAndTableRegistries)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        const auto v5 = SaveAsV5(*world.reg, mode);
        uint16_t version = 0;
        std::memcpy(&version, v5.data() + 5, sizeof(version));
        ASSERT_EQ(version, 5u);

        {   // Name declared Universal
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<Position, Name>();
            int inits = 0;
            Astra::Registry::Config cfg;
            cfg.AddUniversal<Name>(&MintName, &inits);
            auto loaded = Astra::Registry::Load(v5, creg, cfg);
            ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
            Astra::Registry& out = **loaded.GetValue();
            EXPECT_EQ(inits, kEntities / 2);
            for (int i = 0; i < kEntities; i += 2)
                EXPECT_EQ(out.GetComponent<Name>(world.entities[i])->value, LongName(i));
            EXPECT_EQ(out.ValidateStorageInvariants(), "");
        }
        {   // Name Sparse
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<Position>();
            Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
            auto loaded = Astra::Registry::Load(v5, creg);
            ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
            Astra::Registry& out = **loaded.GetValue();
            for (int i = 0; i < kEntities; ++i)
                EXPECT_EQ(out.HasComponentByHash(world.entities[i], Astra::TypeID<Name>::Hash()), i % 2 == 0);
            EXPECT_EQ(out.ValidateStorageInvariants(), "");
        }
        {   // Name still Table
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<Position, Name>();
            auto loaded = Astra::Registry::Load(v5, creg);
            ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
            Astra::Registry& out = **loaded.GetValue();
            for (int i = 0; i < kEntities; ++i)
                EXPECT_EQ(out.HasComponent<Name>(world.entities[i]), i % 2 == 0);
        }
    }
}
```

- [ ] **Step 2: Regenerate, build, run, and watch it fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageMigration.*
```

Expected FAIL:
- Every test except the "still Table" block. Name stays a Table column under a descriptor whose kind is now Sparse or Universal.
- `HasComponentByHash` routes to the empty sparse set, so `Value of: out.HasComponentByHash(...) Actual: false Expected: true`.
- The Universal cases see `inits == 128` (every entity minted) instead of `64`.
- `ValidateStorageInvariants` may also report P1, or the load may hit the Chunk A Task-level `ConstructComponentAt` assert on a non-Table column. Any of these is the expected red.

- [ ] **Step 3: Implement**

`include/Astra/Serialization/StorageMigration.hpp`:

1. Add `#include "../Component/Component.hpp"` and `#include "../Entity/Entity.hpp"` to the includes.
2. Append after the `StorageFormat` namespace:

```cpp
namespace Astra
{
    // Load-time diversion seam (spec s8.3). Archetype::Deserialize meets columns of
    // components the LOADING registry does not keep in tables and hands their
    // values to ComponentStorage through these callbacks (Archetype cannot see
    // Registry or ComponentStorage). Null hooks mean no diversion: every caller
    // that deserializes archetypes outside Registry::Load is unchanged.
    struct StorageLoadHooks
    {
        void* ctx = nullptr;
        bool  (*isDiverted)(void* ctx, ComponentID id) = nullptr;
        // Constructed element to deserialize over (EmptyComponentSentinel() for a
        // tag); nullptr refuses the load.
        void* (*acquire)(void* ctx, const ComponentDescriptor& desc, Entity e) = nullptr;
        void  (*setDisabled)(void* ctx, ComponentID id, Entity e) = nullptr;

        ASTRA_NODISCARD bool IsDiverted(ComponentID id) const { return isDiverted && isDiverted(ctx, id); }
    };
}
```

`include/Astra/Archetype/Archetype.hpp`:

1. Add `#include "../Serialization/StorageMigration.hpp"` beside the other Serialization includes.

2. Signature:
   ```cpp
   static Result<std::unique_ptr<Archetype>, SerializationError> Deserialize(BinaryReader& reader, const std::vector<ComponentDescriptor>& registryDescriptors, ArchetypeChunkPool* componentPool = nullptr, const Tick* tickSource = nullptr, const StorageLoadHooks* hooks = nullptr)
   ```

3. Replace the `diskHasDisabledSection` declaration block (the IM-9 comment plus `std::vector<uint8_t> diskHasDisabledSection; diskHasDisabledSection.reserve(descriptorCount);`) with:
   ```cpp
            // Every ARCHIVED descriptor, in disk order (the column order chunks were
            // written in). `descriptors` above keeps only the ones this registry stores
            // in tables; a diverted one (s8.3: Table in the archive, Sparse/Universal
            // here) is read straight into ComponentStorage. hasDisabledSection is the
            // IM-9 disk flag; diskSize > 0 || hasDisabledSection says whether the
            // archive wrote a column for it at all (tags without bits never did).
            struct ArchivedColumn
            {
                ComponentDescriptor desc;
                uint64_t diskSize;
                bool hasDisabledSection;
                bool diverted;
            };
            std::vector<ArchivedColumn> archived;
            archived.reserve(descriptorCount);
   ```

4. In the descriptor loop, replace
   ```cpp
                if (it != registryDescriptors.end())
                {
                    descriptors.push_back(*it);
                    diskHasDisabledSection.push_back(hasDisabledSection);
                }
   ```
   with
   ```cpp
                if (it != registryDescriptors.end())
                {
                    const bool diverted = hooks && hooks->IsDiverted(it->id);
                    archived.push_back(ArchivedColumn{*it, size, hasDisabledSection != 0, diverted});
                    if (!diverted)
                        descriptors.push_back(*it);
                }
   ```

5. Replace the mask cross-check `if (mask.Count() != descriptors.size())` with the following. Keep the comment above it, and add one line explaining the change.
   ```cpp
            // s8.3: the disk mask counts every ARCHIVED component (run-independent,
            // archive-internal); the rebuilt localMask counts only the Table ones.
            if (mask.Count() != archived.size())
   ```
   `localMask` is still built from `descriptors`, which now holds the Table subset only.

6. Replace the chunk column loop, from `for (size_t di = 0; di < descriptors.size(); ++di)` down to its closing brace (just before `chunkEntitySum += chunkEntityCount;`), with the loop below.
   - The `else` half is the old loop body with `diskHasDisabledSection[di]` replaced by `column.hasDisabledSection`.
   - Its skip test is Chunk A Task 7's: a column block follows iff `desc.size != 0 || hasDisabledFlag`. Enableable Table tags have bit-only columns, so `GetComponentArrayByID(id) != nullptr` no longer implies data. Task 7 spells it `desc.size != 0 || diskHasDisabledSection[di] != 0` (`archiveHasColumn`); keep that spelling.
   ```cpp
                for (const ArchivedColumn& column : archived)
                {
                    const ComponentDescriptor& desc = column.desc;

                    if (column.diverted)
                    {
                        // s8.3: Table in the archive, Sparse/Universal here. Consume the
                        // column exactly as DeserializeColumn would, but into the store.
                        const bool diskHasColumn = column.diskSize > 0 || column.hasDisabledSection;
                        bool ok = false;
                        if (diskHasColumn && reader.GetVersion() >= 5 && reader.GetCompressionMode() == CompressionMode::LZ4)
                        {
                            auto blk = reader.ReadCompressedBlock();
                            if (blk.IsErr())
                                return ResultType::Err(SerializationError::CorruptedData);
                            const auto& bytes = *blk.GetValue();
                            BinaryReader sub(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
                            sub.SetChecksumEnabled(false);
                            ok = DivertColumn(sub, chunk, desc, chunkEntityCount,
                                              column.diskSize > 0, column.hasDisabledSection, *hooks);
                        }
                        else
                        {
                            // Inline column, or a tag the archive wrote nothing for
                            // (diskHasColumn false: DivertColumn reads zero bytes).
                            ok = DivertColumn(reader, chunk, desc, chunkEntityCount,
                                              column.diskSize > 0, column.hasDisabledSection, *hooks);
                        }
                        if (!ok)
                            return ResultType::Err(reader.HasError() ? reader.GetError() : SerializationError::CorruptedData);
                        continue;
                    }

                    // Chunk A Task 7: a column block follows iff the type has data or
                    // the archive carries its disabled section (bit-only tag columns).
                    if (desc.size == 0 && !column.hasDisabledSection) continue;

                    ResultType colResult = ResultType::Ok(nullptr);
                    if (reader.GetVersion() >= 5 && reader.GetCompressionMode() == CompressionMode::LZ4)
                    {
                        auto blk = reader.ReadCompressedBlock();
                        if (blk.IsErr())
                            return ResultType::Err(SerializationError::CorruptedData);
                        const auto& bytes = *blk.GetValue();
                        BinaryReader sub(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
                        sub.SetChecksumEnabled(false);
                        colResult = DeserializeColumn(sub, chunk, archetype.get(), desc,
                                                      static_cast<size_t>(chunkEntityCount),
                                                      column.hasDisabledSection);
                    }
                    else
                    {
                        colResult = DeserializeColumn(reader, chunk, archetype.get(), desc,
                                                      static_cast<size_t>(chunkEntityCount),
                                                      column.hasDisabledSection);
                    }
                    if (colResult.IsErr())
                        return colResult;
                }
   ```
   (Keep the old body's explanatory comments, the v<=4 LZ4 gate note included, above the `colResult` block.)

7. Private static helper, next to `DeserializeColumn`:
   ```cpp
        // s8.3: one chunk-column archived as Table whose component the loading
        // registry keeps in ComponentStorage. Values are deserialized over the
        // store's element for each of the chunk's entities; the column's disabled
        // section (IM-9 disk flag) becomes the store's per-element flag. Consumes
        // the stream exactly as DeserializeColumn does.
        static bool DivertColumn(BinaryReader& r, ArchetypeChunk* chunk, const ComponentDescriptor& desc,
                                 size_t chunkEntityCount, bool diskHasData, bool hasDisabledSection,
                                 const StorageLoadHooks& hooks)
        {
            const auto& entities = chunk->GetEntities();
            for (size_t i = 0; i < chunkEntityCount; ++i)
            {
                void* target = hooks.acquire(hooks.ctx, desc, entities[i]);
                if (!target)
                    return false;
                if (diskHasData && (desc.size == 0 || !desc.deserializeVersioned(r, target)))
                    return false;   // data archived for a type that is a tag here, or a bad element
            }
            if (hasDisabledSection)
            {
                std::vector<uint8_t> disabled;
                if (!StorageFormat::ReadDisabledBitmap(r, chunkEntityCount, disabled))
                    return false;
                if (desc.isEnableable)   // else consumed, validated, dropped (IM-9)
                    for (size_t i = 0; i < chunkEntityCount; ++i)
                        if (disabled[i]) hooks.setDisabled(hooks.ctx, desc.id, entities[i]);
            }
            return !r.HasError();
        }
   ```

`include/Astra/Archetype/ArchetypeManager.hpp`, `Deserialize`:

1. Signature: `bool Deserialize(BinaryReader& reader, const StorageLoadHooks* hooks = nullptr)`.

2. Directly after `const uint32_t firstArchetypeIndex = (reader.GetVersion() >= 3) ? 0u : 1u;`, add:
   ```cpp
            // v2 archives carry no root record, so index 0 is never re-read and the
            // m_archetypeMap.Clear() above left the surviving root unmapped. Re-map it
            // so the duplicate check below (and later empty-mask lookups) see it.
            if (firstArchetypeIndex == 1 && m_rootArchetype)
                m_archetypeMap[m_rootArchetype->GetMask()] = m_rootArchetype;
            std::vector<Archetype*> duplicates;   // s8.3 collapsed masks, merged below
   ```

3. Pass the hooks: `auto archetypeResult = Archetype::Deserialize(reader, registryDescriptors, &m_chunkPool, &m_tick, hooks);`

4. Replace the non-root `else` branch (`ArchetypeEntry entry; ... m_archetypes.push_back(std::move(entry));`) with:
   ```cpp
                else
                {
                    ArchetypeEntry entry;
                    entry.archetype = std::move(archetype);
                    // s8.3: diverting a column out of an archived archetype can make
                    // its mask equal an earlier one's (or the root's). The map keeps
                    // the first; the duplicate is merged into it once every record
                    // is located (below).
                    if (m_archetypeMap.Find(entry.archetype->GetMask()) != m_archetypeMap.end())
                        duplicates.push_back(entry.archetype.get());
                    else
                        m_archetypeMap[entry.archetype->GetMask()] = entry.archetype.get();
                    m_archetypes.push_back(std::move(entry));
                }
   ```

5. Replace the final `return !reader.HasError();` with:
   ```cpp
            if (reader.HasError())
                return false;
            return duplicates.empty() || MergeDuplicateArchetypes(duplicates);
   ```

6. New private helper, placed beside `BatchMoveEntitiesWithoutComponent`:
   ```cpp
        // Load-only (s8.3): every entity of a duplicate archetype moves, in ONE
        // batched move, into the mapped archetype with the same mask (all columns
        // shared: values, disabled bits and ticks carry), then the drained
        // duplicate is dropped. Records are already located, and
        // BatchMoveEntitiesInternal rewrites them. Nothing caches these archetypes
        // yet: no views exist, and freshly loaded edges are null.
        bool MergeDuplicateArchetypes(const std::vector<Archetype*>& duplicates)
        {
            for (Archetype* dup : duplicates)
            {
                auto it = m_archetypeMap.Find(dup->GetMask());
                if (it == m_archetypeMap.end() || it->second == dup)
                    return false;
                Archetype* canonical = it->second;

                SmallVector<std::pair<Entity, EntityLocation>, 8> batch;
                const auto& chunks = dup->GetChunks();
                for (size_t c = 0; c < chunks.size(); ++c)
                {
                    if (!chunks[c]) continue;
                    const auto& entities = chunks[c]->GetEntities();
                    for (size_t i = 0; i < chunks[c]->GetCount(); ++i)
                        batch.emplace_back(entities[i], EntityLocation::Create(c, i));
                }
                if (!batch.empty() && BatchMoveEntitiesWithoutComponent(dup, canonical, batch) != batch.size())
                    return false;

                auto pos = std::find_if(m_archetypes.begin(), m_archetypes.end(),
                                        [dup](const ArchetypeEntry& e) { return e.archetype.get() == dup; });
                if (pos == m_archetypes.end())
                    return false;
                m_archetypes.erase(pos);
            }
            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
            m_archetypeRemovalCounter.fetch_add(1, std::memory_order_release);
            return true;
        }
   ```

`include/Astra/Registry/Registry.hpp`:

1. Private static hook functions, next to `FinishStorageLoad`:
   ```cpp
        // StorageLoadHooks for LoadInternal (s8.3). ctx is the registry being
        // loaded; its EntityManager is already restored, so entity ids from the
        // archive are validated before any store is touched.
        static bool LoadHookIsDiverted(void* ctx, ComponentID id)
        {
            return static_cast<Registry*>(ctx)->m_componentStorage->KindOf(id) != StorageKind::Table;
        }
        static void* LoadHookAcquire(void* ctx, const ComponentDescriptor& desc, Entity e)
        {
            auto* self = static_cast<Registry*>(ctx);
            if (!self->IsValid(e))
                return nullptr;
            return Detail::AcquireLoadTarget(*self->m_componentStorage, desc, e);
        }
        static void LoadHookSetDisabled(void* ctx, ComponentID id, Entity e)
        {
            Detail::SetLoadedDisabled(*static_cast<Registry*>(ctx)->m_componentStorage, id, e);
        }
   ```

2. In `LoadInternal`, replace `if (!registry->m_archetypeManager->Deserialize(reader))` with:
   ```cpp
            const StorageLoadHooks hooks{registry.get(), &LoadHookIsDiverted, &LoadHookAcquire, &LoadHookSetDisabled};
            if (!registry->m_archetypeManager->Deserialize(reader, &hooks))
   ```
   Leave the block body unchanged.
   - **Ordering requirement.** Task 5 (g) rebuilds `registry->m_componentStorage` (a `std::shared_ptr<ComponentStorage>`) from the caller's `Config` directly after the ArchetypeManager rebuild, BEFORE this `Deserialize` call (see Task 5's ordering block). Verify it is still there; otherwise the diverted values land in a store that is then thrown away.
   - The hooks look up `m_componentStorage` at call time, so they always reach the current store.

- [ ] **Step 4: Build and run the new tests, then the full suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageMigration.*:ComponentStorageFormat.*:BinarySerializationTests.*:ArchetypeManagerTest.*:LoadRobustness.*:RootArchetype*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- All `StorageMigration.*` pass, including `Lz4TableColumnIsDivertedIntoTheSparseSet` (Review Focus 4).
- The existing archetype-level tests are unchanged: they call `Archetype::Deserialize` and `ArchetypeManager::Deserialize` without hooks.
- `V2LoadNullsSurvivingRootStaleEdges` still passes with the root re-mapped.
- The full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Serialization/StorageMigration.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp tests/Serialization/StorageMigrationTest.cpp
git commit -m @'
feat(storage): storage-agnostic load, Table -> Sparse/Universal -- Archetype::Deserialize diverts a now-non-Table column (inline or out of its LZ4 block) into ComponentStorage; collapsed archetypes merge by one batched move; v5 archives load through the same path

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---
### Task 34: Sparse/Universal-in-archive, now Table: staged values + `ApplyMigratedTableComponents`

**Files:**
- Modify: `include/Astra/Serialization/StorageMigration.hpp` (add `StagedTableColumn`)
- Modify: `include/Astra/Registry/ComponentStorage.hpp` (member `m_pendingTable` + accessor `PendingTableMigrations()`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (public `ApplyMigratedTableComponents`, private `GetOrCreateArchetypeByMask`)
- Modify: `include/Astra/Registry/Registry.hpp` (the Table branch of `Detail::ReadStoredPayload`; `FinishStorageLoad` applies the staged columns first)
- Test: `tests/Serialization/StorageMigrationTest.cpp` (append)

**Interfaces:**
- Consumes: Task 32's `ReadStoredPayload`, Task 33's test helpers, `ArchetypeManager::BatchMoveEntitiesInternal` (private, same class), `ArchetypeChunk::{GetComponentPointer, SetDisabled}`.
- Produces:
  - `class StagedTableColumn`: move-only. Owns `count` default-constructed, `desc.alignment`-aligned elements plus their entities and disabled flags. `MoveOut` move-constructs into a column slot and destroys the staged element. The destructor destroys whatever was not moved out.
  - `std::vector<StagedTableColumn>& ComponentStorage::PendingTableMigrations()`.
  - `bool ArchetypeManager::ApplyMigratedTableComponents(std::vector<StagedTableColumn>&)`. It groups entities by (source archetype, added mask) and does one `BatchMoveEntitiesInternal` per group, so each destination archetype receives every staged component in one move with no intermediate archetype. It clears the vector on success.

- [ ] **Step 1: Write the failing tests** (append to `tests/Serialization/StorageMigrationTest.cpp`)

```cpp
namespace
{
    // Save side with Name (and optionally Hierarchy) Sparse via override.
    struct SparseNamedWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::ComponentID nameId = 0;
        Astra::ComponentID hierId = 0;
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> entities;
        SparseNamedWorld()
        {
            creg->RegisterComponents<Position>();
            nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
            hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
            reg = std::make_unique<Astra::Registry>(creg);
            for (int i = 0; i < kEntities; ++i)
            {
                const Astra::Entity e = reg->CreateEntity<Position>();
                if (i % 2 == 0)
                {
                    Name n(LongName(i));
                    EXPECT_TRUE(reg->AddComponentByID(e, nameId, &n, sizeof(n)));
                }
                if (i % 4 == 0)
                {
                    Hierarchy h{};
                    h.parent = static_cast<uint32_t>(i);
                    EXPECT_TRUE(reg->AddComponentByID(e, hierId, &h, sizeof(h)));
                    if (i % 8 == 0) EXPECT_TRUE(reg->SetEnabledByID(e, hierId, false));
                }
                entities.push_back(e);
            }
        }
    };
}

// s8.3 bullet 2: Sparse in the archive, Table here. Staged values land through one
// batched move per destination archetype: {P,N} for Name-only entities and
// {P,N,H} for entities with both. No intermediate {P,H} archetype is created,
// and the disabled state survives.
TEST(StorageMigration, SparseInTheArchiveBecomesTableByOneBatchedMovePerDestination)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    SparseNamedWorld world;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        const auto bytes = SaveWith(*world.reg, mode);
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position, Name, Hierarchy>();   // all Table here
        auto loaded = Astra::Registry::Load(bytes, creg);
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& out = **loaded.GetValue();

        const auto pos = Astra::TypeID<Position>::Value();
        const auto name = Astra::TypeID<Name>::Value();
        const auto hier = Astra::TypeID<Hierarchy>::Value();
        EXPECT_EQ(ArchetypesWithMask(out, MaskOf({pos, name})), 1u);
        EXPECT_EQ(ArchetypesWithMask(out, MaskOf({pos, name, hier})), 1u);
        EXPECT_EQ(out.GetArchetypeManager()->FindArchetype(MaskOf({pos, hier})), nullptr)
            << "staged components must land in one move, never through an intermediate archetype";
        EXPECT_EQ(out.GetArchetypeManager()->FindArchetype(MaskOf({pos, name, hier}))->GetEntityCount(),
                  static_cast<size_t>(kEntities / 4));

        for (int i = 0; i < kEntities; ++i)
        {
            const Astra::Entity e = world.entities[i];
            ASSERT_EQ(out.HasComponent<Name>(e), i % 2 == 0) << "entity " << i;
            if (i % 2 == 0) EXPECT_EQ(out.GetComponent<Name>(e)->value, LongName(i));
            ASSERT_EQ(out.HasComponent<Hierarchy>(e), i % 4 == 0) << "entity " << i;
            if (i % 4 == 0)
            {
                EXPECT_EQ(out.GetComponent<Hierarchy>(e)->parent, static_cast<uint32_t>(i));
                EXPECT_EQ(out.IsEnabled<Hierarchy>(e), i % 8 != 0) << "entity " << i;
            }
        }
        EXPECT_EQ(out.ValidateStorageInvariants(), "");
    }
}

TEST(StorageMigration, UniversalInTheArchiveBecomesTableWhenTheLoaderDoesNotDeclareIt)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Name>();
    Astra::Registry::Config cfg;
    cfg.AddUniversal<Name>();
    Astra::Registry reg(saveCreg, cfg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 20; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.GetComponent<Name>(es.back())->value = LongName(i);
    }
    const auto bytes = SaveWith(reg, Astra::CompressionMode::LZ4);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position, Name>();
    auto loaded = Astra::Registry::Load(bytes, creg);   // default Config: Name is Table here
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    const auto pos = Astra::TypeID<Position>::Value();
    const auto name = Astra::TypeID<Name>::Value();
    EXPECT_EQ(out.GetArchetypeManager()->FindArchetype(MaskOf({pos, name}))->GetEntityCount(), 20u);
    for (int i = 0; i < 20; ++i)
    {
        ASSERT_TRUE(out.HasComponent<Name>(es[i]));
        EXPECT_EQ(out.GetComponent<Name>(es[i])->value, LongName(i));
    }
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}
```

- [ ] **Step 2: Build and run, and watch it fail**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageMigration.*
```

Expected FAIL: both new tests show `loaded.IsOk()` false with `error 4` (`UnknownComponent`). Task 32's `ReadStoredPayload` refuses a block whose component is Table in the loading registry.

- [ ] **Step 3: Implement**

`include/Astra/Serialization/StorageMigration.hpp`: add `#include <new>` and `#include <utility>`, then append inside `namespace Astra`:

```cpp
    // s8.3 bullet 2: values archived Sparse/Universal for a component the loading
    // registry keeps in tables. Elements are default-constructed in an aligned
    // buffer, deserialized over, and later moved into their destination columns by
    // ArchetypeManager::ApplyMigratedTableComponents. Whatever is not moved out is
    // destroyed with the column, so a failed load leaks nothing.
    class StagedTableColumn
    {
    public:
        StagedTableColumn(const ComponentDescriptor& desc, size_t count)
            : m_desc(desc), m_entities(count), m_disabled(count, 0), m_constructed(count, 0)
        {
            if (desc.size == 0 || count == 0)
                return;
            m_data = static_cast<std::byte*>(::operator new(desc.size * count, std::align_val_t{desc.alignment}));
            for (size_t i = 0; i < count; ++i)
            {
                desc.DefaultConstruct(m_data + i * desc.size);
                m_constructed[i] = 1;
            }
        }
        ~StagedTableColumn() { Release(); }

        StagedTableColumn(StagedTableColumn&& other) noexcept
            : m_desc(other.m_desc), m_data(std::exchange(other.m_data, nullptr)),
              m_entities(std::move(other.m_entities)), m_disabled(std::move(other.m_disabled)),
              m_constructed(std::move(other.m_constructed)) {}
        StagedTableColumn& operator=(StagedTableColumn&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_desc = other.m_desc;
                m_data = std::exchange(other.m_data, nullptr);
                m_entities = std::move(other.m_entities);
                m_disabled = std::move(other.m_disabled);
                m_constructed = std::move(other.m_constructed);
            }
            return *this;
        }
        StagedTableColumn(const StagedTableColumn&) = delete;
        StagedTableColumn& operator=(const StagedTableColumn&) = delete;

        ASTRA_NODISCARD const ComponentDescriptor& Descriptor() const noexcept { return m_desc; }
        ASTRA_NODISCARD size_t Size() const noexcept { return m_entities.size(); }
        ASTRA_NODISCARD Entity& EntityAt(size_t i) noexcept { return m_entities[i]; }
        ASTRA_NODISCARD Entity EntityAt(size_t i) const noexcept { return m_entities[i]; }
        ASTRA_NODISCARD void* At(size_t i) noexcept { return m_data ? m_data + i * m_desc.size : EmptyComponentSentinel(); }
        ASTRA_NODISCARD bool IsDisabled(size_t i) const noexcept { return m_disabled[i] != 0; }
        void SetDisabled(size_t i, bool disabled) noexcept { m_disabled[i] = disabled ? 1 : 0; }

        // Move-construct element i into raw column memory, then destroy the staged one.
        void MoveOut(size_t i, void* dst)
        {
            if (!m_data || !m_constructed[i])
                return;
            m_desc.MoveConstruct(dst, At(i));
            m_desc.Destruct(At(i));
            m_constructed[i] = 0;
        }

    private:
        void Release() noexcept
        {
            if (!m_data)
                return;
            for (size_t i = 0; i < m_constructed.size(); ++i)
                if (m_constructed[i]) m_desc.Destruct(m_data + i * m_desc.size);
            ::operator delete(m_data, std::align_val_t{m_desc.alignment});
            m_data = nullptr;
        }

        ComponentDescriptor m_desc;          // by value: the ResourceStorage lifetime rule
        std::byte* m_data = nullptr;
        std::vector<Entity> m_entities;
        std::vector<uint8_t> m_disabled;
        std::vector<uint8_t> m_constructed;
    };
```

`include/Astra/Registry/ComponentStorage.hpp`:
- `#include "../Serialization/StorageMigration.hpp"`.
- Public:
  ```cpp
  // s8.3: Table-now values read from a v6 block, applied after archetype load.
  ASTRA_NODISCARD std::vector<StagedTableColumn>& PendingTableMigrations() noexcept { return m_pendingTable; }
  ```
- Private: `std::vector<StagedTableColumn> m_pendingTable;`

`include/Astra/Registry/Registry.hpp`, `Detail::ReadStoredPayload`: replace

```cpp
            if (storage.KindOf(desc.id) == StorageKind::Table)
                return false;   // archived Sparse/Universal, Table here: staged by Task 34

            const bool hasValues = (flags & StorageFormat::FLAG_HAS_VALUES) != 0;
```

with

```cpp
            const bool hasValues = (flags & StorageFormat::FLAG_HAS_VALUES) != 0;
            if (storage.KindOf(desc.id) == StorageKind::Table)
            {
                // s8.3 bullet 2: archived Sparse/Universal, Table here. Stage the
                // values; FinishStorageLoad moves them into their archetypes.
                StagedTableColumn staged(desc, entities.size());
                for (size_t i = 0; i < entities.size(); ++i)
                {
                    staged.EntityAt(i) = entities[i];
                    if (hasValues && !desc.deserializeVersioned(r, staged.At(i)))
                        return false;
                }
                if (flags & StorageFormat::FLAG_HAS_DISABLED)
                {
                    std::vector<uint8_t> disabled;
                    if (!StorageFormat::ReadDisabledBitmap(r, entities.size(), disabled))
                        return false;
                    if (desc.isEnableable)
                        for (size_t i = 0; i < entities.size(); ++i) staged.SetDisabled(i, disabled[i] != 0);
                }
                storage.PendingTableMigrations().push_back(std::move(staged));
                return !r.HasError();
            }
```

`FinishStorageLoad` gains the first step:

```cpp
        bool FinishStorageLoad()
        {
            if (!m_archetypeManager->ApplyMigratedTableComponents(m_componentStorage->PendingTableMigrations()))
                return false;
            if (!FillMissingUniversals())
                return false;
            return DeriveStoragePresence();
        }
```

`include/Astra/Archetype/ArchetypeManager.hpp`. Public, after `Deserialize`:

```cpp
        // Load-only (spec s8.3 bullet 2): moves values archived Sparse/Universal into
        // the Table columns this registry uses for them. Entities are grouped by
        // (source archetype, set of staged components); each group makes ONE batched
        // move into source-mask | staged-mask, so no intermediate archetype is ever
        // created, and the staged values are move-constructed into the new columns
        // in the move's post-op. A staged component the entity already holds as a
        // Table column, or a stale entity, refuses the load. Clears `staged` on success.
        bool ApplyMigratedTableComponents(std::vector<StagedTableColumn>& staged)
        {
            if (staged.empty())
                return true;

            struct Item { Entity entity; uint32_t column; uint32_t element; };
            std::vector<Item> items;
            for (size_t c = 0; c < staged.size(); ++c)
                for (size_t i = 0; i < staged[c].Size(); ++i)
                    items.push_back(Item{staged[c].EntityAt(i), static_cast<uint32_t>(c), static_cast<uint32_t>(i)});
            std::sort(items.begin(), items.end(), [](const Item& a, const Item& b)
            {
                return a.entity.GetID() != b.entity.GetID() ? a.entity.GetID() < b.entity.GetID() : a.column < b.column;
            });

            struct Group
            {
                Archetype* src = nullptr;
                ComponentMask added;
                SmallVector<std::pair<Entity, EntityLocation>, 8> batch;
            };
            std::vector<Group> groups;
            std::unordered_map<uint64_t, std::pair<size_t, size_t>> runOf;   // entity id -> [begin, end) in items

            for (size_t b = 0; b < items.size();)
            {
                size_t e = b;
                while (e < items.size() && items[e].entity == items[b].entity) ++e;
                const Entity entity = items[b].entity;
                EntityRecord* rec = m_records->GetRecord(entity.GetID());
                if (!rec || rec->version != entity.GetVersion() || !rec->archetype)
                    return false;

                ComponentMask added;
                for (size_t k = b; k < e; ++k)
                {
                    const ComponentID id = staged[items[k].column].Descriptor().id;
                    if (rec->archetype->GetMask().Test(id) || added.Test(id))
                        return false;
                    added.Set(id);
                }
                auto g = std::find_if(groups.begin(), groups.end(),
                                      [&](const Group& x) { return x.src == rec->archetype && x.added == added; });
                if (g == groups.end())
                {
                    groups.push_back(Group{rec->archetype, added, {}});
                    g = groups.end() - 1;
                }
                g->batch.emplace_back(entity, rec->location);
                runOf[static_cast<uint64_t>(entity.GetID())] = {b, e};
                b = e;
            }

```cpp
            for (Group& group : groups)
            {
                // An earlier group's swap-removes may have moved these entities
                // inside the shared source archetype: refresh from the records.
                for (auto& [entity, location] : group.batch)
                    location = m_records->GetRecord(entity.GetID())->location;

                Archetype* dst = GetOrCreateArchetypeByMask(group.src->GetMask() | group.added);
                if (!dst)
                    return false;

                const size_t moved = BatchMoveEntitiesInternal(group.src, dst, group.batch,
                    [&](Archetype* d, const std::vector<EntityLocation>& locations)
                    {
                        // BatchMoveEntitiesInternal sorted group.batch in place before
                        // the move, so locations[i] belongs to group.batch[i].
                        const ArchetypeColumnMeta& meta = d->GetColumnMeta();
                        for (size_t i = 0; i < locations.size(); ++i)
                        {
                            auto [chunk, index] = d->ResolveLocation(locations[i]);
                            const auto [begin, end] = runOf[static_cast<uint64_t>(group.batch[i].first.GetID())];
                            for (size_t k = begin; k < end; ++k)
                            {
                                StagedTableColumn& column = staged[items[k].column];
                                const ComponentDescriptor& desc = column.Descriptor();
                                if (desc.size > 0)
                                    column.MoveOut(items[k].element, chunk->GetComponentPointer(desc.id, index));
                                const int c = meta.idToColumn[desc.id];
                                if (c >= 0 && column.IsDisabled(items[k].element))
                                    chunk->SetDisabled(c, index, true);
                            }
                        }
                    });
                if (moved != group.batch.size())
                    return false;
            }

            staged.clear();
            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
            return true;
        }
```

Add `#include <unordered_map>` to the header's includes if it is not already there.

Private, beside `GetArchetypeWithModified`. This duplicates that function's creation half for an arbitrary mask, so the hot single-component edge path is left untouched (S4):

```cpp
        Archetype* GetOrCreateArchetypeByMask(const ComponentMask& mask)
        {
            auto it = m_archetypeMap.Find(mask);
            if (it != m_archetypeMap.end())
                return it->second;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return nullptr;

            auto archetype = std::make_unique<Archetype>(mask);
            Archetype* ptr = archetype.get();
            ptr->m_chunkPool = &m_chunkPool;
            ptr->SetTickSource(&m_tick);

            std::vector<ComponentDescriptor> descriptors;
            for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
            {
                if (!mask.Test(id)) continue;
                const ComponentDescriptor* desc = registry->GetComponentDescriptor(id);
                if (!desc)
                    return nullptr;
                descriptors.push_back(*desc);
            }
            ptr->Initialize(descriptors);

            m_archetypeMap[mask] = ptr;
            ArchetypeEntry entry;
            entry.archetype = std::move(archetype);
            entry.creationGeneration = ++m_generation;
            m_archetypes.push_back(std::move(entry));
            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
            return ptr;
        }
```

- [ ] **Step 4: Build and run the new tests, then the full suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageMigration.*:ComponentStorageFormat.*:LoadRobustness.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: all pass; the full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Serialization/StorageMigration.hpp include/Astra/Registry/ComponentStorage.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp tests/Serialization/StorageMigrationTest.cpp
git commit -m @'
feat(storage): storage-agnostic load, Sparse/Universal -> Table -- block values are staged and applied by ApplyMigratedTableComponents, one batched archetype move per destination (no intermediate archetypes), disabled state preserved

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---
### Task 35: Unload mechanism: store-release hooks and `ComponentStorage::ReleaseComponent`

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp`
  - public: `StoreReleaseFn`, `AddStoreReleaseHook`, `RemoveStoreReleaseHook`, `StoreReleaseHookCount`.
  - private: `CollectDeparting`, `NotifyStoreRelease`, hook storage.
  - `ReleaseModule` (~555-582) and `UnregisterModuleRange` (~153-223) notify first.
- Modify: `include/Astra/Registry/ComponentStorage.hpp` (add `ReleaseComponent`; no earlier task declares it)
- Test: `tests/Component/StorageModuleUnloadTest.cpp` (new file: run premake)

**Interfaces:**
- Consumes:
  - `ComponentStorage` internals, by their defining tasks' names:
    - Chunk A (Task 4): the slot table `m_slotOf` (id -> slot, `int16_t`, -1 = none), `m_componentOfSlot` (slot -> id, `INVALID_COMPONENT` = free) and `m_usedSlots`. A slot is freed by resetting BOTH entries and decrementing `m_usedSlots`; `AcquireSparseSlot` then reuses the lowest free slot.
    - Chunk B (Tasks 9, 11): the store owners `m_universalColumns` / `m_sparseSets` (`std::array<std::unique_ptr<...>, MAX_COMPONENTS>`), the live-set list `m_sparseIds` (iterated by `SetTickSource`, `Clear` and every other all-sets loop -- a released id MUST leave it, or those loops dereference a null set) and the store count `m_storeCount` (`HasAnyStore`).
    - Chunk C (Task 20): the coarse version array `m_coarseVersion`, reset so a reloaded type starts with no history.
  - Chunk C's `ArchetypeChunk::ReleaseSparseBlock(uint16_t slot)` (with `GetSparseBlocks`/`HasSparseBlocks`). It already exists, so this chunk adds no chunk-pool code; the Task 35 test pins the behaviour Task 36 relies on.
- Produces:
  - `using ComponentRegistry::StoreReleaseFn = void (*)(void* ctx, ComponentID id, const ComponentDescriptor& departing);`
  - `uint64_t AddStoreReleaseHook(void* ctx, StoreReleaseFn)`, `void RemoveStoreReleaseHook(uint64_t)`, `size_t StoreReleaseHookCount() const`.
  - `void ComponentStorage::ReleaseComponent(ComponentID)` (canonical).

**Contract (spec s4.4, R4):**
- `ReleaseModule(owner)` and `UnregisterModuleRange(base, size)` first collect every departing entry (live and shadowed) with its descriptor, under the registration lock.
- They then call every hook OUTSIDE all locks, while the departing descriptors are still installed. Element destructors are user code, and a hook may destroy a `Registry`, which removes its own hook.
- Only then do they change any slot.
- `ComponentStorage::ReleaseComponent(id)` destroys every element through the stored descriptor, drops the `SparseSet`/`UniversalColumn`, and frees the sparse slot.
- Chunk blocks belong to the archetypes, so the `Registry` releases them (Task 36).

- [ ] **Step 1: Write the failing tests**

`tests/Component/StorageModuleUnloadTest.cpp`:

```cpp
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
#include <gtest/gtest.h>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include "../TestComponents.hpp"
#include "../Support/StorageOverride.hpp"

namespace
{
    using namespace Astra::Test;

    struct ReleaseLog
    {
        Astra::ComponentRegistry* creg = nullptr;
        std::vector<Astra::ComponentID> ids;
        std::vector<bool> installedAtCall;
        std::vector<Astra::ComponentDescriptor::DestructFn*> destructs;
    };

    void RecordRelease(void* ctx, Astra::ComponentID id, const Astra::ComponentDescriptor& departing)
    {
        auto* log = static_cast<ReleaseLog*>(ctx);
        log->ids.push_back(id);
        log->installedAtCall.push_back(log->creg->GetComponentDescriptor(id) != nullptr);
        log->destructs.push_back(departing.destruct);
    }
}

TEST(ComponentRegistryStoreRelease, ReleaseModuleNotifiesBeforeTheSlotChangesAndRemovedHooksStaySilent)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    ReleaseLog log;
    log.creg = creg.get();
    const uint64_t handle = creg->AddStoreReleaseHook(&log, &RecordRelease);
    EXPECT_EQ(creg->StoreReleaseHookCount(), 1u);

    const auto id = Astra::TypeID<Position>::Value();
    {
        auto mod = Astra::ComponentModule::Open(creg, "HookedPlugin");
        ASSERT_TRUE(static_cast<bool>(mod));
        mod.Register<Position>();
        const auto destruct = creg->GetComponentDescriptor(id)->destruct;
        mod.Reset();   // the plugin's Shutdown

        ASSERT_EQ(log.ids.size(), 1u);
        EXPECT_EQ(log.ids[0], id);
        EXPECT_TRUE(log.installedAtCall[0]) << "stores must be torn down while the descriptor is still installed";
        EXPECT_EQ(log.destructs[0], destruct);
        EXPECT_EQ(creg->GetComponentDescriptor(id), nullptr);
    }

    creg->RemoveStoreReleaseHook(handle);
    EXPECT_EQ(creg->StoreReleaseHookCount(), 0u);
    auto again = Astra::ComponentModule::Open(creg, "SecondPlugin");
    again.Register<Position>();
    again.Reset();
    EXPECT_EQ(log.ids.size(), 1u) << "a removed hook is never called";
}

TEST(ComponentRegistryStoreRelease, RangePurgeNotifiesLiveAndShadowedEntries)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    ReleaseLog log;
    log.creg = creg.get();
    const uint64_t handle = creg->AddStoreReleaseHook(&log, &RecordRelease);

    const auto id = Astra::TypeID<Position>::Value();
    creg->RegisterComponent<Position>();                       // anonymous base (shadowed below)
    auto mod = Astra::ComponentModule::Open(creg, "PurgeVictim");
    mod.Register<Position>();                                  // live override
    const auto* live = creg->GetComponentDescriptor(id);
    ASSERT_NE(live, nullptr);
    const void* base = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(live->defaultConstruct) & ~uintptr_t(0xFFFF));
    EXPECT_GE(creg->UnregisterModuleRange(base, size_t(1) << 30), 1u);

    EXPECT_EQ(std::count(log.ids.begin(), log.ids.end(), id), 2) << "live override + shadowed base";
    for (const bool installed : log.installedAtCall) EXPECT_TRUE(installed);
    creg->RemoveStoreReleaseHook(handle);
    mod.Reset();   // idempotent after the purge
}
```

```cpp
TEST(ComponentStorageRelease, ReleaseComponentDestroysEveryElementAndFreesTheSlot)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID id = Astra::TestSupport::InstallWithStorage<RelocationCanary>(*creg, Astra::Storage::Sparse);
    const int liveAtStart = RelocationCanary::s_live;
    {
        Astra::Registry reg(creg);
        for (int i = 0; i < 16; ++i)
        {
            const Astra::Entity e = reg.CreateEntity<Position>();
            RelocationCanary v(i);
            ASSERT_TRUE(reg.AddComponentByID(e, id, &v, sizeof(v)));
        }
        EXPECT_EQ(RelocationCanary::s_live - liveAtStart, 16);

        Astra::ComponentStorage& storage = *reg.GetComponentStorage();
        ASSERT_GE(storage.SparseSlotOf(id), 0);
        storage.ReleaseComponent(id);
        EXPECT_EQ(RelocationCanary::s_live, liveAtStart) << "every element destroyed";
        EXPECT_EQ(storage.SparseSlotOf(id), -1) << "the slot is freed";
        EXPECT_GE(storage.AcquireSparseSlot(id), 0) << "and can be acquired again";
    }
    EXPECT_EQ(RelocationCanary::s_live, liveAtStart);
}

TEST(ComponentStorageRelease, ReleaseSparseBlockDropsOnlyThatSlotsBlock)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    const Astra::ComponentID hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
    Astra::Registry reg(creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Name n("held");
    Hierarchy h{};
    ASSERT_TRUE(reg.AddComponentByID(e, nameId, &n, sizeof(n)));
    ASSERT_TRUE(reg.AddComponentByID(e, hierId, &h, sizeof(h)));

    const auto slotName = static_cast<uint16_t>(reg.GetComponentStorage()->SparseSlotOf(nameId));
    const auto slotHier = static_cast<uint16_t>(reg.GetComponentStorage()->SparseSlotOf(hierId));
    const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
    ASSERT_NE(rec, nullptr);
    ASSERT_NE(rec->chunk->FindSparseBlock(slotName), nullptr);
    ASSERT_NE(rec->chunk->FindSparseBlock(slotHier), nullptr);

    rec->chunk->ReleaseSparseBlock(slotName);
    EXPECT_EQ(rec->chunk->FindSparseBlock(slotName), nullptr);
    ASSERT_NE(rec->chunk->FindSparseBlock(slotHier), nullptr);
    EXPECT_TRUE(rec->chunk->IsSparsePresent(slotHier, rec->location.GetEntityIndex()));
    rec->chunk->ReleaseSparseBlock(slotName);   // no block: a no-op
}
```

- [ ] **Step 2: Regenerate, build, and watch it fail**

```powershell
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
```

Expected: compile error C2039, `'AddStoreReleaseHook': is not a member of 'Astra::ComponentRegistry'`.

If you compile the `ComponentStorageRelease` tests on their own:
- `ReleaseComponentDestroysEveryElementAndFreesTheSlot` fails to compile: `'ReleaseComponent': is not a member of 'Astra::ComponentStorage'`.
- `ReleaseSparseBlockDropsOnlyThatSlotsBlock` already passes: it pins Chunk C's `ReleaseSparseBlock`, which Task 36 relies on.

- [ ] **Step 3: Implement**

`include/Astra/Component/ComponentRegistry.hpp`. Public, after `ReleaseModule`:

```cpp
        // ==== Store-release hooks (storage policies, spec 2026-10-01 s4.4) ====
        // A Registry's ComponentStorage holds Sparse/Universal elements whose
        // destructors live in the module that registered the type. Before
        // ReleaseModule / UnregisterModuleRange change ANY slot, every hook hears
        // about every departing entry (live or shadowed) with that entry's
        // descriptor, so stores built from it can destroy their elements while the
        // image is still mapped. Hooks run outside m_registrationMutex (element
        // destructors are user code) and outside m_hookMutex (a hook may destroy a
        // Registry, which removes its own hook). Like registration, unload is
        // host-serialized: a hook is never removed concurrently with a release.
        using StoreReleaseFn = void (*)(void* ctx, ComponentID id, const ComponentDescriptor& departing);

        ASTRA_NODISCARD uint64_t AddStoreReleaseHook(void* ctx, StoreReleaseFn fn)
        {
            std::lock_guard<std::mutex> lock(m_hookMutex);
            const uint64_t handle = ++m_nextStoreReleaseHook;
            m_storeReleaseHooks.push_back(StoreReleaseHook{handle, ctx, fn});
            return handle;
        }

        void RemoveStoreReleaseHook(uint64_t handle)
        {
            std::lock_guard<std::mutex> lock(m_hookMutex);
            std::erase_if(m_storeReleaseHooks, [handle](const StoreReleaseHook& h) { return h.handle == handle; });
        }

        ASTRA_NODISCARD size_t StoreReleaseHookCount() const
        {
            std::lock_guard<std::mutex> lock(m_hookMutex);
            return m_storeReleaseHooks.size();
        }
```

Private, beside `RestoreOrClearSlot`:

```cpp
        struct StoreReleaseHook { uint64_t handle; void* ctx; StoreReleaseFn fn; };
        using DepartingEntries = SmallVector<std::pair<ComponentID, ComponentDescriptor>, 4>;

        // Snapshot (under the registration lock) of every entry, live or
        // shadowed, for which departs(owner, desc) holds.
        template<typename Pred>
        DepartingEntries CollectDeparting(Pred&& departs)
        {
            DepartingEntries out;
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            for (size_t raw = 0; raw < MAX_COMPONENTS; ++raw)
            {
                const auto id = static_cast<ComponentID>(raw);
                if (auto it = m_shadow.Find(id); it != m_shadow.end())
                    for (const auto& entry : it->second)
                        if (departs(entry.owner, entry.desc)) out.push_back({id, entry.desc});
                if (m_present.Test(raw) && departs(m_owner[raw], m_components[raw]))
                    out.push_back({id, m_components[raw]});
            }
            return out;
        }

        void NotifyStoreRelease(const DepartingEntries& departing)
        {
            if (departing.empty())
                return;
            SmallVector<StoreReleaseHook, 4> hooks;
            {
                std::lock_guard<std::mutex> lock(m_hookMutex);
                for (const StoreReleaseHook& h : m_storeReleaseHooks) hooks.push_back(h);
            }
            for (const auto& [id, desc] : departing)
                for (const StoreReleaseHook& h : hooks)
                    h.fn(h.ctx, id, desc);
        }
```

Data members, after `m_shadow`:

```cpp
        mutable std::mutex m_hookMutex;                    // guards the two members below only
        std::vector<StoreReleaseHook> m_storeReleaseHooks;
        uint64_t m_nextStoreReleaseHook = 0;
```

`ReleaseModule(uint32_t owner)`: make this the first statement, before `SmallVector<MetaRelease, 4> metaWork;` and the lock.

```cpp
            // s4.4: stores built from this owner's descriptors are torn down FIRST,
            // while those descriptors are installed and their code is still mapped.
            NotifyStoreRelease(CollectDeparting(
                [owner](uint32_t entryOwner, const ComponentDescriptor&) { return entryOwner == owner; }));
```

`UnregisterModuleRange`: directly after the `isOwned` lambda, and before `SmallVector<MetaRelease, 4> metaWork;`.

```cpp
            NotifyStoreRelease(CollectDeparting(
                [&isOwned](uint32_t, const ComponentDescriptor& d) { return isOwned(d); }));
```

`include/Astra/Registry/ComponentStorage.hpp`: add this public member function (no earlier task declares `ReleaseComponent`). The steps:
1. destroy every element through the stored descriptor;
2. drop the store object and undo Chunk B's bookkeeping for it (`m_sparseIds`, `m_storeCount`);
3. reset the coarse version and un-map id<->slot;
4. leave the chunk blocks to the caller.

Add `#include <algorithm>` if it is not already included (`std::erase` is in `<vector>`).

```cpp
        // Module unload (spec s4.4): destroy every element of `id` while the
        // departing module's code is still mapped, drop the SparseSet /
        // UniversalColumn (their by-value descriptors point into that module) and
        // free its sparse slot for reuse. The chunk presence blocks for the slot
        // belong to the archetypes: Registry::ReleaseStoredComponent frees them.
        // A later Sparse()/Universal() for `id` builds a fresh store from whatever
        // descriptor is registered then (a reloaded module's).
        void ReleaseComponent(ComponentID id)
        {
            if (id >= MAX_COMPONENTS)
                return;
            if (auto& set = m_sparseSets[id])
            {
                set->Clear();
                set.reset();
                std::erase(m_sparseIds, id);   // Chunk B bookkeeping: all-sets loops skip it now
                --m_storeCount;
            }
            if (auto& column = m_universalColumns[id])
            {
                column->Clear();
                column.reset();
                --m_storeCount;
            }
            m_coarseVersion[id] = Tick{0};     // Task 20: a reloaded type starts with no history
            const int slot = SparseSlotOf(id);
            if (slot >= 0)
            {
                // Chunk A's slot-free rule: reset BOTH table entries, then the count.
                m_componentOfSlot[static_cast<size_t>(slot)] = INVALID_COMPONENT;
                m_slotOf[id] = -1;
                --m_usedSlots;
            }
        }
```

The chunk side, `ArchetypeChunk::ReleaseSparseBlock(slot)`, is Chunk C's. Task 36 calls it on every chunk after `ReleaseComponent`.

- [ ] **Step 4: Build and run the new tests, then the full suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ComponentRegistryStoreRelease.*:ComponentStorageRelease.*:ComponentModule.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected: the new tests pass; every `ComponentModule.*` test is unchanged; the full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Component/ComponentRegistry.hpp include/Astra/Registry/ComponentStorage.hpp tests/Component/StorageModuleUnloadTest.cpp
git commit -m @'
feat(storage): module-unload mechanism -- ComponentRegistry store-release hooks fire for every departing entry (live or shadowed) before any slot changes and outside every lock; ComponentStorage::ReleaseComponent destroys, drops and un-slots

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---
### Task 36: `Registry` tears its stores down at module unload (Review Focus 5)

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp`
  - Every constructor body subscribes.
  - `~Registry()` unsubscribes.
  - New member `m_storeReleaseHook`.
  - Private `InstallStoreReleaseHook`, `OnStoreRelease`, `ReleaseStoredComponent`.
- Test: `tests/Component/StorageModuleUnloadTest.cpp` (append)

**Interfaces:**
- Consumes: Task 35 (`AddStoreReleaseHook`/`RemoveStoreReleaseHook`, `ComponentStorage::ReleaseComponent`); Chunk C's `ArchetypeChunk::ReleaseSparseBlock` and `ArchetypeManager::BumpSparseStructuralCounter` (Tasks 15, 16); `ComponentStorage::{KindOf, SparseSlotOf, FindSparse, FindUniversal}`; `SparseSet/UniversalColumn::Descriptor()`.
- Produces: every `Registry`, including one built by `Load`, holds exactly one store-release hook on its `ComponentRegistry` for its whole lifetime. On a departing entry whose descriptor the registry's store was built from (matched by `destruct` pointer), the registry:
  1. destroys every element (`ReleaseComponent`);
  2. frees the slot;
  3. releases that slot's presence block in every chunk.
- Entities that held the component simply no longer have it. No signal fires: unload is not a structural edit, and no `ComponentRemoved` is emitted, matching destroy semantics.

- [ ] **Step 1: Write the failing tests** (append to `tests/Component/StorageModuleUnloadTest.cpp`)

```cpp
namespace
{
    bool g_pluginUnloaded = false;
    int g_pluginDestructs = 0;
    int g_postUnloadDestructs = 0;

    // The plugin image's destructor for RelocationCanary. Once the plugin is
    // "unloaded", a call here stands for a jump into unmapped code: it is counted
    // and does nothing else.
    void PluginCanaryDestruct(void* p)
    {
        if (g_pluginUnloaded)
        {
            ++g_postUnloadDestructs;
            return;
        }
        ++g_pluginDestructs;
        static_cast<RelocationCanary*>(p)->~RelocationCanary();
    }

    struct PluginInstall { uint32_t owner; Astra::ComponentID id; };

    // Installs RelocationCanary as a plugin-owned type whose destructor is
    // PluginCanaryDestruct: the in-process stand-in for a DLL's descriptors.
    PluginInstall InstallPluginCanary(Astra::ComponentRegistry& creg, Astra::Storage storage)
    {
        static int s_pluginImage = 0;
        g_pluginUnloaded = false;
        g_pluginDestructs = 0;
        g_postUnloadDestructs = 0;
        const Astra::ComponentID id = Astra::TypeID<RelocationCanary>::Value();
        Astra::ComponentDescriptor desc = Astra::ComponentRegistry::MakeDescriptor<RelocationCanary>(id, nullptr);
        desc.storage = storage;
        desc.destruct = &PluginCanaryDestruct;
        desc.name = "PluginCanary";
        const uint32_t owner = creg.OpenModuleId("CanaryPlugin");
        EXPECT_EQ(creg.InstallOwned(id, owner, desc, &s_pluginImage), Astra::InstallResult::Installed);
        return {owner, id};
    }

    bool AnyChunkHasBlock(Astra::Registry& reg, int slot)
    {
        for (Astra::Archetype* archetype : reg.GetArchetypeManager()->GetArchetypes())
            for (const auto& chunk : archetype->GetChunks())
                if (chunk && chunk->FindSparseBlock(static_cast<uint16_t>(slot)))
                    return true;
        return false;
    }

    enum class UnloadPath { ReleaseModule, RangePurge };

    void RunSparsePluginUnload(UnloadPath path)
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position>();
        const PluginInstall plugin = InstallPluginCanary(*creg, Astra::Storage::Sparse);
        const int liveAtStart = RelocationCanary::s_live;

        Astra::Registry reg(creg);
        std::vector<Astra::Entity> holders;
        for (int i = 0; i < 100; ++i)   // > 64: blocks span several words
        {
            const Astra::Entity e = reg.CreateEntity<Position>();
            RelocationCanary v(i);
            ASSERT_TRUE(reg.AddComponentByID(e, plugin.id, &v, sizeof(v)));
            holders.push_back(e);
        }
        const Astra::Entity bystander = reg.CreateEntity<Position>();
        const int slot = reg.GetComponentStorage()->SparseSlotOf(plugin.id);
        ASSERT_GE(slot, 0);
        ASSERT_TRUE(AnyChunkHasBlock(reg, slot));
        ASSERT_EQ(reg.ValidateStorageInvariants(), "");

        const int destructsBefore = g_pluginDestructs;   // set growth may already have relocated elements
        if (path == UnloadPath::ReleaseModule)
            (void)creg->ReleaseModule(plugin.owner);    // what the plugin's ComponentModule::Reset() runs
        else
            EXPECT_EQ(creg->UnregisterModuleRange(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&PluginCanaryDestruct)), 1), 1u);
        g_pluginUnloaded = true;                        // the image is gone from here on

        EXPECT_EQ(g_pluginDestructs - destructsBefore, 100) << "every element destroyed at unload, through the still-mapped destructor";
        EXPECT_EQ(RelocationCanary::s_live, liveAtStart);
        EXPECT_EQ(creg->GetComponentDescriptor(plugin.id), nullptr);
        EXPECT_EQ(reg.GetComponentStorage()->SparseSlotOf(plugin.id), -1);
        EXPECT_FALSE(AnyChunkHasBlock(reg, slot)) << "no presence block may survive for the released slot";
        EXPECT_EQ(reg.ValidateStorageInvariants(), "");

        reg.DestroyEntities(holders);
        reg.DestroyEntity(bystander);
        EXPECT_EQ(g_postUnloadDestructs, 0) << "destroying the former holders must not reach the unloaded destructor";
        EXPECT_EQ(reg.Size(), 0u);
        EXPECT_EQ(reg.ValidateStorageInvariants(), "");
    }
}
```

```cpp
// Review Focus 5 (PINNED): unload a plugin module whose Sparse component is still
// held by live entities, then destroy those entities. No destructor may be
// reached through the unloaded descriptor, and no presence block may be left
// for the released slot. Both unload paths: the RAII ComponentModule::Reset()
// route (ReleaseModule) and the UnregisterModuleRange safety valve.
TEST(StorageModuleUnload, UnloadingAPluginSparseComponentHeldByLiveEntitiesLeavesNoDanglingDestructorOrBlock)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    RunSparsePluginUnload(UnloadPath::ReleaseModule);
}

TEST(StorageModuleUnload, RangePurgeOfAPluginSparseComponentLeavesNoDanglingDestructorOrBlock)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    RunSparsePluginUnload(UnloadPath::RangePurge);
}

TEST(StorageModuleUnload, UnloadingAPluginUniversalComponentDestroysItsColumn)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const PluginInstall plugin = InstallPluginCanary(*creg, Astra::Storage::Table);   // universal by Config
    Astra::Registry::Config cfg;
    cfg.AddUniversal<RelocationCanary>();
    {
        Astra::Registry reg(creg, cfg);
        std::vector<Astra::Entity> es;
        for (int i = 0; i < 10; ++i) es.push_back(reg.CreateEntity<Position>());

        const int destructsBefore = g_pluginDestructs;
        (void)creg->ReleaseModule(plugin.owner);
        g_pluginUnloaded = true;
        EXPECT_EQ(g_pluginDestructs - destructsBefore, 10);

        reg.DestroyEntities(es);
        EXPECT_EQ(g_postUnloadDestructs, 0);
    }   // registry teardown: no store is left holding the plugin's descriptor
    EXPECT_EQ(g_postUnloadDestructs, 0);
}

TEST(StorageModuleUnload, EveryRegistrySubscribesForItsLifetimeIncludingLoadedOnes)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    EXPECT_EQ(creg->StoreReleaseHookCount(), 0u);
    {
        Astra::Registry reg(creg);
        EXPECT_EQ(creg->StoreReleaseHookCount(), 1u);
        auto saved = reg.Save();
        ASSERT_TRUE(saved.IsOk());
        auto loaded = Astra::Registry::Load(*saved.GetValue(), creg);
        ASSERT_TRUE(loaded.IsOk());
        EXPECT_EQ(creg->StoreReleaseHookCount(), 2u);
    }
    EXPECT_EQ(creg->StoreReleaseHookCount(), 0u) << "a destroyed Registry must never be called back";
}
```

- [ ] **Step 2: Build, run, and watch it fail**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageModuleUnload.*
```

Expected FAIL:
- `StoreReleaseHookCount()` is `0`, not `1`.
- In both pinned tests, `g_pluginDestructs - destructsBefore` is `0`, not `100`.
- `AnyChunkHasBlock(reg, slot)` stays true.
- `g_postUnloadDestructs` is `100`: destroying the former holders reaches the "unloaded" destructor through the stale set. This is the R4 defect the pin guards.

- [ ] **Step 3: Implement** (`include/Astra/Registry/Registry.hpp`)

1. In every constructor body, APPEND this statement after the statements earlier tasks put there (Task 9's `m_componentStorage->SetTickSource(...)` and Task 22's `m_archetypeManager->SetComponentStorage(...)`):
   ```cpp
            InstallStoreReleaseHook();
   ```

2. Replace `~Registry() = default;` with:
   ```cpp
        ~Registry()
        {
            // Unsubscribe before any member (the stores included) is destroyed.
            if (m_componentRegistry && m_storeReleaseHook != 0)
                m_componentRegistry->RemoveStoreReleaseHook(m_storeReleaseHook);
        }
   ```

3. Data member. Add it directly before `m_instanceId`, so `m_instanceId` keeps its "last member" comment:
   ```cpp
        uint64_t m_storeReleaseHook = 0;   // ComponentRegistry store-release subscription (s4.4)
   ```

4. Private functions, beside `FinishStorageLoad`:
   ```cpp
        // s4.4: every Registry, including one built by Load, which goes through
        // these constructors, subscribes once to its ComponentRegistry's
        // store-release hooks. Registry is non-copyable and non-movable, so `this`
        // stays valid for the subscription's whole life.
        void InstallStoreReleaseHook()
        {
            if (m_componentRegistry)
                m_storeReleaseHook = m_componentRegistry->AddStoreReleaseHook(this, &Registry::OnStoreRelease);
        }

        static void OnStoreRelease(void* ctx, ComponentID id, const ComponentDescriptor& departing)
        {
            static_cast<Registry*>(ctx)->ReleaseStoredComponent(id, departing);
        }

        // A module entry is departing (spec s4.4, R4). If this registry's store for
        // `id` was built from that entry's descriptor, matched by its destructor
        // (the code that is about to be unmapped), then:
        //   1. destroy every element and drop the store, freeing the slot;
        //   2. release the slot's presence block in every chunk, so no block
        //      outlives its slot and P1 holds for the former holders.
        // A store built from a different module's entry (a shadowed or
        // overriding registration) is left alone. Runs host-serialized, like
        // registration, on the registry's own context.
        void ReleaseStoredComponent(ComponentID id, const ComponentDescriptor& departing)
        {
            ComponentStorage& storage = *m_componentStorage;
            bool holdsDeparting = false;
            switch (storage.KindOf(id))
            {
            case StorageKind::Sparse:
                if (const SparseSet* set = storage.FindSparse(id))          // never create a store here
                    holdsDeparting = set->Descriptor().destruct == departing.destruct;
                break;
            case StorageKind::Universal:
                if (const UniversalColumn* column = storage.FindUniversal(id))
                    holdsDeparting = column->Descriptor().destruct == departing.destruct;
                break;
            default:
                break;   // Table data lives in archetype columns: not this path's concern
            }
            if (!holdsDeparting)
                return;

            const int slot = storage.SparseSlotOf(id);   // Sparse, or an enableable Universal's mirror slot
            storage.ReleaseComponent(id);
            if (slot < 0)
                return;
            m_archetypeManager->BumpSparseStructuralCounter();   // membership changed for every former holder
            for (Archetype* archetype : m_archetypeManager->GetArchetypes())
                for (const auto& chunk : archetype->GetChunks())
                    if (chunk) chunk->ReleaseSparseBlock(static_cast<uint16_t>(slot));
        }
   ```

- [ ] **Step 4: Build and run the new tests, then the full suite**

```powershell
msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageModuleUnload.*:ComponentRegistryStoreRelease.*:ComponentStorageRelease.*:ComponentModule.*:StorageMigration.*:ComponentStorageFormat.*
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0
```

Expected:
- All pass, including both Review Focus 5 pins: `UnloadingAPluginSparseComponentHeldByLiveEntitiesLeavesNoDanglingDestructorOrBlock` and `RangePurgeOfAPluginSparseComponentLeavesNoDanglingDestructorOrBlock`.
- `ComponentModule.RegistryDestructionReleasesNothing` and the rest of `ComponentModule.*` are unchanged. The hooks touch stores only, never meta refs.
- The full run is green. Record the seed line.

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Registry/Registry.hpp tests/Component/StorageModuleUnloadTest.cpp
git commit -m @'
feat(storage): module unload tears down Sparse/Universal stores -- every Registry subscribes to its ComponentRegistry's store-release hooks for its lifetime and, for a store built from the departing descriptor, destroys its elements, frees the slot and releases that slot's presence block in every chunk before the descriptor goes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
'@
```

---

## Chunk F: proof, performance, tooling (Tasks 37-42)

Chunk F adds no storage behaviour. It proves D2 (the parity suites), hammers P1-P3 (the fuzzer), measures S4 (benchmarks + the gate), exposes the stores to tools (Inspector), and closes the docs.

**A failing parity or fuzz test here is a defect in Tasks 1-36, not in the test.** These tests are expected to pass on first run because the features already exist. When one fails:
1. use superpowers:systematic-debugging;
2. fix the store / dispatch code in the task's spirit (smallest correct fix);
3. never weaken the expectation to match the code -- D2 is binding;
4. record the defect and the fix in the task report.

**Order inside the chunk.** Inspector reporting (Task 40) lands BEFORE the benchmarks (Tasks 41-42), so the S4 gate measures the final code. The Inspector work adds no Registry members and no hot-path code (its churn advisory listens to signals), so it cannot move the gate.

**TypeID budget.** All test component names this chunk uses are declared in ONE header, `tests/Storage/StorageParity.hpp` (Task 37), with an explicit budget note:
- three new ids: `ParityHierS`, `ParityTrackedS`, `ParityLiveS`;
- two aliases, no new id: `ParityPosS = Astra::Test::PosS` (Task 3) and `ParityTagS = Astra::Test::SparseC::SpTag` (chunk C, an enableable sparse tag);
- the Table and Universal variants reuse existing `Astra::Test` types, including `Astra::Test::EnTag` (Task 6) as the enableable Table tag;
- the parity suites, the fuzzer and the Inspector tests all share these types.

**New test files.** Every task that ADDS a `tests/**.cpp` file regenerates the projects first:
`D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026` from `D:\dev\starworks\Astra-storage`.

---

### Task 37: Storage-parity support header + Registry-surface parity suite (s9.1)

**Files:**
- Create: `tests/Storage/StorageParity.hpp`
- Create: `tests/Storage/StorageParityRegistryTest.cpp`

**Interfaces:**
- Consumes:
  - `Astra::Storage`, `Astra::StorageKind` and `Astra::StorageOfV<T>` (Task 3);
  - `Registry::Config::AddUniversal<T>()` (Task 5);
  - `Registry(std::shared_ptr<ComponentRegistry>, const Config&)`;
  - `Registry::Load(std::span<const std::byte>, std::shared_ptr<ComponentRegistry>, const Config&)`;
  - every typed and by-id Registry entry point;
  - the creation funnel, including CommandBuffer creates (Tasks 9 and 11);
  - the Universal remove refusal (Task 12).
- Produces, all in namespace `Astra::Test::Parity` and used by Tasks 38-40:
  - the new components `ParityHierS`, `ParityTrackedS`, `ParityLiveS`, and the aliases `ParityPosS` (= `Astra::Test::PosS`) and `ParityTagS` (= `Astra::Test::SparseC::SpTag`);
  - the variant types `Variant<C, K>`, `TableV<C>`, `SparseV<C>`, `UniversalV<C>`, and `VariantNames`;
  - the helpers `MakePos<C>`, `MakeHier<C>`, `Give<V>`, `RecycleIndex`;
  - the assert-capture helpers `AssertCount`, `CountAndContinue` and `kAssertsActive`.

**How a parity body works.** Each typed suite runs ONE body over three variants of a component that differ only in storage:
- **Table:** the existing `Astra::Test` type in a plain registry.
- **Sparse:** a field-identical twin that declares `AstraStorage = Sparse`.
- **Universal:** the existing `Astra::Test` type in a registry whose `Config` declares it universal.

A body branches on `V::kUniversal` in only two places, both inherent to D1/D2:
- **Presence:** a Universal component is on every live entity. "Giving" it means writing its value, because an `Add` is a no-op that returns `true`.
- **Removal:** a Universal component cannot be removed. Where the other variants remove, the Universal variant asserts the refusal instead: `ASTRA_ASSERT` in Debug, `false` in Release, no signal.

Every other expectation is shared.

- [ ] **Step 1: Write the support header**

Create `tests/Storage/StorageParity.hpp`:

```cpp
#pragma once

// Storage-parity support (spec 2026-10-01 s9.1). The SAME behavioural test bodies run
// over three variants of one component that differ only in storage:
//   Table     -- the Table-trait Astra::Test type in a plain registry;
//   Sparse    -- a field-identical twin declaring `AstraStorage = Sparse`;
//   Universal -- the Table-trait type in a registry whose Config declares it universal.
//
// TypeID BUDGET (see tests/TestComponents.hpp and premake5.lua's ASTRA_MAX_COMPONENTS=192
// note): the AstraTest process mints ComponentIDs process-wide across every suite. The
// Table and Universal variants REUSE existing Astra::Test types (no new id), and so do two of
// the Sparse twins (PosS from Task 3, SpTag from chunk C). Only THREE types below are new
// (ParityHierS, ParityTrackedS, ParityLiveS), and they are shared by the parity suites, the invariant
// fuzzer and the Inspector tests. Do NOT add a type here without re-running the full suite
// (Task 37 Step 6): an id >= ASTRA_MAX_COMPONENTS dies in Bitmap's "index < Bits" guard.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <Astra/Astra.hpp>
#include "../Registry/SparseStructureTestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"
#include "../TestComponents.hpp"

namespace Astra::Test::Parity
{
    // Sparse twin of Astra::Test::Position (same fields, ctor and equality): REUSED from
    // Task 3 (tests/TestComponents.hpp), no new id.
    using ParityPosS = Astra::Test::PosS;

    // Sparse twin of Astra::Test::Hierarchy (enableable, same fields).
    struct ParityHierS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraEnableable = true;
        uint32_t parent = 0;
        uint32_t firstChild = 0;
        uint32_t nextSibling = 0;
        uint32_t childCount = 0;
        ParityHierS() = default;
    };

    // Sparse ENABLEABLE tag (presence only, no data buffer; s3.2): REUSED from chunk C
    // (tests/Registry/SparseStructureTestComponents.hpp), no new id. Its Table twin is
    // Astra::Test::EnTag (Task 6), the enableable Table tag with a bit-only column.
    using ParityTagS = Astra::Test::SparseC::SpTag;

    // Sparse twin of Astra::Test::TrackedPos (change-tracked: per-element ticks in the store).
    struct ParityTrackedS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraChangeTracked = true;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        ParityTrackedS() = default;
        ParityTrackedS(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
        bool operator==(const ParityTrackedS&) const = default;
    };

    // Sparse, non-trivial, lifetime-counted (fuzzer leak / double-destroy accounting):
    // s_live == number of live instances anywhere in the process.
    struct ParityLiveS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static inline int s_live = 0;
        int value = 0;

        ParityLiveS() { ++s_live; }
        explicit ParityLiveS(int v) : value(v) { ++s_live; }
        ParityLiveS(const ParityLiveS& o) : value(o.value) { ++s_live; }
        ParityLiveS(ParityLiveS&& o) noexcept : value(o.value) { ++s_live; }
        ParityLiveS& operator=(const ParityLiveS& o) { value = o.value; return *this; }
        ParityLiveS& operator=(ParityLiveS&& o) noexcept { value = o.value; return *this; }
        ~ParityLiveS() { --s_live; }

        // Required so RegisterComponents<ParityLiveS> compiles (see Tracked's note in
        // TestComponents.hpp: non-trivially-copyable types need a Serialize).
        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(value);
        }
    };

    static_assert(Astra::StorageOfV<ParityPosS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<ParityHierS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<ParityTagS> == Astra::Storage::Sparse && std::is_empty_v<ParityTagS> && Astra::IsEnableableV<ParityTagS>);
    static_assert(Astra::StorageOfV<Astra::Test::EnTag> == Astra::Storage::Table && Astra::IsEnableableV<Astra::Test::EnTag>);
    static_assert(Astra::StorageOfV<ParityTrackedS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<ParityLiveS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<Astra::Test::Position> == Astra::Storage::Table);

    // ---- Variants ------------------------------------------------------------------

    template<class C, Astra::StorageKind K>
    struct Variant
    {
        using Comp = C;
        static constexpr Astra::StorageKind kind = K;
        static constexpr bool kUniversal = (K == Astra::StorageKind::Universal);
        static constexpr bool kSparse = (K == Astra::StorageKind::Sparse);
        static constexpr const char* kName = kUniversal ? "Universal" : (kSparse ? "Sparse" : "Table");

        // The trait must agree with the variant: Sparse twins declare Sparse; the Table and
        // Universal variants use a Table-trait type (Universal is a per-registry override).
        static_assert(kSparse == (Astra::StorageOfV<C> == Astra::Storage::Sparse),
                      "variant kind and the component's AstraStorage trait disagree");

        static Astra::Registry::Config MakeConfig(Astra::Registry::Config config = {})
        {
            if constexpr (kUniversal)
                config.AddUniversal<C>();
            return config;
        }

        // A FRESH ComponentRegistry with C and the suites' Table companions registered, so
        // a universal declaration always finds its descriptor and a Load always resolves.
        static std::shared_ptr<Astra::ComponentRegistry> MakeComponentRegistry()
        {
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<C, Astra::Test::Velocity, Astra::Test::Health>();
            return creg;
        }

        static std::unique_ptr<Astra::Registry> Make(Astra::Registry::Config config = {})
        {
            return std::make_unique<Astra::Registry>(MakeComponentRegistry(), MakeConfig(std::move(config)));
        }
    };

    template<class C> using TableV     = Variant<C, Astra::StorageKind::Table>;
    template<class C> using SparseV    = Variant<C, Astra::StorageKind::Sparse>;
    template<class C> using UniversalV = Variant<C, Astra::StorageKind::Universal>;

    // gtest typed-suite name generator: StorageParityPos/Table.*, /Sparse.*, /Universal.*
    struct VariantNames
    {
        template<class V>
        static std::string GetName(int) { return V::kName; }
    };

    // ---- Values --------------------------------------------------------------------

    // Position-shaped (Position, ParityPosS, TrackedPos, ParityTrackedS).
    template<class C>
    C MakePos(float v) { return C{v, v + 1.0f, v + 2.0f}; }

    // Hierarchy-shaped (Hierarchy, ParityHierS). Not aggregates (user-declared ctor).
    template<class C>
    C MakeHier(uint32_t v)
    {
        C c;
        c.parent = v;
        c.childCount = v + 1u;
        return c;
    }

    // "Give" C to an entity. Table/Sparse: AddComponent. Universal: the component is
    // already present (Add would be a no-op returning true, s3.3), so giving = writing.
    template<class V, class C>
    void Give(Astra::Registry& reg, Astra::Entity e, const C& value)
    {
        if constexpr (V::kUniversal)
        {
            C* slot = reg.GetComponent<C>(e);
            ASSERT_NE(slot, nullptr) << "a live entity must hold its universal component";
            *slot = value;
        }
        else
        {
            ASSERT_TRUE(reg.AddComponent(e, value));
        }
    }

    // Creates entities through `make` until one lands on `dead`'s index (ids are recycled
    // at version+1 since T1-C2), destroys the spill, and returns the new occupant (or
    // Invalid if the index never came back within 4096 creates).
    template<class MakeFn>
    Astra::Entity RecycleIndex(Astra::Registry& reg, Astra::Entity dead, MakeFn&& make)
    {
        std::vector<Astra::Entity> spill;
        for (int i = 0; i < 4096; ++i)
        {
            Astra::Entity e = make(reg);
            if (e.GetID() == dead.GetID())
            {
                for (Astra::Entity s : spill) reg.DestroyEntity(s);
                return e;
            }
            spill.push_back(e);
        }
        for (Astra::Entity s : spill) reg.DestroyEntity(s);
        return Astra::Entity::Invalid();
    }

    // ---- Assert capture (Universal refusal, iteration-safety guard) -----------------

    struct AssertCount
    {
        int count = 0;
        std::string lastMessage;
    };

    // Records and CONTINUES, so an ASTRA_ASSERT returns to the call site (which then
    // takes its Release path, e.g. `return false`). Install with
    // Astra::Testing::ScopedAssertHandler for one scope only: a globally installed
    // continue-handler would hide genuine failures elsewhere.
    inline Astra::AssertAction CountAndContinue(const Astra::AssertContext& ctx, void* user) noexcept
    {
        auto* c = static_cast<AssertCount*>(user);
        ++c->count;
        c->lastMessage = ctx.message ? ctx.message : "";
        return Astra::AssertAction::Continue;
    }

    // ASTRA_ASSERT is live exactly when NDEBUG is undefined (Core/Assert.hpp).
#ifndef NDEBUG
    inline constexpr bool kAssertsActive = true;
#else
    inline constexpr bool kAssertsActive = false;
#endif
} // namespace Astra::Test::Parity
```

- [ ] **Step 2: Write the Registry-surface parity suite**

Create `tests/Storage/StorageParityRegistryTest.cpp`. Its bodies are lifted from these suites:
- `tests/Registry/RegistryTest.cpp`: create-with, batch create, add/remove/get/has;
- `tests/Registry/ViewTest.cpp:491-587` and `tests/Commands/CommandBufferTest.cpp:735-761`: enableable behaviour and deferred `SetEnabled`;
- `tests/Registry/SignalLifetimeTest.cpp`: signal timing;
- `tests/Commands/CommandBufferTest.cpp`: the add/remove/emplace cases.

The file opens with the shared declarations and the Position-shaped suite:

```cpp
// Storage-parity suite, Registry surface (spec 2026-10-01 s9.1, D2). One body per
// behaviour, instantiated over Table / Sparse / Universal variants of the same component.
#include <algorithm>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "StorageParity.hpp"

using namespace Astra::Test::Parity;
using Astra::Test::Health;
using Astra::Test::Velocity;

// ======================= Position-shaped variants =======================

template<class V> class StorageParityPos : public ::testing::Test {};
using PosVariants = ::testing::Types<TableV<Astra::Test::Position>,
                                     SparseV<ParityPosS>,
                                     UniversalV<Astra::Test::Position>>;
TYPED_TEST_SUITE(StorageParityPos, PosVariants, VariantNames);

// Sanity: the variant really is in force (otherwise every parity body is vacuous).
TYPED_TEST(StorageParityPos, VariantIsInForce)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    const size_t archetypesBefore = reg->GetArchetypeCount();
    if constexpr (V::kUniversal)
    {
        EXPECT_TRUE(reg->HasComponent<C>(e));                    // always present
        EXPECT_NE(reg->GetComponent<C>(e), nullptr);
        EXPECT_EQ(reg->GetArchetypeCount(), archetypesBefore);   // not in any archetype mask
    }
    else
    {
        EXPECT_FALSE(reg->HasComponent<C>(e));
        ASSERT_TRUE(reg->AddComponent(e, MakePos<C>(1.0f)));
        if constexpr (V::kSparse)
            EXPECT_EQ(reg->GetArchetypeCount(), archetypesBefore);       // never fragments
        else
            EXPECT_EQ(reg->GetArchetypeCount(), archetypesBefore + 1);   // {Velocity, C} is new
    }
}
```

The same file continues with the create, add and remove bodies:

```cpp
TYPED_TEST(StorageParityPos, CreateWithValueThenGetAndHas)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{4.0f, 5.0f, 6.0f});
    ASSERT_TRUE(reg->IsValid(e));
    EXPECT_TRUE(reg->HasComponent<C>(e));
    C* c = reg->GetComponent<C>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, MakePos<C>(1.0f));          // Universal: a supplied value is never overwritten
    const Astra::Registry& cref = *reg;
    EXPECT_EQ(cref.GetComponent<C>(e), c);    // const and mutable access agree
    EXPECT_FLOAT_EQ(reg->GetComponent<Velocity>(e)->dx, 4.0f);
}

TYPED_TEST(StorageParityPos, AddAndEmplaceWhenPresentAreNoOpsReturningTrue)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    // Universal: present from creation, so even the first Add is the no-op (s3.3, s12.2).
    const C expected = V::kUniversal ? C{} : MakePos<C>(1.0f);

    EXPECT_TRUE(reg->AddComponent(e, MakePos<C>(1.0f)));
    EXPECT_TRUE(reg->HasComponent<C>(e));
    EXPECT_EQ(*reg->GetComponent<C>(e), expected);
    EXPECT_TRUE(reg->AddComponent(e, MakePos<C>(9.0f)));             // present: no-op, true
    EXPECT_EQ(*reg->GetComponent<C>(e), expected);
    EXPECT_TRUE(reg->EmplaceComponent<C>(e, 7.0f, 7.0f, 7.0f));    // present: no-op, true
    EXPECT_EQ(*reg->GetComponent<C>(e), expected);

    Astra::Entity f = reg->CreateEntity<Velocity>();
    EXPECT_TRUE(reg->EmplaceComponent<C>(f, 3.0f, 4.0f, 5.0f));
    EXPECT_EQ(*reg->GetComponent<C>(f), V::kUniversal ? C{} : C{3.0f, 4.0f, 5.0f});
}

TYPED_TEST(StorageParityPos, RemovePresentAbsentAndUniversalRefusal)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{});
    Astra::Entity keep = reg->CreateEntityWith(MakePos<C>(2.0f), Velocity{});
    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            EXPECT_FALSE(reg->RemoveComponent<C>(e));
            EXPECT_FALSE(reg->RemoveComponentByID(e, Astra::TypeID<C>::Value()));
        }
        if (kAssertsActive) EXPECT_GE(cap.count, 2);   // one refusal per call (Debug)
        else                EXPECT_EQ(cap.count, 0);
        EXPECT_TRUE(reg->HasComponent<C>(e));
        EXPECT_EQ(*reg->GetComponent<C>(e), MakePos<C>(1.0f));
    }
    else
    {
        EXPECT_TRUE(reg->RemoveComponent<C>(e));
        EXPECT_FALSE(reg->HasComponent<C>(e));
        EXPECT_EQ(reg->GetComponent<C>(e), nullptr);
        EXPECT_FALSE(reg->RemoveComponent<C>(e));                            // absent: false
        EXPECT_FALSE(reg->RemoveComponentByID(e, Astra::TypeID<C>::Value()));
        EXPECT_TRUE(reg->IsValid(e));
        EXPECT_NE(reg->GetComponent<Velocity>(e), nullptr);                  // other data untouched
    }
    EXPECT_EQ(*reg->GetComponent<C>(keep), MakePos<C>(2.0f));                // neighbour value intact
}

// Review Focus 1: a stale handle (destroy + recycle at version+1) must miss or be
// refused, and must never read or write the new occupant's element.
TYPED_TEST(StorageParityPos, StaleHandleAfterDestroyAndRecycleMisses)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity stale = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{});
    reg->DestroyEntity(stale);
    Astra::Entity occupant = RecycleIndex(*reg, stale, [](Astra::Registry& r) {
        return r.CreateEntityWith(MakePos<C>(5.0f), Velocity{});
    });
    ASSERT_TRUE(occupant.IsValid()) << "the destroyed index was never recycled";
    ASSERT_EQ(occupant.GetID(), stale.GetID());
    ASSERT_NE(occupant.GetVersion(), stale.GetVersion());

    EXPECT_FALSE(reg->HasComponent<C>(stale));
    EXPECT_EQ(reg->GetComponent<C>(stale), nullptr);
    EXPECT_FALSE(reg->HasComponentByHash(stale, Astra::TypeID<C>::Hash()));
    EXPECT_EQ(reg->GetComponentByHash(stale, Astra::TypeID<C>::Hash()), nullptr);
    EXPECT_FALSE(reg->AddComponent(stale, MakePos<C>(9.0f)));
    EXPECT_FALSE(reg->EmplaceComponent<C>(stale, 9.0f, 9.0f, 9.0f));
    {
        AssertCount cap;   // Universal: a remove is refused whatever the handle; never fatal here
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
        EXPECT_FALSE(reg->RemoveComponent<C>(stale));
        EXPECT_FALSE(reg->RemoveComponentByID(stale, Astra::TypeID<C>::Value()));
    }
    EXPECT_TRUE(reg->HasComponent<C>(occupant));
    EXPECT_EQ(*reg->GetComponent<C>(occupant), MakePos<C>(5.0f));   // the occupant is untouched
}

// Review Focus 1 (Clear): every pre-Clear handle is stale afterwards.
TYPED_TEST(StorageParityPos, ClearThenRecycleMisses)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    std::vector<Astra::Entity> before;
    for (int i = 0; i < 64; ++i) before.push_back(reg->CreateEntityWith(MakePos<C>(float(i)), Velocity{}));
    reg->Clear();
    EXPECT_EQ(reg->Size(), 0u);
    std::vector<Astra::Entity> after;
    for (int i = 0; i < 64; ++i) after.push_back(reg->CreateEntityWith(MakePos<C>(100.0f + i), Velocity{}));
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_FALSE(reg->HasComponent<C>(before[i])) << i;
        EXPECT_EQ(reg->GetComponent<C>(before[i]), nullptr) << i;
        EXPECT_FALSE(reg->AddComponent(before[i], MakePos<C>(-1.0f))) << i;
        EXPECT_EQ(*reg->GetComponent<C>(after[i]), MakePos<C>(100.0f + i)) << i;
    }
}
```

Next come the batch, by-id and reflection bodies:

```cpp
// Batches spanning several chunks (3000 x 12 B + Velocity), lifted from RegistryTest's
// BatchEntityCreation; survivors must keep their values through every swap-remove.
TYPED_TEST(StorageParityPos, BatchCreateAddEmplaceRemove)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 3000;
    std::vector<Astra::Entity> withC(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{withC}, [](size_t i) {
        return std::tuple{MakePos<C>(float(i)), Velocity{}};
    })), N);
    for (size_t i = 0; i < N; ++i) ASSERT_EQ(*reg->GetComponent<C>(withC[i]), MakePos<C>(float(i))) << i;

    std::vector<Astra::Entity> plain(N);
    ASSERT_EQ(reg->CreateEntities<Velocity>(N, std::span{plain}), N);
    reg->AddComponents(std::span{plain}, MakePos<C>(42.0f));
    for (Astra::Entity e : plain) ASSERT_EQ(*reg->GetComponent<C>(e), V::kUniversal ? C{} : MakePos<C>(42.0f));

    std::vector<Astra::Entity> emplaced(64);
    ASSERT_EQ(reg->CreateEntities<Velocity>(64, std::span{emplaced}), 64u);
    reg->EmplaceComponents<C>(std::span{emplaced}, 3.0f, 3.0f, 3.0f);
    for (Astra::Entity e : emplaced) ASSERT_EQ(*reg->GetComponent<C>(e), V::kUniversal ? C{} : C{3.0f, 3.0f, 3.0f});

    std::span<Astra::Entity> firstHalf(withC.data(), N / 2);
    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        size_t removed = 0;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            removed = reg->RemoveComponents<C>(firstHalf);
        }
        EXPECT_EQ(removed, 0u);
        if (kAssertsActive) EXPECT_GE(cap.count, 1);
        else                EXPECT_EQ(cap.count, 0);
        for (Astra::Entity e : firstHalf) EXPECT_TRUE(reg->HasComponent<C>(e));
    }
    else
    {
        EXPECT_EQ(reg->RemoveComponents<C>(firstHalf), N / 2);
        for (size_t i = 0; i < N; ++i)
        {
            ASSERT_EQ(reg->HasComponent<C>(withC[i]), i >= N / 2) << i;
            if (i >= N / 2) ASSERT_EQ(*reg->GetComponent<C>(withC[i]), MakePos<C>(float(i))) << i;
        }
        EXPECT_EQ(reg->RemoveComponents<C>(firstHalf), 0u);   // all absent now
    }
}

// The type-erased paths (CommandBuffer's funnel) and the reflection APIs (s3.3 last row):
// store membership must be merged into GetEntityComponents / InspectEntity / ByHash.
TYPED_TEST(StorageParityPos, ByIdPathsAndReflectionSeeTheComponent)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    const Astra::ComponentID id = Astra::TypeID<C>::Value();
    const uint64_t hash = Astra::TypeID<C>::Hash();
    const C value = MakePos<C>(3.0f);

    EXPECT_TRUE(reg->AddComponentByID(e, id, &value, sizeof(C)));
    const C expected = V::kUniversal ? C{} : value;
    ASSERT_TRUE(reg->HasComponentByHash(e, hash));
    ASSERT_NE(reg->GetComponentByHash(e, hash), nullptr);
    EXPECT_EQ(*static_cast<const C*>(reg->GetComponentByHash(e, hash)), expected);
    EXPECT_EQ(reg->GetComponentByHash(e, hash), static_cast<void*>(reg->GetComponent<C>(e)));

    auto countId = [&](const std::vector<const Astra::ComponentDescriptor*>& ds) {
        return std::count_if(ds.begin(), ds.end(), [&](const Astra::ComponentDescriptor* d) { return d && d->id == id; });
    };
    EXPECT_EQ(countId(reg->GetEntityComponents(e)), 1);
    const auto infos = reg->InspectEntity(e);
    auto it = std::find_if(infos.begin(), infos.end(), [&](const auto& ci) { return ci.descriptor && ci.descriptor->id == id; });
    ASSERT_NE(it, infos.end());
    EXPECT_EQ(it->data, static_cast<void*>(reg->GetComponent<C>(e)));

    if constexpr (!V::kUniversal)
    {
        EXPECT_TRUE(reg->RemoveComponentByID(e, id));
        EXPECT_FALSE(reg->HasComponentByHash(e, hash));
        EXPECT_EQ(reg->GetComponentByHash(e, hash), nullptr);
        EXPECT_EQ(countId(reg->GetEntityComponents(e)), 0);
        EXPECT_EQ(reg->AddComponentsByID(std::span{&e, 1}, id, &value, sizeof(C)), 1u);
        EXPECT_EQ(reg->RemoveComponentsByID(std::span{&e, 1}, id), 1u);
        EXPECT_FALSE(reg->HasComponent<C>(e));
    }
}
```

Then the signal and CommandBuffer bodies:

```cpp
// Signal timing and payload (s3.3 row "Signals", s7.4).
TYPED_TEST(StorageParityPos, SignalsFireOnGenuineChangeOnly)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    reg->EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    const Astra::ComponentID id = Astra::TypeID<C>::Value();
    std::vector<std::pair<Astra::Entity, C>> added, removed;
    reg->GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) {
            if (ev.componentId == id) added.emplace_back(ev.entity, *static_cast<const C*>(ev.component));
        });
    reg->GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev) {   // emitted BEFORE removal: read the live value
            if (ev.componentId == id) removed.emplace_back(ev.entity, *static_cast<const C*>(ev.component));
        });

    Astra::Entity e = reg->CreateEntity<Velocity>();
    if constexpr (V::kUniversal)
    {
        ASSERT_EQ(added.size(), 1u);   // Universal: ComponentAdded at create (s3.3)
        EXPECT_EQ(added[0].first, e);
        added.clear();
    }
    else
    {
        EXPECT_TRUE(added.empty());
    }

    ASSERT_TRUE(reg->AddComponent(e, MakePos<C>(1.0f)));
    if constexpr (V::kUniversal)
    {
        EXPECT_TRUE(added.empty());    // a no-op add is silent
    }
    else
    {
        ASSERT_EQ(added.size(), 1u);
        EXPECT_EQ(added[0].first, e);
        EXPECT_EQ(added[0].second, MakePos<C>(1.0f));
    }
    added.clear();
    ASSERT_TRUE(reg->AddComponent(e, MakePos<C>(2.0f)));   // present: silent no-op
    EXPECT_TRUE(added.empty());

    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            EXPECT_FALSE(reg->RemoveComponent<C>(e));
        }
        EXPECT_TRUE(removed.empty());  // refused: no signal
    }
    else
    {
        ASSERT_TRUE(reg->RemoveComponent<C>(e));
        ASSERT_EQ(removed.size(), 1u);
        EXPECT_EQ(removed[0].first, e);
        EXPECT_EQ(removed[0].second, MakePos<C>(1.0f));
        EXPECT_FALSE(reg->RemoveComponent<C>(e));
        EXPECT_EQ(removed.size(), 1u);
    }
    const size_t removedBeforeDestroy = removed.size();
    reg->DestroyEntity(e);
    EXPECT_EQ(removed.size(), removedBeforeDestroy);   // destroy emits no ComponentRemoved (today's rule)
}

// CommandBuffer record/execute (CommandBufferTest CreateEntityWithComponents / EmplaceComponent /
// BatchOperations / RemoveComponents / DestroyEntity), incl. the creation funnel (s7.1).
TYPED_TEST(StorageParityPos, CommandBufferPlaysBackIdentically)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::CommandBuffer cmd(reg.get());
    Astra::Entity t = cmd.CreateEntity();
    cmd.AddComponent(t, Velocity{1.0f, 0.0f, 0.0f});
    cmd.AddComponent(t, MakePos<C>(1.0f));
    Astra::Entity u = cmd.CreateEntity();
    cmd.EmplaceComponent<C>(u, 2.0f, 3.0f, 4.0f);
    std::vector<Astra::Entity> batch = {cmd.CreateEntity(), cmd.CreateEntity(), cmd.CreateEntity()};
    cmd.AddComponents<C>(batch, MakePos<C>(10.0f));
    ASSERT_TRUE(cmd.Execute().IsOk());

    ASSERT_TRUE(reg->IsValid(t));
    ASSERT_TRUE(reg->IsValid(u));
    EXPECT_TRUE(reg->HasComponent<C>(t));   // Universal: constructed by the funnel, never skipped
    EXPECT_TRUE(reg->HasComponent<C>(u));
    EXPECT_EQ(*reg->GetComponent<C>(t), V::kUniversal ? C{} : MakePos<C>(1.0f));
    EXPECT_EQ(*reg->GetComponent<C>(u), V::kUniversal ? C{} : C{2.0f, 3.0f, 4.0f});
    for (Astra::Entity b : batch) EXPECT_EQ(*reg->GetComponent<C>(b), V::kUniversal ? C{} : MakePos<C>(10.0f));

    if constexpr (!V::kUniversal)
    {
        Astra::CommandBuffer rm(reg.get());
        rm.RemoveComponent<C>(t);
        ASSERT_TRUE(rm.Execute().IsOk());
        EXPECT_FALSE(reg->HasComponent<C>(t));
        EXPECT_TRUE(reg->HasComponent<Velocity>(t));
    }
    Astra::CommandBuffer d(reg.get());
    d.DestroyEntity(u);
    ASSERT_TRUE(d.Execute().IsOk());
    EXPECT_FALSE(reg->IsValid(u));
    EXPECT_EQ(reg->GetComponent<C>(u), nullptr);
    for (Astra::Entity b : batch) EXPECT_TRUE(reg->HasComponent<C>(b));   // other holders unaffected
}
```

The enableable and tag suites close the file:

```cpp
// ======================= Enableable variants (ViewTest.cpp:491-587, CommandBufferTest.cpp:735-761) =======================

template<class V> class StorageParityEnableable : public ::testing::Test {};
using EnVariants = ::testing::Types<TableV<Astra::Test::Hierarchy>,
                                    SparseV<ParityHierS>,
                                    UniversalV<Astra::Test::Hierarchy>>;
TYPED_TEST_SUITE(StorageParityEnableable, EnVariants, VariantNames);

TYPED_TEST(StorageParityEnableable, BornEnabledToggleReportsAndSignalsOnChangeOnly)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakeHier<C>(7), Velocity{});
    EXPECT_TRUE(reg->IsEnabled<C>(e));

    reg->EnableSignals(Astra::Signal::ComponentEnabled | Astra::Signal::ComponentDisabled);
    const Astra::ComponentID id = Astra::TypeID<C>::Value();
    int enabledSignals = 0, disabledSignals = 0;
    reg->GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const Astra::Events::ComponentEnabled& ev) { if (ev.componentId == id) ++enabledSignals; });
    reg->GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const Astra::Events::ComponentDisabled& ev) { if (ev.componentId == id) ++disabledSignals; });

    EXPECT_TRUE(reg->SetEnabled<C>(e, false));
    EXPECT_FALSE(reg->IsEnabled<C>(e));
    EXPECT_EQ(disabledSignals, 1);
    EXPECT_TRUE(reg->SetEnabled<C>(e, false));      // idempotent: still true, silent
    EXPECT_EQ(disabledSignals, 1);
    EXPECT_TRUE(reg->HasComponent<C>(e));           // existence never lies
    EXPECT_EQ(reg->GetComponent<C>(e)->parent, 7u);
    EXPECT_TRUE(reg->SetEnabled<C>(e, true));
    EXPECT_TRUE(reg->IsEnabled<C>(e));
    EXPECT_EQ(enabledSignals, 1);
}

// Review Focus 1 (SetEnabled): a stale handle never toggles the new occupant.
TYPED_TEST(StorageParityEnableable, StaleAndMissingAreRefused)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity dead = reg->CreateEntityWith(MakeHier<C>(1), Velocity{});
    reg->DestroyEntity(dead);
    Astra::Entity occupant = RecycleIndex(*reg, dead, [](Astra::Registry& r) {
        return r.CreateEntityWith(MakeHier<C>(2), Velocity{});
    });
    ASSERT_TRUE(occupant.IsValid());
    ASSERT_EQ(occupant.GetID(), dead.GetID());
    EXPECT_FALSE(reg->SetEnabled<C>(dead, false));
    EXPECT_FALSE(reg->IsEnabled<C>(dead));
    EXPECT_TRUE(reg->IsEnabled<C>(occupant));
    EXPECT_EQ(reg->GetComponent<C>(occupant)->parent, 2u);
    if constexpr (!V::kUniversal)
    {
        Astra::Entity without = reg->CreateEntity<Velocity>();
        EXPECT_FALSE(reg->SetEnabled<C>(without, false));   // missing component
        EXPECT_FALSE(reg->IsEnabled<C>(without));
    }
}

// Disabled state and values survive every relocation kind: archetype moves (Health is
// Table), swap-removes (destroy) and compaction (Defragment); views agree.
TYPED_TEST(StorageParityEnableable, DisabledStateSurvivesRelocationAndViewsAgree)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 300;
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{es}, [](size_t i) {
        return std::tuple{MakeHier<C>(uint32_t(i)), Velocity{}};
    })), N);
    for (size_t i = 0; i < N; i += 3) ASSERT_TRUE(reg->SetEnabled<C>(es[i], false));
    for (int i : {64, 65, 127, 128}) (void)reg->SetEnabled<C>(es[size_t(i)], false);   // word boundaries
    for (size_t i = 0; i < N; i += 2) ASSERT_TRUE(reg->AddComponent(es[i], Health{}));
    for (size_t i = 0; i < N; i += 5) reg->DestroyEntity(es[i]);
    (void)reg->Defragment();

    auto disabled = [](size_t i) { return i % 3 == 0 || i == 64 || i == 65 || i == 127 || i == 128; };
    size_t alive = 0, enabled = 0;
    for (size_t i = 0; i < N; ++i)
    {
        if (i % 5 == 0) { EXPECT_FALSE(reg->IsValid(es[i])); continue; }
        ++alive;
        enabled += disabled(i) ? 0 : 1;
        EXPECT_EQ(reg->IsEnabled<C>(es[i]), !disabled(i)) << "entity " << i;
        EXPECT_EQ(reg->GetComponent<C>(es[i])->parent, uint32_t(i)) << "entity " << i;
    }
    size_t seen = 0;
    reg->CreateView<C>().ForEach([&](Astra::Entity, C&) { ++seen; });
    EXPECT_EQ(seen, enabled);
    EXPECT_EQ(reg->CreateView<C>().Size(), enabled);
    EXPECT_EQ(reg->CreateView<Astra::IncludeDisabled<C>>().Size(), alive);
    size_t nullOpt = 0;
    reg->CreateView<Velocity, Astra::Optional<C>>().ForEach([&](Astra::Entity, Velocity&, C* c) { nullOpt += (c == nullptr); });
    EXPECT_EQ(nullOpt, alive - enabled);             // a disabled Optional<T> yields null
}

TYPED_TEST(StorageParityEnableable, DeferredSetEnabledAppliesAtFlushAndStaleFails)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakeHier<C>(1), Velocity{});
    Astra::CommandBuffer cmd(reg.get());
    cmd.SetEnabled<C>(e, false);
    EXPECT_TRUE(reg->IsEnabled<C>(e));               // not yet applied
    ASSERT_TRUE(cmd.Execute().IsOk());
    EXPECT_FALSE(reg->IsEnabled<C>(e));

    Astra::Entity dead = reg->CreateEntityWith(MakeHier<C>(2), Velocity{});
    reg->DestroyEntity(dead);
    Astra::CommandBuffer stale(reg.get());
    stale.SetEnabled<C>(dead, false);
    EXPECT_TRUE(stale.Execute().IsErr());
}
```

```cpp
// ======================= Tag variants (enableable Table tag vs enableable Sparse tag) =======================
// s3.2: every tag may be enableable -- Table tags through a bit-only column (EnTag, Task 6),
// Sparse tags through the store (SpTag). Universal tags are degenerate (always present, no
// data) and are not parity-tested.

template<class V> class StorageParityTag : public ::testing::Test {};
using TagVariants = ::testing::Types<TableV<Astra::Test::EnTag>, SparseV<ParityTagS>>;
TYPED_TEST_SUITE(StorageParityTag, TagVariants, VariantNames);

TYPED_TEST(StorageParityTag, AddHasRemoveAndFilters)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    std::vector<Astra::Entity> es(100);
    ASSERT_EQ(reg->CreateEntities<Velocity>(100, std::span{es}), 100u);
    for (size_t i = 0; i < 100; i += 4) ASSERT_TRUE(reg->AddComponent(es[i], C{}));
    for (size_t i = 0; i < 100; ++i) EXPECT_EQ(reg->HasComponent<C>(es[i]), i % 4 == 0) << i;

    EXPECT_EQ(reg->CreateView<Velocity, C>().Size(), 25u);
    EXPECT_EQ(reg->CreateView<Velocity, Astra::Not<C>>().Size(), 75u);
    size_t n = 0;
    reg->CreateView<Velocity, Astra::With<C>>().ForEach([&](Astra::Entity, Velocity&) { ++n; });
    EXPECT_EQ(n, 25u);

    for (size_t i = 0; i < 100; i += 8) ASSERT_TRUE(reg->RemoveComponent<C>(es[i]));   // 13 removed
    EXPECT_EQ(reg->CreateView<Velocity, C>().Size(), 12u);
    EXPECT_FALSE(reg->RemoveComponent<C>(es[0]));
    EXPECT_TRUE(reg->HasComponentByHash(es[4], Astra::TypeID<C>::Hash()));
    EXPECT_NE(reg->GetComponentByHash(es[4], Astra::TypeID<C>::Hash()), nullptr);   // present tag: sentinel, never null
    EXPECT_FALSE(reg->HasComponentByHash(es[8], Astra::TypeID<C>::Hash()));

    // Enableable tags (s3.2): toggling never changes existence, only visibility.
    EXPECT_TRUE(reg->IsEnabled<C>(es[4]));
    EXPECT_TRUE(reg->SetEnabled<C>(es[4], false));
    EXPECT_FALSE(reg->IsEnabled<C>(es[4]));
    EXPECT_TRUE(reg->HasComponent<C>(es[4]));
    EXPECT_EQ(reg->CreateView<Velocity, C>().Size(), 11u);                          // disabled: filtered out
    EXPECT_EQ(reg->CreateView<Velocity, Astra::IncludeDisabled<C>>().Size(), 12u);
    EXPECT_FALSE(reg->SetEnabled<C>(es[8], false));                                  // absent: refused
    EXPECT_TRUE(reg->SetEnabled<C>(es[4], true));
    EXPECT_EQ(reg->CreateView<Velocity, C>().Size(), 12u);
}
```

- [ ] **Step 3: Regenerate the projects (two new test sources)**

Run from `D:\dev\starworks\Astra-storage`:
`D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`

Expected: `Done`, and `ide\AstraTest.vcxproj` now lists `tests\Storage\StorageParityRegistryTest.cpp`. Check it with:
`Select-String -Path ide\AstraTest.vcxproj -Pattern StorageParityRegistryTest`

- [ ] **Step 4: Build**

Run (PowerShell):
`msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`

Expected: `Build succeeded.` and `0 Error(s)`. The test project builds with `/WX`.

If a parity body does not COMPILE for one variant, the public API is not storage-invisible: a D2 violation. Fix the API, not the test. For example, a `static_assert` that refuses Sparse in some `View` path is such a violation.

- [ ] **Step 5: Run the parity suite**

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageParity*`

Expected:
- `[  PASSED  ] 44 tests` (the instantiated suites are counted separately per variant):
  - `StorageParityPos`: 10 bodies x 3 variants = 30;
  - `StorageParityEnableable`: 4 x 3 = 12;
  - `StorageParityTag`: 1 x 2 = 2.
- Names read like `StorageParityPos/Sparse.RemovePresentAbsentAndUniversalRefusal`.

Any failure is a defect in Tasks 1-36. Debug it per the chunk preamble, fix it, and re-run.

- [ ] **Step 6: TypeID budget check (full shuffled run)**

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`

Expected:
- `[  PASSED  ]` for every test;
- process exit code 0;
- record the `Randomizing tests' orders with a seed of N` line.

If the run dies with exit code 3 or a fatal `index < Bits` (`Bitmap.hpp`), the three new ids pushed the process past `ASTRA_MAX_COMPONENTS=192`. Then:
1. STOP. Do not raise the ceiling yourself. The 128 -> 192 raise was human-approved (`premake5.lua` comment).
2. Report the failing seed.
3. Propose the fix: raise the test-only define in `premake5.lua`'s `AstraTest` `defines` to `ASTRA_MAX_COMPONENTS=256`.

Before that, look for further reuse: a Sparse twin of Hierarchy or TrackedPos with the SAME fields added by Tasks 8-36 can replace `ParityHierS` / `ParityTrackedS` with a `using` alias, the way `ParityPosS` and `ParityTagS` already alias `PosS` and `SpTag`. That saves budget without asking.

- [ ] **Step 7: Commit**

```powershell
git add tests/Storage/StorageParity.hpp tests/Storage/StorageParityRegistryTest.cpp
git commit -m "test(storage): storage-parity suite, Registry surface -- the same bodies over Table / Sparse / Universal variants (add/emplace no-op, remove + Universal refusal, stale handles after destroy and Clear, batches, by-id + reflection, signals, CommandBuffer, enableable, tags)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 38: Storage-parity suite -- queries, iteration, change detection, serialization, Relations (s9.1)

**Files:**
- Create: `tests/Storage/StorageParityQueryTest.cpp`

**Interfaces:**
- Consumes everything Task 37's header produces.
- Consumes the query surface built by chunk D (Tasks 22-30):
  - `View` with required / `Not` / `With` / `Optional` / `Any` / `OneOf` / `IncludeDisabled` / `Changed` / `Added` arguments;
  - `Size`, `Contains`, `Get`, `Single`;
  - range-for;
  - `ParallelForEach`;
  - the iteration-safety guard, which chunk D extends to sparse structural changes;
  - Relations filters.
- Consumes format v6 Save/Load (Tasks 31-33).
- Produces: none (tests only).

**Lifted from:**
- `tests/Registry/ViewTest.cpp` (`Any`, combined modifiers, enableable filtering);
- `ViewEnrichmentTest.cpp` (`With`, `Contains`, `Get`, `Single`);
- `ViewIteratorTest.cpp` (range-for);
- `ParallelIterationTest.cpp`;
- `IterationSafetyTest.cpp` / `View.hpp:829-856` (the structural-mutation guard);
- `ChangeDetectionTest.cpp:943-1010` and `750-790`;
- `tests/Serialization/BinarySerializationTests.cpp:862-907` and `RegistrySerializationTest.cpp:601`;
- `RelationsTest.cpp:62-145`.

**What the bodies compare.** Iteration ORDER is unspecified for tables and is the dense order for sparse-driven views (s6.2), so bodies compare visited SETS, never sequences.

- [ ] **Step 1: Write the query parity suite**

Create `tests/Storage/StorageParityQueryTest.cpp`. It opens with the shared population and the presence filters:

```cpp
// Storage-parity suite, query surface (spec 2026-10-01 s9.1, D2).
#include <atomic>
#include <set>
#include <span>
#include <tuple>
#include <vector>

#include "StorageParity.hpp"
#include "../Support/TestWorkerPool.hpp"

using namespace Astra::Test::Parity;
using Astra::Test::Health;
using Astra::Test::Velocity;

namespace
{
    // n entities, all with Velocity{dx = i}; Health on even i; C "given" on i % 3 == 0
    // with MakePos<C>(i). Universal: every entity holds C; non-givens hold C{}.
    template<class V>
    struct Population
    {
        using C = typename V::Comp;
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> es;
        size_t n = 0;

        bool Given(size_t i) const { return i % 3 == 0; }
        bool HasC(size_t i) const { return V::kUniversal || Given(i); }
        static bool HasHealth(size_t i) { return i % 2 == 0; }
        C ValueOf(size_t i) const { return Given(i) ? MakePos<C>(float(i)) : C{}; }
        size_t Holders() const { size_t h = 0; for (size_t i = 0; i < n; ++i) h += HasC(i); return h; }
        std::set<Astra::Entity> Where(auto pred) const
        {
            std::set<Astra::Entity> s;
            for (size_t i = 0; i < n; ++i) if (pred(i)) s.insert(es[i]);
            return s;
        }
    };

    template<class V>
    Population<V> Populate(size_t n, Astra::Registry::Config cfg = {})
    {
        Population<V> p;
        p.reg = V::Make(std::move(cfg));
        p.n = n;
        p.es.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            p.es[i] = p.reg->CreateEntityWith(Velocity{float(i), 0.0f, 0.0f});
            if (Population<V>::HasHealth(i)) EXPECT_TRUE(p.reg->AddComponent(p.es[i], Health{}));
            if (p.Given(i)) Give<V>(*p.reg, p.es[i], MakePos<typename V::Comp>(float(i)));
        }
        return p;
    }

    void AdvanceTo(Astra::Registry& reg, Astra::Tick t)
    {
        while (reg.CurrentTick() < t) reg.AdvanceTick();
    }
}

template<class V> class StorageParityQuery : public ::testing::Test {};
using PosVariants = ::testing::Types<TableV<Astra::Test::Position>,
                                     SparseV<ParityPosS>,
                                     UniversalV<Astra::Test::Position>>;
TYPED_TEST_SUITE(StorageParityQuery, PosVariants, VariantNames);

// Archetype-driven (Velocity is Table): C becomes a chunk filter + per-entity fetch.
TYPED_TEST(StorageParityQuery, RequiredArgumentVisitsExactlyTheHolders)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(600);
    std::set<Astra::Entity> seen;
    p.reg->CreateView<Velocity, C>().ForEach([&](Astra::Entity e, Velocity& v, C& c) {
        seen.insert(e);
        const size_t i = size_t(v.dx);
        EXPECT_EQ(e, p.es[i]);
        EXPECT_EQ(c, p.ValueOf(i));
    });
    const auto expected = p.Where([&](size_t i) { return p.HasC(i); });
    EXPECT_EQ(seen, expected);
    auto view = p.reg->CreateView<Velocity, C>();
    EXPECT_EQ(view.Size(), expected.size());
    for (size_t i = 0; i < p.n; ++i) EXPECT_EQ(view.Contains(p.es[i]), p.HasC(i)) << i;
}

// No Table argument: sparse-driven for Sparse, every archetype for Universal (s6.2).
TYPED_TEST(StorageParityQuery, ViewOfOnlyTheComponentVisitsExactlyTheHolders)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(600);
    std::set<Astra::Entity> seen;
    p.reg->CreateView<const C>().ForEach([&](Astra::Entity e, const C& c) {
        seen.insert(e);
        EXPECT_EQ(c, *p.reg->GetComponent<C>(e));
    });
    EXPECT_EQ(seen, p.Where([&](size_t i) { return p.HasC(i); }));
    EXPECT_EQ(p.reg->CreateView<C>().Size(), p.Holders());
}

TYPED_TEST(StorageParityQuery, NotWithOptionalAnyOneOf)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(300);
    auto count = [&](auto view) { size_t k = 0; view.ForEach([&](Astra::Entity, Velocity&) { ++k; }); return k; };
    auto expectCount = [&](auto pred) { return p.Where(pred).size(); };

    EXPECT_EQ(count(p.reg->CreateView<Velocity, Astra::Not<C>>()), expectCount([&](size_t i) { return !p.HasC(i); }));
    EXPECT_EQ(count(p.reg->CreateView<Velocity, Astra::With<C>>()), expectCount([&](size_t i) { return p.HasC(i); }));
    EXPECT_EQ(count(p.reg->CreateView<Velocity, Astra::Any<C, Health>>()),
              expectCount([&](size_t i) { return p.HasC(i) || p.HasHealth(i); }));
    EXPECT_EQ(count(p.reg->CreateView<Velocity, Astra::OneOf<C, Health>>()),
              expectCount([&](size_t i) { return p.HasC(i) != p.HasHealth(i); }));

    size_t visited = 0;
    p.reg->CreateView<Velocity, Astra::Optional<C>>().ForEach([&](Astra::Entity, Velocity& v, C* c) {
        ++visited;
        const size_t i = size_t(v.dx);
        EXPECT_EQ(c != nullptr, p.HasC(i)) << i;
        if (c) EXPECT_EQ(*c, p.ValueOf(i)) << i;
    });
    EXPECT_EQ(visited, p.n);
}
```

The file continues with random access, range-for, parallel iteration and the iteration-safety guard:

```cpp
TYPED_TEST(StorageParityQuery, GetSingleContainsAgreeWithForEach)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto p = Populate<V>(30);
    auto v = p.reg->CreateView<Velocity, C>();
    for (size_t i = 0; i < p.n; ++i)
    {
        auto r = v.Get(p.es[i]);
        ASSERT_EQ(r.IsOk(), p.HasC(i)) << i;
        if (r.IsOk())
        {
            auto [vel, c] = *r.GetValue();
            EXPECT_EQ(*c, p.ValueOf(i));
            EXPECT_EQ(c, p.reg->GetComponent<C>(p.es[i]));   // pointer identity with GetComponent
            (void)vel;
        }
        else
        {
            EXPECT_EQ(*r.GetError(), Astra::QueryError::NotMatched);
        }
    }

    // Single: exactly one Velocity+C holder (the others carry Health only).
    auto reg = V::Make();
    Astra::Entity only = reg->CreateEntityWith(Velocity{});
    Give<V>(*reg, only, MakePos<C>(7.0f));
    for (int i = 0; i < 4; ++i) (void)reg->CreateEntityWith(Health{});
    auto sv = reg->CreateView<Velocity, C>();
    {
        auto r = sv.Single();
        ASSERT_TRUE(r.IsOk());
        auto [vel, c] = *r.GetValue();
        (void)vel;
        EXPECT_EQ(*c, MakePos<C>(7.0f));
    }
    Astra::Entity second = reg->CreateEntityWith(Velocity{});
    Give<V>(*reg, second, MakePos<C>(8.0f));
    auto r2 = sv.Single();
    ASSERT_TRUE(r2.IsErr());
    EXPECT_EQ(*r2.GetError(), Astra::QueryError::MultipleMatched);
    reg->DestroyEntity(second);
    EXPECT_FALSE(sv.Contains(second));                       // stale handle
    EXPECT_TRUE(sv.Get(second).IsErr());
}

// s6.5: a required Sparse argument must NOT trigger the range-for refusal (a Table one never does).
TYPED_TEST(StorageParityQuery, RangeForVisitsTheSameSetAsForEach)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(400);
    auto view = p.reg->CreateView<Velocity, C>();
    std::set<Astra::Entity> seen;
    for (auto [e, vel, c] : view)
    {
        seen.insert(e);
        EXPECT_EQ(c, p.ValueOf(size_t(vel.dx)));
    }
    EXPECT_EQ(seen, p.Where([&](size_t i) { return p.HasC(i); }));
}

TYPED_TEST(StorageParityQuery, ParallelForEachOnARealSchedulerVisitsEveryHolderOnce)
{
    using C = typename TypeParam::Comp;
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    auto p = Populate<TypeParam>(30'000, cfg);

    std::atomic<size_t> visits{0};
    p.reg->CreateView<Velocity, C>().ParallelForEach([&](Astra::Entity, Velocity&, C& c) {
        c.z += 1.0f;                                         // each entity owns its slot in every store
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visits.load(), p.Holders());
    for (size_t i = 0; i < p.n; ++i)
        if (p.HasC(i)) ASSERT_FLOAT_EQ(p.reg->GetComponent<C>(p.es[i])->z, p.ValueOf(i).z + 1.0f) << i;

    std::atomic<size_t> onlyC{0};                            // sparse-driven partitioning (s6.5)
    p.reg->CreateView<const C>().ParallelForEach([&](Astra::Entity, const C&) { onlyC.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(onlyC.load(), p.Holders());
}

// Parity of the Debug structural-mutation guard (View.hpp:829-856, extended by Task 25).
// Integration ruling (D vs F): parity means "same outcome as Table today", NOT new guard
// semantics. Today's guard trips on an archetype-SET change (a never-seen archetype is
// created); Task 25 adds the sparse membership counter. So, for every kind:
//   - an archetype-set change inside ForEach trips it identically;
//   - a Sparse add/remove trips it through the sparse counter;
//   - a deferred (CommandBuffer) change never trips it.
// Destroying a bystander, and a move into an EXISTING archetype, do not trip it for Table
// today, so they are deliberately asserted for no kind.
TYPED_TEST(StorageParityQuery, StructuralChangeDuringForEachTripsTheGuard)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the structural-mutation guard is a Debug-only ASTRA_ASSERT";
#else
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity iterated = reg->CreateEntityWith(Velocity{});      // the view's ONLY match
    Give<V>(*reg, iterated, MakePos<C>(1.0f));
    Astra::Entity bystander = reg->CreateEntityWith(Health{});       // outside the iterated archetype
    Astra::Entity holder = reg->CreateEntityWith(Health{});
    Give<V>(*reg, holder, MakePos<C>(2.0f));

    auto runAndCount = [&](auto&& mutate) {
        AssertCount cap;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            reg->CreateView<Velocity, C>().ForEach([&](Astra::Entity, Velocity&, C&) { mutate(); });
        }
        return cap.count;
    };

    // {Health, Velocity} has never existed in this registry, for any kind: an archetype-set change.
    EXPECT_GE(runAndCount([&] { (void)reg->AddComponent(bystander, Velocity{}); }), 1);   // every kind
    if constexpr (V::kSparse)
    {
        // Sparse membership changes never touch the archetype set; the sparse counter catches them.
        Astra::Entity lacking = reg->CreateEntityWith(Health{});
        EXPECT_GE(runAndCount([&] { (void)reg->AddComponent(lacking, MakePos<C>(3.0f)); }), 1);
        EXPECT_GE(runAndCount([&] { (void)reg->RemoveComponent<C>(holder); }), 1);
    }
    Astra::Entity later = reg->CreateEntityWith(Health{});
    Astra::CommandBuffer cmd(reg.get());
    EXPECT_EQ(runAndCount([&] { cmd.DestroyEntity(later); }), 0);                    // deferred: never trips
    ASSERT_TRUE(cmd.Execute().IsOk());
    EXPECT_FALSE(reg->IsValid(later));
#endif
}
```

Then the serialization and Relations bodies:

```cpp
// Save -> Load into a registry with the SAME storage assignment (raw and LZ4). Cross-storage
// migration (Table<->Sparse/Universal of one type) is pinned by Tasks 33-34.
TYPED_TEST(StorageParityQuery, SaveLoadRoundTripKeepsValuesAndMembership)
{
    using V = TypeParam;
    using C = typename V::Comp;
    for (Astra::CompressionMode mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        SCOPED_TRACE(mode == Astra::CompressionMode::LZ4 ? "LZ4" : "raw");
        auto p = Populate<V>(3000);
        Astra::Registry::SaveConfig sc;
        sc.compressionMode = mode;
        auto saved = p.reg->Save(sc);
        ASSERT_TRUE(saved.IsOk());
        auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()),
                                            V::MakeComponentRegistry(), V::MakeConfig());
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& r = **loaded.GetValue();
        ASSERT_EQ(r.Size(), p.n);
        for (size_t i = 0; i < p.n; ++i)
        {
            ASSERT_EQ(r.HasComponent<C>(p.es[i]), p.HasC(i)) << i;
            if (p.HasC(i)) ASSERT_EQ(*r.GetComponent<C>(p.es[i]), p.ValueOf(i)) << i;
            ASSERT_EQ(r.HasComponent<Health>(p.es[i]), p.HasHealth(i)) << i;
        }
        EXPECT_EQ(r.CreateView<Velocity, C>().Size(), p.Holders());
        EXPECT_EQ(r.CreateView<Velocity, Astra::Not<C>>().Size(), p.n - p.Holders());
    }
}

TYPED_TEST(StorageParityQuery, RelationsFiltersSeeTheComponent)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity parent = reg->CreateEntity<Velocity>();
    for (int i = 0; i < 6; ++i)
    {
        Astra::Entity kid = reg->CreateEntity<Velocity>();
        if (i % 2 == 0) Give<V>(*reg, kid, MakePos<C>(float(i)));
        reg->SetParent(kid, parent);
    }
    const size_t holders = V::kUniversal ? 6u : 3u;
    EXPECT_EQ(reg->GetRelations<C>(parent).GetChildren().size(), holders);
    EXPECT_EQ(reg->GetRelations<Astra::Not<C>>(parent).GetChildren().size(), 6u - holders);
    size_t visited = 0;
    float sumX = 0.0f;
    reg->GetRelations<C>(parent).ForEachChild([&](Astra::Entity, C& c) { ++visited; sumX += c.x; });
    EXPECT_EQ(visited, holders);
    EXPECT_FLOAT_EQ(sumX, 0.0f + 2.0f + 4.0f);   // Universal non-givens hold C{} (x == 0)
}
```

The change-tracked suite follows:

```cpp
// ======================= Change-tracked variants (ChangeDetectionTest.cpp:750-1010) =======================
// Untracked components are chunk-coarse for tables BY DESIGN (a superset), so exact
// parity is defined for change-TRACKED components only: per-entity ticks in every store.

template<class V> class StorageParityTracked : public ::testing::Test {};
using TrackedVariants = ::testing::Types<TableV<Astra::Test::TrackedPos>,
                                         SparseV<ParityTrackedS>,
                                         UniversalV<Astra::Test::TrackedPos>>;
TYPED_TEST_SUITE(StorageParityTracked, TrackedVariants, VariantNames);

TYPED_TEST(StorageParityTracked, ChangedAndAddedArePerEntity)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    AdvanceTo(*reg, 2);
    std::vector<Astra::Entity> ents(500);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(500, std::span{ents}, [](size_t) {
        return std::tuple{C{}, Velocity{}};
    })), 500u);
    AdvanceTo(*reg, 3);
    const std::vector<size_t> marked = {0, 3, 64, 65, 127, 300, 499};
    std::set<Astra::Entity> expected;
    for (size_t i : marked) { reg->GetComponent<C>(ents[i])->x = 1.0f; expected.insert(ents[i]); }
    Astra::Entity fresh = reg->CreateEntityWith(C{}, Velocity{});          // added == 3

    auto changed = reg->CreateView<const C, Astra::Changed<C>>();          // sparse-driven for Sparse
    std::set<Astra::Entity> seen;
    changed.Since(2).ForEach([&](Astra::Entity e, const C&) { seen.insert(e); });
    std::set<Astra::Entity> expectedChanged = expected;
    expectedChanged.insert(fresh);
    EXPECT_EQ(seen, expectedChanged);
    size_t n3 = 0, n0 = 0;
    changed.Since(3).ForEach([&](Astra::Entity, const C&) { ++n3; });
    changed.Since(0).ForEach([&](Astra::Entity, const C&) { ++n0; });
    EXPECT_EQ(n3, 0u);
    EXPECT_EQ(n0, 501u);

    auto hot = reg->CreateView<const Velocity, const C, Astra::Changed<C>>();   // archetype-driven
    seen.clear();
    hot.Since(2).ForEach([&](Astra::Entity e, const Velocity&, const C&) { seen.insert(e); });
    EXPECT_EQ(seen, expectedChanged);

    auto added = reg->CreateView<const C, Astra::Added<C>>();
    seen.clear();
    added.Since(2).ForEach([&](Astra::Entity e, const C&) { seen.insert(e); });
    EXPECT_EQ(seen, std::set<Astra::Entity>{fresh});
}

TYPED_TEST(StorageParityTracked, MutModifiedAndSetIfNeqMarkExactly)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    AdvanceTo(*reg, 2);
    Astra::Entity e = reg->CreateEntityWith(C{1.0f, 2.0f, 3.0f}, Velocity{});
    AdvanceTo(*reg, 3);
    reg->CreateView<C>().ForEach([](Astra::Mut<C> c) { (void)c.Read(); });   // Read never marks
    EXPECT_FALSE(reg->IsChanged<C>(e, 2));
    AdvanceTo(*reg, 4);
    reg->CreateView<C>().ForEach([](Astra::Mut<C> c) { c->y = 9.0f; });      // operator-> marks
    EXPECT_TRUE(reg->IsChanged<C>(e, 3));
    AdvanceTo(*reg, 5);
    EXPECT_FALSE(reg->SetIfNeq(e, C{1.0f, 9.0f, 3.0f}));                      // equal: no mark
    EXPECT_FALSE(reg->IsChanged<C>(e, 4));
    EXPECT_TRUE(reg->SetIfNeq(e, C{7.0f, 9.0f, 3.0f}));
    EXPECT_TRUE(reg->IsChanged<C>(e, 4));
    AdvanceTo(*reg, 6);
    EXPECT_TRUE(reg->Modified<C>(e));
    EXPECT_TRUE(reg->IsChanged<C>(e, 5));
    EXPECT_TRUE(reg->IsAdded<C>(e, 1));
    EXPECT_FALSE(reg->IsAdded<C>(e, 2));
}
```

The enableable query suite closes the file:

```cpp
// ======================= Enableable filtering through queries + round trip =======================

template<class V> class StorageParityEnableableQuery : public ::testing::Test {};
using EnVariants = ::testing::Types<TableV<Astra::Test::Hierarchy>,
                                    SparseV<ParityHierS>,
                                    UniversalV<Astra::Test::Hierarchy>>;
TYPED_TEST_SUITE(StorageParityEnableableQuery, EnVariants, VariantNames);

TYPED_TEST(StorageParityEnableableQuery, DefaultViewsSkipDisabledAndBitsSurviveSaveLoad)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 1000;   // 16 B x 1000 clears the 1024-byte LZ4 threshold
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{es}, [](size_t i) {
        return std::tuple{MakeHier<C>(uint32_t(i)), Velocity{float(i), 0.0f, 0.0f}};
    })), N);
    auto disabled = [](size_t i) { return i % 3 == 0 || i == 63 || i == 64 || i == 65 || i == 127 || i == 128; };
    for (size_t i = 0; i < N; ++i) if (disabled(i)) ASSERT_TRUE(reg->SetEnabled<C>(es[i], false));
    size_t enabled = 0;
    for (size_t i = 0; i < N; ++i) enabled += disabled(i) ? 0 : 1;

    auto check = [&](Astra::Registry& r) {
        std::set<Astra::Entity> seen;
        r.CreateView<Velocity, C>().ForEach([&](Astra::Entity e, Velocity& v, C& c) {
            seen.insert(e);
            EXPECT_FALSE(disabled(size_t(v.dx)));
            EXPECT_EQ(c.parent, uint32_t(v.dx));
        });
        EXPECT_EQ(seen.size(), enabled);
        EXPECT_EQ(r.CreateView<C>().Size(), enabled);
        EXPECT_EQ(r.CreateView<Astra::IncludeDisabled<C>>().Size(), N);
        EXPECT_EQ(r.CreateView<Velocity, Astra::IncludeDisabled<C>>().Size(), N);
        for (size_t i = 0; i < N; ++i) ASSERT_EQ(r.IsEnabled<C>(es[i]), !disabled(i)) << i;
        auto v = r.CreateView<Velocity, C>();
        EXPECT_FALSE(v.Contains(es[0]));                     // disabled -> invisible to random access
        EXPECT_TRUE(v.Contains(es[1]));
        EXPECT_TRUE(v.Get(es[1]).IsOk());
        EXPECT_TRUE(v.Get(es[0]).IsErr());
    };
    check(*reg);

    Astra::Registry::SaveConfig sc;
    sc.compressionMode = Astra::CompressionMode::LZ4;
    auto saved = reg->Save(sc);
    ASSERT_TRUE(saved.IsOk());
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()),
                                        V::MakeComponentRegistry(), V::MakeConfig());
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    check(**loaded.GetValue());
}
```

- [ ] **Step 2: Regenerate, build, run the suite**

Run from `D:\dev\starworks\Astra-storage`, in order:
1. `D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`
2. `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
3. `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageParity*`

Expected: `Build succeeded.`, then `[  PASSED  ] 80 tests`:
- 44 from Task 37;
- `StorageParityQuery`: 9 bodies x 3 = 27;
- `StorageParityTracked`: 2 x 3 = 6;
- `StorageParityEnableableQuery`: 1 x 3 = 3.

The Universal and Table rows must pass with no special-casing beyond the `kUniversal` branches already in the bodies.

If `RangeForVisitsTheSameSetAsForEach` fails to COMPILE for the Sparse variant, chunk D did not lift the range-for refusal for required Sparse arguments (s6.5). Fix `View::begin()/end()`'s `static_assert` there. Do not drop the test.

- [ ] **Step 3: Full shuffled run**

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`

Expected:
- `[  PASSED  ]` for all tests;
- exit code 0;
- record the seed line.

- [ ] **Step 4: Commit**

```powershell
git add tests/Storage/StorageParityQueryTest.cpp
git commit -m "test(storage): storage-parity suite, query surface -- required/Not/With/Optional/Any/OneOf, sparse-driven views, Get/Single/Contains, range-for, parallel, the structural-mutation guard, tracked Changed/Added/Mut, Save/Load raw+LZ4, Relations filters, enableable filtering -- over Table / Sparse / Universal" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 39: Storage invariant fuzzer (s9.2)

**Files:**
- Create: `tests/Storage/StorageInvariantFuzzTest.cpp`

**Interfaces:**
- Consumes `Registry::ValidateStorageInvariants() const` (Task 16, every configuration). It returns an empty string when P1-P3 hold.
- Consumes `Config::AddUniversal<T>(UniversalInitFn, void*)` (Task 5).
- Consumes the Task 37 header's Sparse twins.
- Consumes every structural entry point: create, destroy, batch create/destroy, add, emplace, remove (typed, by-id, batch), `SetEnabled`, `Defragment`, `Clear`, Save -> Load, and CommandBuffer playback.
- Produces two tests:
  - `StorageInvariantFuzz.FixedSeedsShortRun`, in the normal suite;
  - `StorageInvariantFuzz.DISABLED_LongRun`, the long run behind its own filter.

**The component mix (s9.2).** No new types: the Sparse ones come from Task 37.

| Role | Type | Kind |
|---|---|---|
| Table data | `Velocity` | Table |
| Table enableable | `Hierarchy` | Table (data column + disabled words) |
| Table tag | `Player` | Table (no column) |
| Table enableable tag | `EnTag` (Task 6) | Table (bit-only column) |
| Sparse data | `ParityPosS` | Sparse |
| Sparse enableable | `ParityHierS` | Sparse |
| Sparse enableable tag | `ParityTagS` (= chunk C's `SpTag`) | Sparse |
| Sparse non-trivial, lifetime-counted | `ParityLiveS` | Sparse |
| Universal non-trivial with `init` | `Name` (`std::string`, the Identity shape) | Universal |
| Universal enableable | `Timer` | Universal |

**The reference model.** One `ModelEntity` per live entity (`std::unordered_map<Entity, ModelEntity>`). After EVERY operation, the fuzzer checks all of the following:
1. `ValidateStorageInvariants()` is empty (P1-P3).
2. No unexpected assert fired. Universal removes are the only expected ones.
3. `Registry::Size()` equals the model size.
4. `ParityLiveS::s_live` equals its baseline plus the model's `ParityLiveS` count: no leak and no double destroy.
5. Every model entity matches the registry, component by component: presence, value and enabled state, for all ten types.
6. Ten view sizes equal the model's counts: sparse-driven, archetype-driven with a sparse filter, `Not<sparse tag>`, enableable sparse, `IncludeDisabled`, Universal enableable, a tag-only view, `Optional`, the Table enableable, and the enableable Table tag.

**Determinism and reporting.**
- Seeds are printed (`[ fuzz ] seed=N ops=M`).
- The first divergence stops that seed and reports the seed, the op index, the op name and the mismatch.
- An unexpected `ASTRA_ASSERT` prints the seed and op to stderr before the default abort, so even a fatal crash is reproducible.

- [ ] **Step 1: Write the fuzzer**

Create `tests/Storage/StorageInvariantFuzzTest.cpp`. It opens with the model, the universal config and the fuzzer's state:

```cpp
// Storage invariant fuzzer (spec 2026-10-01 s9.2): seeded random operation sequences over
// Table / Sparse / Sparse-enableable / Sparse-tag / Universal components against a reference
// model; ValidateStorageInvariants (P1-P3) + a full model comparison after EVERY operation.
//
// Normal suite:  StorageInvariantFuzz.FixedSeedsShortRun (4 fixed seeds x 1500 ops).
// Long run:      --gtest_also_run_disabled_tests --gtest_filter=StorageInvariantFuzz.DISABLED_LongRun
//                env ASTRA_FUZZ_SEED (first seed, default 1000), ASTRA_FUZZ_SEEDS (count, 64),
//                ASTRA_FUZZ_OPS (ops per seed, 20000). Reproduce one seed with ASTRA_FUZZ_SEEDS=1.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "StorageParity.hpp"

using namespace Astra::Test::Parity;
using Astra::Test::EnTag;
using Astra::Test::Hierarchy;
using Astra::Test::Name;
using Astra::Test::Player;
using Astra::Test::Timer;
using Astra::Test::Velocity;

namespace
{
    constexpr size_t kMaxAlive = 400;

    struct ModelEntity
    {
        std::optional<Velocity> vel;
        std::optional<Hierarchy> hier;
        bool hierEnabled = true;
        bool player = false;
        bool enTag = false;
        bool enTagEnabled = true;
        std::optional<ParityPosS> pos;
        std::optional<ParityHierS> hs;
        bool hsEnabled = true;
        bool tag = false;
        std::optional<int> live;
        std::string name;                 // Universal: always present
        float timerDuration = 1.0f;       // Universal: Timer{} has duration 1.0
        bool timerEnabled = true;
    };

    std::string InitName(Astra::Entity e) { return "init" + std::to_string(e.GetID()); }

    // The registry's universal init: runs only when no Name value was supplied (s4.2).
    void NameInit(Astra::Entity e, void* component, void* /*user*/)
    {
        static_cast<Name*>(component)->value = InitName(e);
    }

    Astra::Registry::Config FuzzConfig()
    {
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>(&NameInit, nullptr);
        cfg.AddUniversal<Timer>();
        return cfg;
    }

    std::shared_ptr<Astra::ComponentRegistry> FuzzComponents()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Velocity, Hierarchy, Player, ParityPosS, ParityHierS,
                                 ParityTagS, ParityLiveS, Name, Timer, EnTag>();
        return creg;
    }

    uint64_t EnvU64(const char* name, uint64_t fallback)
    {
#ifdef _MSC_VER
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return fallback;
        const uint64_t parsed = std::strtoull(value, nullptr, 0);
        std::free(value);
        return parsed;
#else
        const char* value = std::getenv(name);
        return value ? std::strtoull(value, nullptr, 0) : fallback;
#endif
    }

    class StorageFuzzer
    {
    public:
        explicit StorageFuzzer(uint64_t seed)
            : m_seed(seed), m_rng(seed), m_creg(FuzzComponents()),
              m_reg(std::make_unique<Astra::Registry>(m_creg, FuzzConfig())),
              m_liveBase(ParityLiveS::s_live)
        {}

        // Returns false at the first divergence (a gtest failure has been recorded).
        bool Run(size_t ops)
        {
            Astra::Testing::ScopedAssertHandler guard(&OnAssert, this);
            for (m_opIndex = 0; m_opIndex < ops; ++m_opIndex)
            {
                m_op = Step();
                if (!Check()) return false;
            }
            return true;
        }

        // Destroys the registry: every ParityLiveS it held must be destructed exactly once.
        void Finish()
        {
            m_reg.reset();
            EXPECT_EQ(ParityLiveS::s_live, m_liveBase) << "seed " << m_seed << ": ParityLiveS leaked or double-destroyed";
        }

    private:
        uint64_t m_seed;
        std::mt19937_64 m_rng;
        std::shared_ptr<Astra::ComponentRegistry> m_creg;
        std::unique_ptr<Astra::Registry> m_reg;
        int m_liveBase;
        std::unordered_map<Astra::Entity, ModelEntity> m_model;
        std::vector<Astra::Entity> m_alive;
        std::vector<Astra::Entity> m_dead;
        size_t m_opIndex = 0;
        const char* m_op = "";
        uint64_t m_counter = 0;
        bool m_expectRefusal = false;
        int m_refusals = 0;
        int m_unexpectedAsserts = 0;
        std::string m_lastUnexpected;
```

The class continues with its helpers, the op dispatcher, and the create/destroy/add ops:

```cpp
        // Expected refusals (Universal removes) count and continue; anything else is a
        // defect: print the reproduction line FIRST (an ASTRA_ASSERT aborts after Break).
        static Astra::AssertAction OnAssert(const Astra::AssertContext& ctx, void* user) noexcept
        {
            auto* self = static_cast<StorageFuzzer*>(user);
            if (self->m_expectRefusal)
            {
                ++self->m_refusals;
                return Astra::AssertAction::Continue;
            }
            ++self->m_unexpectedAsserts;
            self->m_lastUnexpected = ctx.message ? ctx.message : (ctx.expression ? ctx.expression : "?");
            std::fprintf(stderr, "[ fuzz ] UNEXPECTED ASSERT seed=%" PRIu64 " op#%zu (%s): %s\n",
                         self->m_seed, self->m_opIndex, self->m_op, self->m_lastUnexpected.c_str());
            std::fflush(stderr);
            return Astra::AssertAction::Break;
        }

        uint32_t Rand(size_t n) { return static_cast<uint32_t>(m_rng() % (n ? n : 1)); }
        bool Chance(uint32_t pct) { return Rand(100) < pct; }
        float F() { return static_cast<float>(Rand(1000)); }
        Astra::Entity PickAlive() { return m_alive[Rand(m_alive.size())]; }

        std::vector<Astra::Entity> PickDistinct(size_t k)
        {
            std::vector<Astra::Entity> pool = m_alive;
            std::shuffle(pool.begin(), pool.end(), m_rng);
            pool.resize(std::min(k, pool.size()));
            return pool;
        }

        void Track(Astra::Entity e, ModelEntity m)
        {
            ASSERT_TRUE(e.IsValid()) << "create returned Invalid (seed " << m_seed << ")";
            m_model.emplace(e, std::move(m));
            m_alive.push_back(e);
        }

        void Untrack(Astra::Entity e)
        {
            m_model.erase(e);
            auto it = std::find(m_alive.begin(), m_alive.end(), e);
            if (it != m_alive.end()) { *it = m_alive.back(); m_alive.pop_back(); }
            NoteDead(e);
        }

        // Only the 128 most recent dead handles are probed: an index recycled ~2^8 times
        // (8 version bits) could legitimately make a very old handle compare live again.
        void NoteDead(Astra::Entity e)
        {
            m_dead.push_back(e);
            if (m_dead.size() > 128) m_dead.erase(m_dead.begin());
        }

        const char* Step()
        {
            const uint32_t r = Rand(1000);
            if (m_alive.empty() || r < 120) return OpCreate();
            if (r < 170) return OpBatchCreate();
            if (r < 250) return OpDestroy();
            if (r < 270) return OpBatchDestroy();
            if (r < 470) return OpAdd();
            if (r < 620) return OpRemove();
            if (r < 700) return OpEnable();
            if (r < 790) return OpWrite();
            if (r < 840) return OpBatchSparse();
            if (r < 870) return OpCommandBuffer();
            if (r < 890) return OpStaleProbe();
            if (r < 920) { (void)m_reg->Defragment(); return "defragment"; }
            if (r < 927) return OpSaveLoad();
            if (r < 930) return OpClear();
            return OpRangeFor();
        }

        const char* OpCreate()
        {
            if (m_alive.size() >= kMaxAlive) return OpDestroy();
            ModelEntity m;
            Astra::Entity e;
            bool nameGiven = false;
            switch (Rand(4))
            {
            case 0:
                e = m_reg->CreateEntity();
                break;
            case 1:
            {
                const Velocity v{F(), F(), F()};
                const ParityPosS p{F(), F(), F()};
                e = m_reg->CreateEntityWith(v, p);
                m.vel = v;
                m.pos = p;
                break;
            }
            case 2:
            {
                std::string n = "given" + std::to_string(m_counter++);
                e = m_reg->CreateEntityWith(Name{n}, ParityTagS{}, Hierarchy{});   // explicit Name: init must NOT run
                m.name = n;
                nameGiven = true;
                m.tag = true;
                m.hier = Hierarchy{};
                break;
            }
            default:
                e = m_reg->CreateEntity<Velocity, ParityHierS, Player>();
                m.vel = Velocity{};
                m.hs = ParityHierS{};
                m.player = true;
                break;
            }
            if (!nameGiven) m.name = InitName(e);
            Track(e, std::move(m));
            return "create";
        }

        const char* OpBatchCreate()
        {
            if (m_alive.size() >= kMaxAlive) return OpBatchDestroy();
            const size_t k = 1 + Rand(48);
            std::vector<Astra::Entity> out(k);
            if (Chance(50))
            {
                const float base = F();
                const size_t made = m_reg->CreateEntitiesWith<Velocity, ParityPosS>(k, std::span{out}, [&](size_t i) {
                    return std::tuple{Velocity{base + float(i), 0.0f, 0.0f}, ParityPosS{base, float(i), 0.0f}};
                });
                EXPECT_EQ(made, k);
                for (size_t i = 0; i < made; ++i)
                {
                    ModelEntity m;
                    m.vel = Velocity{base + float(i), 0.0f, 0.0f};
                    m.pos = ParityPosS{base, float(i), 0.0f};
                    m.name = InitName(out[i]);
                    Track(out[i], std::move(m));
                }
                return "batch-create Velocity+PosS";
            }
            const size_t made = m_reg->CreateEntities<ParityLiveS, ParityTagS>(k, std::span{out});
            EXPECT_EQ(made, k);
            for (size_t i = 0; i < made; ++i)
            {
                ModelEntity m;
                m.live = 0;
                m.tag = true;
                m.name = InitName(out[i]);
                Track(out[i], std::move(m));
            }
            return "batch-create LiveS+TagS";
        }

        const char* OpDestroy()
        {
            const Astra::Entity e = PickAlive();
            m_reg->DestroyEntity(e);
            Untrack(e);
            return "destroy";
        }

        const char* OpBatchDestroy()
        {
            std::vector<Astra::Entity> victims = PickDistinct(1 + Rand(16));
            m_reg->DestroyEntities(std::span{victims});
            for (Astra::Entity e : victims) Untrack(e);
            return "batch-destroy";
        }
```

Then the single-entity add/remove/enable/write ops:

```cpp
        // Typed, emplace or by-id add of T; `present` is the model's view BEFORE the call.
        template<class T>
        void AddOne(Astra::Entity e, const T& value)
        {
            bool ok = false;
            const uint32_t how = std::is_empty_v<T> ? Rand(2) : Rand(3);   // no by-id for tags
            if (how == 0)      ok = m_reg->AddComponent(e, value);
            else if (how == 1) ok = m_reg->EmplaceComponent<T>(e, value);
            else               ok = m_reg->AddComponentByID(e, Astra::TypeID<T>::Value(), &value, sizeof(T));
            EXPECT_TRUE(ok) << "add of a live entity must return true (present => no-op)";
        }

        const char* OpAdd()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(10))
            {
            case 0: { const Velocity v{F(), F(), F()}; AddOne(e, v); if (!m.vel) m.vel = v; return "add Velocity"; }
            case 1: { Hierarchy h; h.parent = Rand(1000); AddOne(e, h); if (!m.hier) { m.hier = h; m.hierEnabled = true; } return "add Hierarchy"; }
            case 2: { AddOne(e, Player{}); m.player = true; return "add Player"; }
            case 3: { const ParityPosS p{F(), F(), F()}; AddOne(e, p); if (!m.pos) m.pos = p; return "add PosS"; }
            case 4: { ParityHierS h; h.parent = Rand(1000); AddOne(e, h); if (!m.hs) { m.hs = h; m.hsEnabled = true; } return "add HierS"; }
            case 5: { AddOne(e, ParityTagS{}); m.tag = true; return "add TagS"; }
            case 6: { const ParityLiveS l(static_cast<int>(Rand(1000))); AddOne(e, l); if (!m.live) m.live = l.value; return "add LiveS"; }
            case 7: { AddOne(e, Name{"ignored"}); return "add Name (universal no-op)"; }
            case 8: { AddOne(e, Timer{5.0f}); return "add Timer (universal no-op)"; }
            default: { AddOne(e, EnTag{}); if (!m.enTag) { m.enTag = true; m.enTagEnabled = true; } return "add EnTag"; }
            }
        }

        template<class T>
        bool RemoveOne(Astra::Entity e)
        {
            return Chance(50) ? m_reg->RemoveComponent<T>(e)
                              : m_reg->RemoveComponentByID(e, Astra::TypeID<T>::Value());
        }

        template<class T>
        void RemoveUniversal(Astra::Entity e)
        {
            const int before = m_refusals;
            m_expectRefusal = true;
            const bool removed = RemoveOne<T>(e);
            m_expectRefusal = false;
            EXPECT_FALSE(removed) << "a Universal component must never be removed";
            if (kAssertsActive) EXPECT_GT(m_refusals, before) << "Debug must assert on a Universal remove";
        }

        const char* OpRemove()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(10))
            {
            case 0: EXPECT_EQ(RemoveOne<Velocity>(e), m.vel.has_value()); m.vel.reset(); return "remove Velocity";
            case 1: EXPECT_EQ(RemoveOne<Hierarchy>(e), m.hier.has_value()); m.hier.reset(); m.hierEnabled = true; return "remove Hierarchy";
            case 2: EXPECT_EQ(RemoveOne<Player>(e), m.player); m.player = false; return "remove Player";
            case 3: EXPECT_EQ(RemoveOne<ParityPosS>(e), m.pos.has_value()); m.pos.reset(); return "remove PosS";
            case 4: EXPECT_EQ(RemoveOne<ParityHierS>(e), m.hs.has_value()); m.hs.reset(); m.hsEnabled = true; return "remove HierS";
            case 5: EXPECT_EQ(RemoveOne<ParityTagS>(e), m.tag); m.tag = false; return "remove TagS";
            case 6: EXPECT_EQ(RemoveOne<ParityLiveS>(e), m.live.has_value()); m.live.reset(); return "remove LiveS";
            case 7: RemoveUniversal<Name>(e); return "remove Name (refused)";
            case 8: RemoveUniversal<Timer>(e); return "remove Timer (refused)";
            default: EXPECT_EQ(RemoveOne<EnTag>(e), m.enTag); m.enTag = false; m.enTagEnabled = true; return "remove EnTag";
            }
        }

        const char* OpEnable()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            const bool enable = Chance(50);
            switch (Rand(4))
            {
            case 0:
                EXPECT_EQ(m_reg->SetEnabled<Hierarchy>(e, enable), m.hier.has_value());
                if (m.hier) m.hierEnabled = enable;
                return "enable Hierarchy";
            case 1:
                EXPECT_EQ(m_reg->SetEnabled<ParityHierS>(e, enable), m.hs.has_value());
                if (m.hs) m.hsEnabled = enable;
                return "enable HierS";
            case 2:
                EXPECT_TRUE(m_reg->SetEnabled<Timer>(e, enable));   // Universal: always present
                m.timerEnabled = enable;
                return "enable Timer";
            default:
                EXPECT_EQ(m_reg->SetEnabled<EnTag>(e, enable), m.enTag);   // enableable Table tag (bit-only column)
                if (m.enTag) m.enTagEnabled = enable;
                return "enable EnTag";
            }
        }

        // Value writes through GetComponent: a relocation that loses or swaps data shows up
        // as a value mismatch on the next check.
        const char* OpWrite()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(6))
            {
            case 0: if (m.vel) { const float v = F(); m_reg->GetComponent<Velocity>(e)->dx = v; m.vel->dx = v; } return "write Velocity";
            case 1: if (m.pos) { const float v = F(); m_reg->GetComponent<ParityPosS>(e)->y = v; m.pos->y = v; } return "write PosS";
            case 2: if (m.hs) { const uint32_t v = Rand(1000); m_reg->GetComponent<ParityHierS>(e)->parent = v; m.hs->parent = v; } return "write HierS";
            case 3: if (m.live) { const int v = static_cast<int>(Rand(1000)); m_reg->GetComponent<ParityLiveS>(e)->value = v; m.live = v; } return "write LiveS";
            case 4: { std::string v = "w" + std::to_string(m_counter++); m_reg->GetComponent<Name>(e)->value = v; m.name = v; return "write Name"; }
            default: { const float v = F(); m_reg->GetComponent<Timer>(e)->duration = v; m.timerDuration = v; return "write Timer"; }
            }
        }
```

Then the batch, CommandBuffer, stale-handle, save/load, Clear and range-for ops:

```cpp
        const char* OpBatchSparse()
        {
            std::vector<Astra::Entity> es = PickDistinct(1 + Rand(32));
            switch (Rand(4))
            {
            case 0:
            {
                const ParityPosS p{F(), F(), F()};
                m_reg->AddComponents(std::span{es}, p);
                for (Astra::Entity e : es) if (!m_model[e].pos) m_model[e].pos = p;
                return "batch-add PosS";
            }
            case 1:
            {
                size_t had = 0;
                for (Astra::Entity e : es) had += m_model[e].pos.has_value();
                EXPECT_EQ(m_reg->RemoveComponents<ParityPosS>(std::span{es}), had);
                for (Astra::Entity e : es) m_model[e].pos.reset();
                return "batch-remove PosS";
            }
            case 2:
            {
                m_reg->EmplaceComponents<ParityTagS>(std::span{es});
                for (Astra::Entity e : es) m_model[e].tag = true;
                return "batch-emplace TagS";
            }
            default:
            {
                const ParityLiveS l(static_cast<int>(Rand(1000)));
                m_reg->AddComponents(std::span{es}, l);
                for (Astra::Entity e : es) if (!m_model[e].live) m_model[e].live = l.value;
                return "batch-add LiveS";
            }
            }
        }

        // Recorded commands apply in record order at Execute (eager CommandBuffer).
        const char* OpCommandBuffer()
        {
            Astra::CommandBuffer cmd(m_reg.get());
            const Astra::Entity t = cmd.CreateEntity();
            const ParityPosS p{F(), F(), F()};
            cmd.AddComponent(t, p);
            std::vector<Astra::Entity> ab = PickDistinct(2);
            bool destroyB = false;
            if (ab.size() == 2)
            {
                cmd.AddComponent(ab[0], ParityTagS{});
                if (m_model[ab[0]].live) cmd.RemoveComponent<ParityLiveS>(ab[0]);
                else                     cmd.AddComponent(ab[0], ParityLiveS(7));
                if (m_model[ab[1]].hs) cmd.SetEnabled<ParityHierS>(ab[1], false);
                destroyB = Chance(30);
                if (destroyB) cmd.DestroyEntity(ab[1]);
            }
            EXPECT_TRUE(cmd.Execute().IsOk()) << "CommandBuffer playback failed";

            ModelEntity mt;
            mt.pos = p;
            mt.name = InitName(t);              // created through the funnel: universal init ran
            Track(t, std::move(mt));
            if (ab.size() == 2)
            {
                ModelEntity& a = m_model[ab[0]];
                a.tag = true;
                if (a.live) a.live.reset(); else a.live = 7;
                if (m_model[ab[1]].hs) m_model[ab[1]].hsEnabled = false;
                if (destroyB) Untrack(ab[1]);
            }
            return "command-buffer";
        }

        // Review Focus 1: a stale handle misses or is refused for every kind.
        const char* OpStaleProbe()
        {
            if (m_dead.empty()) return "stale-probe (none)";
            const Astra::Entity d = m_dead[Rand(m_dead.size())];
            EXPECT_FALSE(m_reg->IsValid(d));
            EXPECT_EQ(m_reg->GetComponent<ParityPosS>(d), nullptr);
            EXPECT_EQ(m_reg->GetComponent<Name>(d), nullptr);
            EXPECT_FALSE(m_reg->HasComponent<ParityTagS>(d));
            EXPECT_FALSE(m_reg->HasComponent<Timer>(d));
            EXPECT_FALSE(m_reg->AddComponent(d, ParityPosS{}));
            EXPECT_FALSE(m_reg->AddComponent(d, ParityTagS{}));
            EXPECT_FALSE(m_reg->SetEnabled<ParityHierS>(d, false));
            EXPECT_FALSE(m_reg->SetEnabled<Timer>(d, false));
            EXPECT_FALSE(m_reg->RemoveComponent<ParityLiveS>(d));
            return "stale-probe";
        }

        // The old registry is destroyed when m_reg is replaced; s_live accounting must hold.
        const char* OpSaveLoad()
        {
            Astra::Registry::SaveConfig sc;
            sc.compressionMode = Chance(50) ? Astra::CompressionMode::LZ4 : Astra::CompressionMode::None;
            auto saved = m_reg->Save(sc);
            if (!saved.IsOk()) { ADD_FAILURE() << "Save failed"; return "save-load"; }
            auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), m_creg, FuzzConfig());
            if (!loaded.IsOk()) { ADD_FAILURE() << "Load failed: " << static_cast<int>(*loaded.GetError()); return "save-load"; }
            m_reg = std::move(*loaded.GetValue());
            return sc.compressionMode == Astra::CompressionMode::LZ4 ? "save-load LZ4" : "save-load raw";
        }

        const char* OpClear()
        {
            m_reg->Clear();
            for (Astra::Entity e : m_alive) NoteDead(e);
            m_alive.clear();
            m_model.clear();
            return "clear";
        }

        const char* OpRangeFor()
        {
            size_t n = 0;
            for (auto [e, vel, pos] : m_reg->CreateView<Velocity, ParityPosS>())
            {
                ++n;
                auto it = m_model.find(e);
                EXPECT_TRUE(it != m_model.end() && it->second.pos && *it->second.pos == pos);
                (void)vel;
            }
            size_t expected = 0;
            for (const auto& [e, m] : m_model) expected += (m.vel && m.pos) ? 1 : 0;
            EXPECT_EQ(n, expected);
            return "range-for";
        }
```

The class closes with the checks:

```cpp
        // First mismatch between the registry and the model for `e`, or empty.
        std::string CompareEntity(Astra::Entity e, const ModelEntity& m) const
        {
            const Astra::Registry& r = *m_reg;
            if (!r.IsValid(e)) return "entity not valid";

            const Velocity* vel = r.GetComponent<Velocity>(e);
            if ((vel != nullptr) != m.vel.has_value() || r.HasComponent<Velocity>(e) != m.vel.has_value()) return "Velocity presence";
            if (vel && (vel->dx != m.vel->dx || vel->dy != m.vel->dy || vel->dz != m.vel->dz)) return "Velocity value";

            const Hierarchy* hier = r.GetComponent<Hierarchy>(e);
            if ((hier != nullptr) != m.hier.has_value()) return "Hierarchy presence";
            if (hier && hier->parent != m.hier->parent) return "Hierarchy value";
            if (r.IsEnabled<Hierarchy>(e) != (m.hier.has_value() && m.hierEnabled)) return "Hierarchy enabled";

            if (r.HasComponent<Player>(e) != m.player) return "Player presence";
            if (r.HasComponent<EnTag>(e) != m.enTag) return "EnTag presence";
            if (r.IsEnabled<EnTag>(e) != (m.enTag && m.enTagEnabled)) return "EnTag enabled";

            const ParityPosS* pos = r.GetComponent<ParityPosS>(e);
            if ((pos != nullptr) != m.pos.has_value() || r.HasComponent<ParityPosS>(e) != m.pos.has_value()) return "PosS presence";
            if (pos && !(*pos == *m.pos)) return "PosS value";

            const ParityHierS* hs = r.GetComponent<ParityHierS>(e);
            if ((hs != nullptr) != m.hs.has_value()) return "HierS presence";
            if (hs && hs->parent != m.hs->parent) return "HierS value";
            if (r.IsEnabled<ParityHierS>(e) != (m.hs.has_value() && m.hsEnabled)) return "HierS enabled";

            if (r.HasComponent<ParityTagS>(e) != m.tag) return "TagS presence";

            const ParityLiveS* live = r.GetComponent<ParityLiveS>(e);
            if ((live != nullptr) != m.live.has_value()) return "LiveS presence";
            if (live && live->value != *m.live) return "LiveS value";

            const Name* name = r.GetComponent<Name>(e);
            if (!name || !r.HasComponent<Name>(e)) return "Name (universal) missing";
            if (name->value != m.name) return "Name value: got '" + name->value + "' want '" + m.name + "'";

            const Timer* timer = r.GetComponent<Timer>(e);
            if (!timer || !r.HasComponent<Timer>(e)) return "Timer (universal) missing";
            if (timer->duration != m.timerDuration) return "Timer value";
            if (r.IsEnabled<Timer>(e) != m.timerEnabled) return "Timer enabled";
            return {};
        }

        bool Fail(const std::string& what)
        {
            ADD_FAILURE() << "[ fuzz ] seed=" << m_seed << " op#" << m_opIndex << " (" << m_op << "): " << what;
            return false;
        }

        bool Check()
        {
            if (::testing::Test::HasFailure()) return Fail("an EXPECT inside the op failed (see above)");
            const std::string invariants = m_reg->ValidateStorageInvariants();
            if (!invariants.empty()) return Fail("ValidateStorageInvariants: " + invariants);
            if (m_unexpectedAsserts != 0) return Fail("unexpected assert: " + m_lastUnexpected);
            if (m_reg->Size() != m_model.size())
                return Fail("Size " + std::to_string(m_reg->Size()) + " != model " + std::to_string(m_model.size()));
            size_t live = 0, pos = 0, velPos = 0, velNoTag = 0, hsOn = 0, hsAll = 0, timerOn = 0, tag = 0, vel = 0, hierOn = 0, enTagOn = 0;
            for (const auto& [e, m] : m_model)
            {
                if (std::string why = CompareEntity(e, m); !why.empty())
                    return Fail("entity " + std::to_string(e.GetID()) + "v" + std::to_string(e.GetVersion()) + ": " + why);
                live += m.live.has_value();
                pos += m.pos.has_value();
                velPos += (m.vel && m.pos);
                velNoTag += (m.vel && !m.tag);
                hsOn += (m.hs && m.hsEnabled);
                hsAll += m.hs.has_value();
                timerOn += m.timerEnabled;
                tag += m.tag;
                vel += m.vel.has_value();
                hierOn += (m.hier && m.hierEnabled);
                enTagOn += (m.vel && m.enTag && m.enTagEnabled);
            }
            if (ParityLiveS::s_live != m_liveBase + static_cast<int>(live))
                return Fail("ParityLiveS live count " + std::to_string(ParityLiveS::s_live - m_liveBase) + " != model " + std::to_string(live));
            auto expectSize = [&](size_t got, size_t want, const char* view) {
                return got == want ? true : Fail(std::string(view) + " Size " + std::to_string(got) + " != " + std::to_string(want));
            };
            return expectSize(m_reg->CreateView<ParityPosS>().Size(), pos, "View<PosS>")
                && expectSize(m_reg->CreateView<Velocity, ParityPosS>().Size(), velPos, "View<Velocity,PosS>")
                && expectSize(m_reg->CreateView<Velocity, Astra::Not<ParityTagS>>().Size(), velNoTag, "View<Velocity,Not<TagS>>")
                && expectSize(m_reg->CreateView<ParityHierS>().Size(), hsOn, "View<HierS>")
                && expectSize(m_reg->CreateView<Astra::IncludeDisabled<ParityHierS>>().Size(), hsAll, "View<IncludeDisabled<HierS>>")
                && expectSize(m_reg->CreateView<Timer>().Size(), timerOn, "View<Timer> (universal)")
                && expectSize(m_reg->CreateView<ParityTagS>().Size(), tag, "View<TagS>")
                && expectSize(m_reg->CreateView<Velocity, Astra::Optional<ParityLiveS>>().Size(), vel, "View<Velocity,Optional<LiveS>>")
                && expectSize(m_reg->CreateView<Hierarchy>().Size(), hierOn, "View<Hierarchy>")
                && expectSize(m_reg->CreateView<Velocity, EnTag>().Size(), enTagOn, "View<Velocity,EnTag>");
        }
    };
} // namespace

TEST(StorageInvariantFuzz, FixedSeedsShortRun)
{
    constexpr size_t kOps = 1500;
    for (uint64_t seed : {1ull, 2ull, 3ull, 0xA57A5EEDull})
    {
        std::printf("[ fuzz ] seed=%" PRIu64 " ops=%zu\n", seed, kOps);
        StorageFuzzer fuzzer(seed);
        const bool ok = fuzzer.Run(kOps);
        fuzzer.Finish();
        if (!ok) return;   // the failure message carries seed + op index
    }
}

TEST(StorageInvariantFuzz, DISABLED_LongRun)
{
    const uint64_t first = EnvU64("ASTRA_FUZZ_SEED", 1000);
    const uint64_t count = EnvU64("ASTRA_FUZZ_SEEDS", 64);
    const uint64_t ops = EnvU64("ASTRA_FUZZ_OPS", 20000);
    for (uint64_t seed = first; seed < first + count; ++seed)
    {
        std::printf("[ fuzz ] seed=%" PRIu64 " ops=%" PRIu64 "\n", seed, ops);
        std::fflush(stdout);
        StorageFuzzer fuzzer(seed);
        const bool ok = fuzzer.Run(static_cast<size_t>(ops));
        fuzzer.Finish();
        if (!ok) return;
    }
}
```

`ValidateStorageInvariants()` is a `const` member compiled in every configuration (Task 16), so `Check()` calls it unconditionally and the fuzzer also runs in a Release test build.

- [ ] **Step 2: Regenerate, build, run the short fuzz**

Run from `D:\dev\starworks\Astra-storage`, in order:
1. `D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`
2. `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
3. `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=StorageInvariantFuzz.*`

Expected output:
- four `[ fuzz ] seed=... ops=1500` lines;
- `[  PASSED  ] 1 test.`;
- `YOU HAVE 1 DISABLED TEST`.

Record the test's wall time from the `(N ms)` line. If it exceeds 20 s in Debug, lower `kOps` to 1000 and record the change. The normal suite must stay quick.

- [ ] **Step 3: Prove the fuzzer bites (mutation check, reverted)**

A fuzzer that cannot fail proves nothing. Run this check:
1. Confirm a clean tree: `git status --short` lists nothing tracked as modified.
2. Temporarily break the swap-remove carry. In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, make the first statement of `ArchetypeChunk::SwapRemoveSparseBits` an early `return;` (the canonical swap-remove helper that Tasks 15 and 17 carry through).
3. Rebuild (Step 2 command 2), then re-run Step 2 command 3.
   - Expected: FAILED, with a message that starts `[ fuzz ] seed=1 op#` and names `ValidateStorageInvariants` (P1) or a presence/view-size mismatch.
4. Revert:
   - `git checkout -- include/Astra/Archetype/ArchetypeChunkPool.hpp`
   - `git status --short` shows no tracked modification;
   - rebuild;
   - re-run Step 2 command 3: PASSED again.
5. Record the failing message in the task report.

- [ ] **Step 4: Run the long fuzz (separate filter)**

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_also_run_disabled_tests --gtest_filter=StorageInvariantFuzz.DISABLED_LongRun`

Expected:
- 64 `[ fuzz ] seed=1000..1063 ops=20000` lines;
- `[  PASSED  ] 1 test.`

Record the wall time. To reproduce one seed:
`$env:ASTRA_FUZZ_SEED='1037'; $env:ASTRA_FUZZ_SEEDS='1'; .\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_also_run_disabled_tests --gtest_filter=StorageInvariantFuzz.DISABLED_LongRun`

Then clear both variables:
`Remove-Item Env:ASTRA_FUZZ_SEED, Env:ASTRA_FUZZ_SEEDS`

- [ ] **Step 5: Full shuffled run**

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`

Expected:
- every test PASSED;
- exit code 0;
- record the seed line.

- [ ] **Step 6: Commit**

```powershell
git add tests/Storage/StorageInvariantFuzzTest.cpp
git commit -m "test(storage): invariant fuzzer -- seeded op sequences over Table / Sparse / Sparse-enableable / Sparse-tag / Universal components vs a reference model; ValidateStorageInvariants + full model, view-size and lifetime comparison after every op; fixed-seed short run in the suite, long run behind DISABLED_LongRun" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 40: Debug::Inspector reports storage kinds, stores, presence blocks + the "sparse-shaped" advisory (s9.4)

**Files:**
- Modify: `include/Astra/Debug/Inspector.hpp`.
- Create: `tests/Debug/InspectorStorageTest.cpp`

**Interfaces:**
- Consumes:
  - `Registry::GetComponentStorage()` (Task 5);
  - `ComponentStorage::KindOf`, `SparseSlotOf`, `ComponentOfSlot` (`INVALID_COMPONENT` for a free slot), `Sparse`, `Universal`, `UsedSparseSlotCount` (Task 4);
  - `SparseSet::Size`, `PageCount` (Task 10);
  - `UniversalColumn::LiveCount`, `PageCount` (Task 8);
  - `ArchetypeChunk::GetSparseBlocks() const` (a `std::span<const ArchetypeChunk::SparseBlock>`, Task 15);
  - `ComponentRegistry::GetAllDescriptors`;
  - the signal manager (`Events::ComponentAdded` / `ComponentRemoved`).
- Produces:
  - `Astra::Debug::StoreInfo`;
  - `Astra::Debug::SparseBlockInfo`;
  - `ChunkInfo::sparseBlocks`;
  - `InspectorSnapshot::components`;
  - `RegistrySnapshot::sparseSlotsUsed`;
  - `Astra::Debug::ChurnTracker`;
  - `Capture(Registry&, InspectorSnapshot&, const ChurnTracker* churn = nullptr)`.

**Design (G4-safe).** The advisory needs per-component add/remove counts. Counting them inside `Registry` would put an increment on every Table add/remove, the hot path the S4 gate protects, and grow the Registry. Instead:
- An opt-in `Debug::ChurnTracker`, which a tool attaches to one registry, enables `ComponentAdded`/`ComponentRemoved` and counts per `ComponentID`.
- A registry without a tracker pays nothing: signals stay off, and nothing new runs.
- The tracker restores the registry's previous signal mask on destruction.

- [ ] **Step 1: Write the failing tests**

Create `tests/Debug/InspectorStorageTest.cpp`:

```cpp
// Inspector storage reporting + the "sparse-shaped" advisory (spec 2026-10-01 s9.4).
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>
#include "../Storage/StorageParity.hpp"

namespace
{
    using Astra::Test::Health;
    using Astra::Test::Name;
    using Astra::Test::Physics;
    using Astra::Test::Position;
    using Astra::Test::Timer;
    using Astra::Test::Transform;
    using Astra::Test::Velocity;
    using Astra::Test::Parity::ParityHierS;
    using Astra::Test::Parity::ParityPosS;
    using Astra::Test::Parity::ParityTagS;

    const Astra::Debug::StoreInfo* FindStore(const Astra::Debug::InspectorSnapshot& snap, Astra::ComponentID id)
    {
        for (const auto& s : snap.components)
            if (s.id == id) return &s;
        return nullptr;
    }

    // Name (non-enableable) and Timer (enableable) are Universal in this registry.
    std::unique_ptr<Astra::Registry> MakeRegistry()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position, Velocity, Health, Physics, Transform, Name, Timer,
                                 ParityPosS, ParityTagS, ParityHierS>();
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>();
        cfg.AddUniversal<Timer>();
        return std::make_unique<Astra::Registry>(creg, cfg);
    }
}

TEST(InspectorStorage, ReportsKindPopulationPagesAndPresenceBlocks)
{
    auto reg = MakeRegistry();
    std::vector<Astra::Entity> es(2000);
    ASSERT_EQ(reg->CreateEntities<Position>(2000, std::span{es}), 2000u);
    for (size_t i = 0; i < 2000; i += 4) ASSERT_TRUE(reg->AddComponent(es[i], ParityPosS{}));   // 500 holders
    for (size_t i = 0; i < 2000; i += 8) ASSERT_TRUE(reg->SetEnabled<Timer>(es[i], false));     // 250 disabled

    const auto snap = Astra::Debug::Capture(*reg);
    EXPECT_EQ(snap.registry.sparseSlotsUsed, 2u);   // ParityPosS + Timer's disabled mirror

    const auto* pos = FindStore(snap, Astra::TypeID<Position>::Value());
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->kind, Astra::StorageKind::Table);
    EXPECT_EQ(pos->population, 2000u);
    EXPECT_EQ(pos->archetypeCount, 1u);          // Sparse membership never fragments
    EXPECT_EQ(pos->sparseSlot, -1);
    EXPECT_EQ(pos->pageCount, 0u);

    const auto* ps = FindStore(snap, Astra::TypeID<ParityPosS>::Value());
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->kind, Astra::StorageKind::Sparse);
    EXPECT_GE(ps->sparseSlot, 0);
    EXPECT_EQ(ps->population, 500u);
    EXPECT_GE(ps->pageCount, 1u);
    EXPECT_GE(ps->presenceBlockCount, 1u);
    EXPECT_EQ(ps->presenceBitsSet, 500u);        // P1, as the Inspector sees it

    const auto* name = FindStore(snap, Astra::TypeID<Name>::Value());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->kind, Astra::StorageKind::Universal);
    EXPECT_EQ(name->population, 2000u);
    EXPECT_EQ(name->pageCount, 2u);              // indices < 2048 -> pages 0 and 1 (1024 slots each)
    EXPECT_EQ(name->sparseSlot, -1);             // non-enableable Universal: no slot, no blocks

    const auto* timer = FindStore(snap, Astra::TypeID<Timer>::Value());
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->kind, Astra::StorageKind::Universal);
    EXPECT_GE(timer->sparseSlot, 0);             // enableable Universal: slot for the disabled mirror
    EXPECT_EQ(timer->disabledCount, 250u);       // P3, as the Inspector sees it

    size_t bits = 0, blocks = 0;
    for (const auto& a : snap.archetypes)
        for (const auto& ch : a.chunks)
            for (const auto& b : ch.sparseBlocks)
                if (b.componentId == Astra::TypeID<ParityPosS>::Value())
                {
                    ++blocks;
                    bits += b.presentCount;
                    EXPECT_TRUE(b.hasPresentWords);
                    EXPECT_GT(ch.sparseBlockBytes, 0u);
                }
    EXPECT_EQ(bits, 500u);
    EXPECT_EQ(blocks, ps->presenceBlockCount);
}

TEST(InspectorStorage, CaptureNeverCreatesStoresOrSlots)
{
    auto reg = MakeRegistry();
    (void)reg->CreateEntity<Position>();
    for (int pass = 0; pass < 2; ++pass)
    {
        const auto snap = Astra::Debug::Capture(*reg);
        const auto* tag = FindStore(snap, Astra::TypeID<ParityTagS>::Value());
        ASSERT_NE(tag, nullptr);                   // Sparse kinds are listed even when unused...
        EXPECT_EQ(tag->kind, Astra::StorageKind::Sparse);
        EXPECT_EQ(tag->sparseSlot, -1);            // ...but looking acquires no slot
        EXPECT_EQ(tag->population, 0u);
    }
}

TEST(InspectorStorage, AdvisoryFlagsToggledTableComponentsOnly)
{
    auto reg = MakeRegistry();
    Astra::Debug::ChurnTracker churn(*reg);
    std::vector<Astra::Entity> es(50);
    ASSERT_EQ(reg->CreateEntities<Position>(50, std::span{es}), 50u);
    for (int round = 0; round < 4; ++round)
        for (Astra::Entity e : es)
        {
            ASSERT_TRUE(reg->AddComponent(e, Health{}));        // Table, toggled: flagged
            ASSERT_TRUE(reg->RemoveComponent<Health>(e));
            ASSERT_TRUE(reg->AddComponent(e, ParityPosS{}));    // already Sparse: never flagged
            ASSERT_TRUE(reg->RemoveComponent<ParityPosS>(e));
        }
    for (Astra::Entity e : es) ASSERT_TRUE(reg->AddComponent(e, Velocity{}));   // added once, kept

    const auto snap = Astra::Debug::Capture(*reg, &churn);
    const auto* health = FindStore(snap, Astra::TypeID<Health>::Value());
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health->adds, 200u);
    EXPECT_EQ(health->removes, 200u);
    EXPECT_EQ(health->population, 0u);
    EXPECT_TRUE(health->looksSparseShaped);
    EXPECT_NE(health->advisory.find("toggled"), std::string::npos) << health->advisory;

    const auto* ps = FindStore(snap, Astra::TypeID<ParityPosS>::Value());
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->removes, 200u);
    EXPECT_FALSE(ps->looksSparseShaped);

    const auto* vel = FindStore(snap, Astra::TypeID<Velocity>::Value());
    ASSERT_NE(vel, nullptr);
    EXPECT_EQ(vel->adds, 50u);
    EXPECT_EQ(vel->removes, 0u);
    EXPECT_FALSE(vel->looksSparseShaped);
}
```

The test file ends with the rare-multiplier advisory and the tracker's signal-mask restore:

```cpp
// 1000 entities over 5 archetypes; Physics on ONE entity of each -> 5 archetypes that
// differ from an existing archetype only by Physics: rare AND multiplying.
TEST(InspectorStorage, AdvisoryFlagsRareTableComponentsThatMultiplyArchetypes)
{
    auto reg = MakeRegistry();
    std::vector<Astra::Entity> holders;
    for (int i = 0; i < 200; ++i)
    {
        Astra::Entity a = reg->CreateEntity<Position>();
        Astra::Entity b = reg->CreateEntity<Position, Velocity>();
        Astra::Entity c = reg->CreateEntity<Position, Health>();
        Astra::Entity d = reg->CreateEntity<Position, Velocity, Health>();
        Astra::Entity e = reg->CreateEntity<Position, Transform>();
        if (i == 0) holders = {a, b, c, d, e};
    }
    for (Astra::Entity h : holders) ASSERT_TRUE(reg->AddComponent(h, Physics{}));

    const auto snap = Astra::Debug::Capture(*reg);
    const auto* physics = FindStore(snap, Astra::TypeID<Physics>::Value());
    ASSERT_NE(physics, nullptr);
    EXPECT_EQ(physics->population, 5u);
    EXPECT_EQ(physics->archetypeCount, 5u);
    EXPECT_EQ(physics->pairedArchetypeCount, 5u);
    EXPECT_TRUE(physics->looksSparseShaped);
    EXPECT_NE(physics->advisory.find("rare"), std::string::npos) << physics->advisory;

    const auto* pos = FindStore(snap, Astra::TypeID<Position>::Value());
    ASSERT_NE(pos, nullptr);
    EXPECT_FALSE(pos->looksSparseShaped);        // on every entity
    const auto* vel = FindStore(snap, Astra::TypeID<Velocity>::Value());
    ASSERT_NE(vel, nullptr);
    EXPECT_FALSE(vel->looksSparseShaped);        // 400 of 1000: not rare
}

TEST(InspectorStorage, ChurnTrackerRestoresTheSignalMask)
{
    Astra::Registry reg;
    const Astra::Signal before = reg.GetEnabledSignals();
    {
        Astra::Debug::ChurnTracker churn(reg);
        EXPECT_NE(reg.GetEnabledSignals(), before);
    }
    EXPECT_EQ(reg.GetEnabledSignals(), before);
}
```

- [ ] **Step 2: Run the tests and watch them fail**

Run, in order:
1. `D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026`
2. `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`

Expected: the build FAILS in `InspectorStorageTest.cpp`, with `StoreInfo` / `ChurnTracker` / `sparseBlocks` not members of `Astra::Debug`.

- [ ] **Step 3: Confirm the inputs exist (no edits)**

Every store-side input already exists:
- `Registry::GetComponentStorage()` (Task 5);
- `SparseSet::PageCount()` (Task 10);
- `UniversalColumn::PageCount()` (Task 8);
- `ArchetypeChunk::GetSparseBlocks()` (Task 15);
- `ComponentStorage::UsedSparseSlotCount()` (Task 4).

Confirm with:
`Select-String -Path include\Astra\Registry\*.hpp,include\Astra\Archetype\ArchetypeChunkPool.hpp -Pattern 'GetComponentStorage|PageCount|GetSparseBlocks|UsedSparseSlotCount'`

Expected: at least one hit for each name. If one is missing, add it in the owning header with the semantics listed in the Interfaces block, and record the deviation.

- [ ] **Step 4: Extend `include/Astra/Debug/Inspector.hpp`**

**4a.** Add `#include <array>` after `#include <cstdint>`.

**4b.** Insert this struct immediately BEFORE `struct ChunkInfo`:

```cpp
    // One sparse presence block of one chunk (spec 2026-10-01 s5.1). Side allocation
    // from the chunk pool's word freelist -- NOT part of the chunk arena (chunkBytes).
    struct SparseBlockInfo
    {
        uint16_t slot = 0;
        ComponentID componentId{};
        uint32_t presentCount = 0;
        uint32_t disabledCount = 0;
        bool hasPresentWords = false;    // false for an enableable Universal ("all present")
        bool hasDisabledWords = false;
    };
```

Then add these two members at the end of `struct ChunkInfo`, after the accounting line:

```cpp
        std::vector<SparseBlockInfo> sparseBlocks;   // ascending slot; empty = no sparse member here
        size_t sparseBlockBytes = 0;                 // side allocations; NOT in the chunkBytes sum
```

**4c.** Insert this struct immediately AFTER `struct ArchetypeInfo`:

```cpp
    // One component's storage, as the Studio storage panel shows it (s9.4). Table entries
    // are listed when the component is held or churned; Sparse/Universal ones always.
    struct StoreInfo
    {
        std::string name;
        ComponentID id{};
        StorageKind kind = StorageKind::Table;   // effective kind in THIS registry
        size_t size = 0;
        bool isEnableable = false;
        bool isChangeTracked = false;
        int sparseSlot = -1;                     // Sparse, or enableable Universal (mirror); else -1
        size_t population = 0;                   // live holders
        size_t pageCount = 0;                    // Sparse: index pages; Universal: column pages; Table: 0
        size_t archetypeCount = 0;               // Table: archetypes whose mask holds it
        size_t pairedArchetypeCount = 0;         // Table: archetypes whose mask-minus-it also exists
        size_t presenceBlockCount = 0;           // chunks with a SparseBlock for sparseSlot
        size_t presenceBitsSet = 0;              // sum of presentCount (== population for Sparse: P1)
        size_t disabledCount = 0;                // Sparse/Universal: sum of the disabled mirror (P3)
        uint64_t adds = 0;                       // ChurnTracker counts (0 without a tracker)
        uint64_t removes = 0;
        bool looksSparseShaped = false;          // ADVISORY only; nothing is promoted automatically
        std::string advisory;                    // the reason, or empty
    };
```

Then add one member to `struct InspectorSnapshot`, after `archetypes`:

```cpp
        std::vector<StoreInfo> components;   // one entry per listed component, ascending id
```

And one member at the end of `struct RegistrySnapshot`:

```cpp
        size_t sparseSlotsUsed = 0;          // ComponentStorage::UsedSparseSlotCount() (cap ASTRA_MAX_SPARSE_COMPONENTS)
```

**4d.** Insert this immediately BEFORE the first `Capture`:

```cpp
    // Opt-in per-component churn counter for the "sparse-shaped" advisory. Attaching one
    // enables ComponentAdded/ComponentRemoved on the registry (a registry without a
    // tracker pays nothing); the previous signal mask is restored on destruction. Must not
    // outlive the registry, and follows only that registry (a Load builds a new one).
    class ChurnTracker
    {
    public:
        explicit ChurnTracker(Registry& registry)
            : m_registry(&registry), m_previous(registry.GetEnabledSignals())
        {
            registry.EnableSignals(Signal::ComponentAdded | Signal::ComponentRemoved);
            SignalManager* signals = registry.GetSignalManager();
            m_addedId = signals->On<Events::ComponentAdded>().Register(
                [this](const Events::ComponentAdded& ev) { if (ev.componentId < MAX_COMPONENTS) ++m_adds[ev.componentId]; });
            m_removedId = signals->On<Events::ComponentRemoved>().Register(
                [this](const Events::ComponentRemoved& ev) { if (ev.componentId < MAX_COMPONENTS) ++m_removes[ev.componentId]; });
        }

        ~ChurnTracker()
        {
            SignalManager* signals = m_registry->GetSignalManager();
            signals->On<Events::ComponentAdded>().Unregister(m_addedId);
            signals->On<Events::ComponentRemoved>().Unregister(m_removedId);
            m_registry->SetEnabledSignals(m_previous);
        }

        ChurnTracker(const ChurnTracker&) = delete;
        ChurnTracker& operator=(const ChurnTracker&) = delete;

        ASTRA_NODISCARD uint64_t Adds(ComponentID id) const noexcept { return id < MAX_COMPONENTS ? m_adds[id] : 0; }
        ASTRA_NODISCARD uint64_t Removes(ComponentID id) const noexcept { return id < MAX_COMPONENTS ? m_removes[id] : 0; }
        void Reset() noexcept { m_adds.fill(0); m_removes.fill(0); }

    private:
        Registry* m_registry;
        Signal m_previous;
        std::array<uint64_t, MAX_COMPONENTS> m_adds{};
        std::array<uint64_t, MAX_COMPONENTS> m_removes{};
        std::size_t m_addedId = 0;
        std::size_t m_removedId = 0;
    };

    // Advisory thresholds (s9.4). Heuristics, deliberately conservative.
    inline constexpr uint64_t kAdvisoryMinRemoves = 64;          // toggled: at least this many removes...
    inline constexpr size_t kAdvisoryMinPairedArchetypes = 4;    // rare: splits at least this many archetypes...
    inline constexpr size_t kAdvisoryRareDivisor = 10;           // ...while held by < 1/10 of live entities

    inline void Advise(StoreInfo& s, size_t liveEntities)
    {
        if (s.kind != StorageKind::Table)
            return;   // already out of the archetype; nothing to suggest
        if (s.removes >= kAdvisoryMinRemoves && s.removes >= s.population)
        {
            s.looksSparseShaped = true;
            s.advisory += "toggled: " + std::to_string(s.removes) + " removes vs population " +
                          std::to_string(s.population) + "; every add/remove moves the whole entity. ";
        }
        if (s.pairedArchetypeCount >= kAdvisoryMinPairedArchetypes &&
            s.population * kAdvisoryRareDivisor < liveEntities)
        {
            s.looksSparseShaped = true;
            s.advisory += "rare: held by " + std::to_string(s.population) + " of " + std::to_string(liveEntities) +
                          " entities but splits " + std::to_string(s.pairedArchetypeCount) + " archetypes. ";
        }
        if (s.looksSparseShaped)
            s.advisory += "Consider `static constexpr auto AstraStorage = Astra::Storage::Sparse;`.";
    }
```

**4e. Replace `Capture`.** Two edits:
- Change the signature of `inline void Capture(Registry& registry, InspectorSnapshot& out)` to:

```cpp
    inline void Capture(Registry& registry, InspectorSnapshot& out, const ChurnTracker* churn = nullptr)
```

- Replace the value-returning overload with:

```cpp
    ASTRA_NODISCARD inline InspectorSnapshot Capture(Registry& registry, const ChurnTracker* churn = nullptr)
    {
        InspectorSnapshot snap;
        Capture(registry, snap, churn);
        return snap;
    }
```

Inside the first `Capture`, make five edits.

**(i)** Directly after `out.archetypes.clear();`, add:

```cpp
        out.components.clear();
```

**(ii)** Directly after the `if (!manager || !components) return;` guard, add the per-component and per-slot accumulators:

```cpp
        ComponentStorage* storage = registry.GetComponentStorage();
        std::array<size_t, MAX_COMPONENTS> tablePopulation{};
        std::array<size_t, MAX_COMPONENTS> tableArchetypes{};
        std::array<size_t, MAX_COMPONENTS> tablePaired{};
        std::array<size_t, ASTRA_MAX_SPARSE_COMPONENTS> slotBlocks{};
        std::array<size_t, ASTRA_MAX_SPARSE_COMPONENTS> slotBits{};
        std::array<size_t, ASTRA_MAX_SPARSE_COMPONENTS> slotDisabled{};
        out.registry.sparseSlotsUsed = storage ? storage->UsedSparseSlotCount() : 0;
```

**(iii)** In the signature loop, directly after the `if (!mask.Test(bit)) continue;` line, add the Table accounting. Each mask bit is a Table component (Sparse and Universal never enter masks, s3.1).

```cpp
                tablePopulation[bit] += archetype->GetEntityCount();
                ++tableArchetypes[bit];
                ComponentMask without = mask;
                without.Reset(bit);
                if (manager->FindArchetype(without) != nullptr)
                    ++tablePaired[bit];   // this archetype exists only because of `bit`
```

**(iv)** In the chunk loop, directly after `ch.slackBytes = ch.chunkBytes - cursor;`, add the presence-block capture:

```cpp
                // Sparse presence blocks (s5.1): side allocations, reported separately.
                const size_t blockWordBytes = ((ch.capacity + 63) / 64) * 8;
                for (const ArchetypeChunk::SparseBlock& block : chunk->GetSparseBlocks())   // sorted by slot
                {
                    if (block.slot >= ASTRA_MAX_SPARSE_COMPONENTS)
                        continue;
                    SparseBlockInfo bi;
                    bi.slot = block.slot;
                    bi.componentId = storage ? storage->ComponentOfSlot(block.slot) : INVALID_COMPONENT;
                    bi.presentCount = block.presentCount;
                    bi.disabledCount = block.disabledCount;
                    bi.hasPresentWords = block.present != nullptr;
                    bi.hasDisabledWords = block.disabled != nullptr;
                    ch.sparseBlockBytes += (bi.hasPresentWords ? blockWordBytes : 0) + (bi.hasDisabledWords ? blockWordBytes : 0);
                    ++slotBlocks[block.slot];
                    slotBits[block.slot] += block.presentCount;
                    slotDisabled[block.slot] += block.disabledCount;
                    ch.sparseBlocks.push_back(bi);
                }
```

**(v)** Directly after `out.registry.archetypeCount = out.archetypes.size();` (the function's last statement), add the per-component store list:

```cpp
        // ---- Per-component storage (s9.4) ------------------------------------------
        std::vector<ComponentDescriptor> descriptors;
        components->GetAllDescriptors(descriptors);
        std::sort(descriptors.begin(), descriptors.end(),
                  [](const ComponentDescriptor& a, const ComponentDescriptor& b) { return a.id < b.id; });
        for (const ComponentDescriptor& d : descriptors)
        {
            if (d.id >= MAX_COMPONENTS)
                continue;
            StoreInfo s;
            s.name = d.name ? d.name : "?";
            s.id = d.id;
            s.size = d.size;
            s.isEnableable = d.isEnableable;
            s.isChangeTracked = d.isChangeTracked;
            s.kind = storage ? storage->KindOf(d.id) : StorageKind::Table;
            s.sparseSlot = storage ? storage->SparseSlotOf(d.id) : -1;
            switch (s.kind)
            {
            case StorageKind::Table:
                s.population = tablePopulation[d.id];
                s.archetypeCount = tableArchetypes[d.id];
                s.pairedArchetypeCount = tablePaired[d.id];
                break;
            case StorageKind::Sparse:
                // Only an assigned slot means the set exists; Sparse() would create it lazily.
                if (s.sparseSlot >= 0)
                    if (const SparseSet* set = storage->Sparse(d.id))
                    {
                        s.population = set->Size();
                        s.pageCount = set->PageCount();
                    }
                break;
            case StorageKind::Universal:
                if (const UniversalColumn* column = storage->Universal(d.id))
                {
                    s.population = column->LiveCount();
                    s.pageCount = column->PageCount();
                }
                break;
            }
            if (s.sparseSlot >= 0 && s.sparseSlot < static_cast<int>(ASTRA_MAX_SPARSE_COMPONENTS))
            {
                s.presenceBlockCount = slotBlocks[static_cast<size_t>(s.sparseSlot)];
                s.presenceBitsSet = slotBits[static_cast<size_t>(s.sparseSlot)];
                s.disabledCount = slotDisabled[static_cast<size_t>(s.sparseSlot)];
            }
            if (churn)
            {
                s.adds = churn->Adds(d.id);
                s.removes = churn->Removes(d.id);
            }
            // Unused Table components (and resource descriptors sharing the id space) are noise.
            if (s.kind == StorageKind::Table && s.population == 0 && s.adds == 0 && s.removes == 0)
                continue;
            Advise(s, out.registry.entityCount);
            out.components.push_back(std::move(s));
        }
```

Also add `#include <algorithm>` beside `<array>` for `std::sort`.

If `ComponentStorage` also tracks a slot for enableable Universal components under a separate accessor, use that accessor for `sparseSlot` and record the deviation. The spec gives that case a slot (s5.1); the canonical `SparseSlotOf` is expected to cover it.

- [ ] **Step 5: Build and run the Inspector tests**

Run, in order:
1. `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
2. `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=InspectorStorage.*:Inspector.*`

Expected:
- `InspectorStorage`: 5 tests PASSED.
- Every pre-existing `Inspector.*` test still PASSES. The snapshot only grew; `Capture(reg)` and `Capture(reg, snap)` keep their meaning.

- [ ] **Step 6: Studio still compiles, full shuffled run**

Studio reads internals only through the Inspector, so the additive change must not break it.

1. Build the Studio:
   `msbuild ide\AstraStudio.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
   Expected: `Build succeeded.`
2. Run the full suite:
   `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`
   Expected: all PASSED, exit code 0. Record the seed line.

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Debug/Inspector.hpp tests/Debug/InspectorStorageTest.cpp
git commit -m "feat(storage): Debug::Inspector reports each component's storage kind, population, sparse/universal page counts and per-chunk presence blocks, plus an opt-in ChurnTracker-fed 'sparse-shaped' advisory (toggled or rare-and-multiplying Table components); advisory only" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 41: Storage benchmarks in `benchmark/Benchmark.cpp` and `bench-compare/` (s9.3)

**Files:**
- Modify: `benchmark/Benchmark.cpp`, to add the `BM_Storage_*` family and its registrations.
- Create: `bench-compare/bench_common.hpp`, a verbatim copy of the shared hand-rolled harness.
  - The harness lives untracked in `D:\dev\starworks\Astra\bench-compare\`. That directory is READ, never written.
- Create: `bench-compare/bench_storage.cpp`, the Astra-only storage-policy ops for the hand-rolled harness.
- Create: `bench-compare/build_storage.bat`
- Create: `bench-compare/storage_results.ps1`, which turns round CSVs into the RESULTS.md table.

**Interfaces:**
- Consumes the `Storage` trait, `Config::AddUniversal<T>()`, and `Registry(std::shared_ptr<ComponentRegistry>, const Config&)`.
- Produces benchmark names. All share the `BM_Storage_` prefix, so the S4 gate (Task 42) can exclude them with `--benchmark_filter=-BM_Storage_`:
  - `BM_Storage_IdentityChurn_Table` / `_Universal`
  - `BM_Storage_AddRemoveChurn_Table` / `_Sparse`
  - `BM_Storage_FilteredIter_Table` / `_Sparse`
  - `BM_Storage_SoleIter_Table` / `_Sparse` (`View<S>` alone; the Sparse one is sparse-driven)
  - `BM_Storage_RandomGet_Table` / `_Sparse` / `_Universal`

**The workloads (s9.3):**
1. **Structural churn with a 48-byte non-trivially-copyable component.** `BenchIdentity { uint64 guidHi, guidLo; std::string name; }` is 48 B on MSVC x64 (the Arcane `Identity` shape). It is present on every entity, as Table vs Universal. Each timed op adds Velocity and then removes it, which is 2 archetype moves. Universal takes the 48 B out of every move.
2. **Add/remove churn of a 16-byte component, Table vs Sparse,** at 1K/10K/100K entities.
3. **Filtered iteration `View<Position, S>`,** with S at 1/50/99 % density, Sparse vs Table, plus a sparse-driven `View<S>`, all at 100K.
4. **Random `GetComponent`** for each kind, at 10K/100K/1M, in a fixed shuffled order.

**Benchmarks are Release-only.** Debug numbers are meaningless. Every command below builds `ide\AstraBenchmark.vcxproj` in Release.

- [ ] **Step 1: Add the Google Benchmark family**

In `benchmark/Benchmark.cpp`, first add three includes after `#include <cstdint>`:

```cpp
#include <algorithm>
#include <string>
#include <type_traits>
```

Then insert the following block immediately BEFORE the final `BENCHMARK_MAIN();`:

```cpp
// ============================================================================
// Storage policies (spec 2026-10-01 s9.3). Every name starts with BM_Storage_ so
// the S4 Table-only regression gate excludes them: --benchmark_filter=-BM_Storage_
// ============================================================================
namespace StorageBench
{
    // The Arcane Identity shape: 16 B GUID + std::string name (48 B on MSVC x64),
    // non-trivially copyable -- every archetype move string-moves it while it is Table.
    struct BenchIdentity
    {
        std::uint64_t guidHi = 0;
        std::uint64_t guidLo = 0;
        std::string name;

        template<typename Archive>
        void Serialize(Archive& ar) { ar(guidHi)(guidLo)(name); }
    };
#if defined(_MSC_VER)
    static_assert(sizeof(BenchIdentity) == 48, "BenchIdentity must match Arcane::Identity's 48 bytes");
#endif
    static_assert(!std::is_trivially_copyable_v<BenchIdentity>);

    struct ChurnT { std::uint64_t a, b; };                                                    // 16 B, Table
    struct ChurnS { static constexpr auto AstraStorage = Astra::Storage::Sparse; std::uint64_t a, b; };   // 16 B, Sparse
    static_assert(sizeof(ChurnT) == 16 && sizeof(ChurnS) == 16);

    template<class... Ts>
    std::shared_ptr<Astra::ComponentRegistry> Components()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Ts...>();
        return creg;
    }

    // Scattered membership at `pct` percent (multiplicative hash, deterministic): the
    // realistic case -- holders are interleaved with non-holders inside every chunk.
    inline bool Holds(size_t i, int pct) { return ((i * 2654435761ull) >> 7) % 100 < static_cast<size_t>(pct); }

    // 1. Structural churn with the 48 B Identity present on every entity: one timed op =
    //    add Velocity + remove Velocity (2 archetype moves). Universal takes Identity out of both.
    template<bool Universal>
    void IdentityChurn(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<BenchIdentity>();
        Astra::Registry registry(Components<Position, Velocity, BenchIdentity>(), cfg);
        std::vector<Astra::Entity> entities;
        entities.reserve(count);
        for (size_t i = 0; i < count; ++i)
            entities.push_back(registry.CreateEntityWith(Position{i, i}, BenchIdentity{i, ~i, "Entity " + std::to_string(i)}));
        for (auto _ : state)
        {
            for (Astra::Entity e : entities) registry.EmplaceComponent<Velocity>(e);
            for (Astra::Entity e : entities) registry.RemoveComponent<Velocity>(e);
        }
        state.SetItemsProcessed(state.iterations() * count * 2);
    }

    // 2. Add/remove churn of a 16 B component (S4: Sparse must beat Table at every size).
    template<class C>
    void AddRemoveChurn(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        Astra::Registry registry(Components<Position, C>());
        std::vector<Astra::Entity> entities;
        entities.reserve(count);
        for (size_t i = 0; i < count; ++i) entities.push_back(registry.CreateEntityWith(Position{i, i}));
        for (auto _ : state)
        {
            for (Astra::Entity e : entities) registry.AddComponent(e, C{1, 2});
            for (Astra::Entity e : entities) registry.RemoveComponent<C>(e);
        }
        state.SetItemsProcessed(state.iterations() * count * 2);
    }

    // 3. Filtered iteration View<Position, C> with C on pct% of the entities.
    template<class C>
    void FilteredIter(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        const int pct = static_cast<int>(state.range(1));
        Astra::Registry registry(Components<Position, C>());
        size_t matched = 0;
        for (size_t i = 0; i < count; ++i)
        {
            Astra::Entity e = registry.CreateEntityWith(Position{i, i});
            if (Holds(i, pct)) { registry.AddComponent(e, C{i, i}); ++matched; }
        }
        auto view = registry.CreateView<Position, C>();
        for (auto _ : state)
        {
            std::uint64_t sum = 0;
            view.ForEach([&](Astra::Entity, Position& p, C& c) { sum += p.x + c.a; });
            benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * count);   // per scanned entity
        state.counters["matched"] = static_cast<double>(matched);
    }

    // 3b. View<C> alone at the same densities: the Sparse one is sparse-driven (s6.2).
    template<class C>
    void SoleIter(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        const int pct = static_cast<int>(state.range(1));
        Astra::Registry registry(Components<Position, C>());
        size_t matched = 0;
        for (size_t i = 0; i < count; ++i)
        {
            Astra::Entity e = registry.CreateEntityWith(Position{i, i});
            if (Holds(i, pct)) { registry.AddComponent(e, C{i, i}); ++matched; }
        }
        auto view = registry.CreateView<C>();
        for (auto _ : state)
        {
            std::uint64_t sum = 0;
            view.ForEach([&](Astra::Entity, C& c) { sum += c.a; });
            benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * matched);
        state.counters["matched"] = static_cast<double>(matched);
    }
```

The block ends with the random-get workload and the registrations:

```cpp
    // 4. Random GetComponent, shuffled order (cache-hostile), per kind.
    template<class C, bool Universal>
    void RandomGet(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<C>();
        Astra::Registry registry(Components<Position, C>(), cfg);
        std::vector<Astra::Entity> entities(count);
        for (size_t i = 0; i < count; ++i) entities[i] = registry.CreateEntityWith(Position{i, i}, C{i, i});
        std::mt19937 rng(0xC0FFEE);
        std::shuffle(entities.begin(), entities.end(), rng);
        for (auto _ : state)
        {
            std::uint64_t sum = 0;
            for (Astra::Entity e : entities) sum += registry.GetComponent<C>(e)->a;
            benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * count);
    }
} // namespace StorageBench

static void BM_Storage_IdentityChurn_Table(benchmark::State& s)     { StorageBench::IdentityChurn<false>(s); }
static void BM_Storage_IdentityChurn_Universal(benchmark::State& s) { StorageBench::IdentityChurn<true>(s); }
static void BM_Storage_AddRemoveChurn_Table(benchmark::State& s)    { StorageBench::AddRemoveChurn<StorageBench::ChurnT>(s); }
static void BM_Storage_AddRemoveChurn_Sparse(benchmark::State& s)   { StorageBench::AddRemoveChurn<StorageBench::ChurnS>(s); }
static void BM_Storage_FilteredIter_Table(benchmark::State& s)      { StorageBench::FilteredIter<StorageBench::ChurnT>(s); }
static void BM_Storage_FilteredIter_Sparse(benchmark::State& s)     { StorageBench::FilteredIter<StorageBench::ChurnS>(s); }
static void BM_Storage_SoleIter_Table(benchmark::State& s)          { StorageBench::SoleIter<StorageBench::ChurnT>(s); }
static void BM_Storage_SoleIter_Sparse(benchmark::State& s)         { StorageBench::SoleIter<StorageBench::ChurnS>(s); }
static void BM_Storage_RandomGet_Table(benchmark::State& s)         { StorageBench::RandomGet<StorageBench::ChurnT, false>(s); }
static void BM_Storage_RandomGet_Sparse(benchmark::State& s)        { StorageBench::RandomGet<StorageBench::ChurnS, false>(s); }
static void BM_Storage_RandomGet_Universal(benchmark::State& s)     { StorageBench::RandomGet<StorageBench::ChurnT, true>(s); }

BENCHMARK(BM_Storage_IdentityChurn_Table)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_IdentityChurn_Universal)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_AddRemoveChurn_Table)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_AddRemoveChurn_Sparse)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_FilteredIter_Table)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_FilteredIter_Sparse)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_SoleIter_Table)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_SoleIter_Sparse)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_RandomGet_Table)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_Storage_RandomGet_Sparse)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_Storage_RandomGet_Universal)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
```

- [ ] **Step 2: Build the benchmark in Release and smoke-run the new family**

Run (PowerShell, from `D:\dev\starworks\Astra-storage`):
`msbuild ide\AstraBenchmark.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`

Expected: `Build succeeded.` The output is `bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe`.

Then run:
`.\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe --benchmark_filter=BM_Storage_ --benchmark_repetitions=3 --benchmark_report_aggregates_only=true`

Expected:
- 31 benchmark rows (4 identity + 6 churn + 6 filtered + 6 sole + 9 random-get) x 3 aggregates (mean / median / stddev);
- no crash;
- the `matched` counter for `FilteredIter`/`SoleIter` is about 1 % / 50 % / 99 % of 100000.

A Debug assert does not fire here, because Release defines NDEBUG. Correctness is the test suite's job.

- [ ] **Step 3: Copy the shared hand-rolled harness header (read-only source)**

Run:
`Copy-Item D:\dev\starworks\Astra\bench-compare\bench_common.hpp D:\dev\starworks\Astra-storage\bench-compare\bench_common.hpp`

This READS the untracked harness of the main checkout. Nothing under `D:\dev\starworks\Astra` is written. The copy is committed, so the storage bench is reproducible from this branch alone.

- [ ] **Step 4: Write the hand-rolled storage bench**

Create `bench-compare/bench_storage.cpp`:

```cpp
// Storage-policy ops for the hand-rolled harness (spec 2026-10-01 s9.3), same protocol as
// bench_astra.cpp (bench_common.hpp: CSV lib,op,N,ns_per_op,items). Astra-only: EnTT and
// flecs have no Table/Sparse switch for one type and no per-registry universal column.
#include "bench_common.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <Astra/Astra.hpp>

const char* kLib = "astra-storage";

namespace
{
    struct BenchIdentity   // Arcane::Identity shape: 48 B on MSVC x64, non-trivially copyable
    {
        uint64_t guidHi = 0;
        uint64_t guidLo = 0;
        std::string name;

        template<typename Archive>
        void Serialize(Archive& ar) { ar(guidHi)(guidLo)(name); }
    };
    struct ChurnT { uint64_t a, b; };
    struct ChurnS { static constexpr auto AstraStorage = Astra::Storage::Sparse; uint64_t a, b; };

    template<class... Ts>
    std::shared_ptr<Astra::ComponentRegistry> Components()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Ts...>();
        return creg;
    }

    bool Holds(size_t i, int pct) { return ((i * 2654435761ull) >> 7) % 100 < static_cast<size_t>(pct); }

    struct State { std::unique_ptr<Astra::Registry> reg; std::vector<Astra::Entity> ents; };

    template<bool Universal>
    State BuildIdentity(size_t N)
    {
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<BenchIdentity>();
        State s;
        s.reg = std::make_unique<Astra::Registry>(Components<Position, Velocity, BenchIdentity>(), cfg);
        s.ents.reserve(N);
        for (size_t i = 0; i < N; ++i)
            s.ents.push_back(s.reg->CreateEntityWith(Position{float(i), 0.0f, 0.0f},
                                                     BenchIdentity{i, ~uint64_t(i), "Entity " + std::to_string(i)}));
        return s;
    }

    template<class C>
    State BuildPositions(size_t N, int pct)
    {
        State s;
        s.reg = std::make_unique<Astra::Registry>(Components<Position, C>());
        s.ents.reserve(N);
        for (size_t i = 0; i < N; ++i)
        {
            Astra::Entity e = s.reg->CreateEntityWith(Position{float(i), 0.0f, 0.0f});
            if (Holds(i, pct)) s.reg->AddComponent(e, C{i, i});
            s.ents.push_back(e);
        }
        return s;
    }

    // ns_per_op = per entity for one add + one remove (items = 2N).
    template<bool Universal>
    void IdentityChurn(const char* op, size_t N)
    {
        report(op, N, median_structural(5, [N] { return BuildIdentity<Universal>(N); }, [](State& s) {
            for (Astra::Entity e : s.ents) s.reg->AddComponent(e, Velocity{1.0f, 1.0f, 1.0f});
            for (Astra::Entity e : s.ents) s.reg->RemoveComponent<Velocity>(e);
        }), N * 2);
    }

    template<class C>
    void AddRemoveChurn(const char* op, size_t N)
    {
        report(op, N, median_structural(5, [N] { return BuildPositions<C>(N, 0); }, [](State& s) {
            for (Astra::Entity e : s.ents) s.reg->AddComponent(e, C{1, 2});
            for (Astra::Entity e : s.ents) s.reg->RemoveComponent<C>(e);
        }), N * 2);
    }

    template<class C>
    void FilteredIter(const char* prefix, size_t N, int pct)
    {
        State s = BuildPositions<C>(N, pct);
        auto view = s.reg->CreateView<Position, C>();
        char op[64];
        std::snprintf(op, sizeof op, "%s_%d", prefix, pct);
        report(op, N, median_persistent(7, [&]() {
            double acc = 0.0;
            view.ForEach([&](Astra::Entity, Position& p, C& c) { acc += p.x + double(c.a); });
            g_sink += acc;
        }), N);
    }

    template<class C>
    void SoleIter(const char* prefix, size_t N, int pct)
    {
        State s = BuildPositions<C>(N, pct);
        auto view = s.reg->CreateView<C>();
        char op[64];
        std::snprintf(op, sizeof op, "%s_%d", prefix, pct);
        report(op, N, median_persistent(7, [&]() {
            double acc = 0.0;
            view.ForEach([&](Astra::Entity, C& c) { acc += double(c.a); });
            g_sink += acc;
        }), N);
    }

    template<class C, bool Universal>
    void RandomGet(const char* op, size_t N)
    {
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<C>();
        Astra::Registry reg(Components<Position, C>(), cfg);
        std::vector<Astra::Entity> ents(N);
        for (size_t i = 0; i < N; ++i) ents[i] = reg.CreateEntityWith(Position{float(i), 0.0f, 0.0f}, C{i, i});
        const std::vector<uint32_t> order = shuffled_indices(N);
        report(op, N, median_persistent(7, [&]() {
            uint64_t acc = 0;
            for (uint32_t i : order) acc += reg.GetComponent<C>(ents[i])->a;
            g_sink += double(acc);
        }), N);
    }
}

int main()
{
    IdentityChurn<false>("identity_churn_table", 100'000);
    IdentityChurn<true>("identity_churn_universal", 100'000);
    for (size_t n : {size_t(1'000), size_t(10'000), size_t(100'000)})
    {
        AddRemoveChurn<ChurnT>("addremove_table", n);
        AddRemoveChurn<ChurnS>("addremove_sparse", n);
    }
    for (int pct : {1, 50, 99})
    {
        FilteredIter<ChurnT>("filter_table", 1'000'000, pct);
        FilteredIter<ChurnS>("filter_sparse", 1'000'000, pct);
        SoleIter<ChurnT>("sole_table", 1'000'000, pct);
        SoleIter<ChurnS>("sole_sparse", 1'000'000, pct);
    }
    RandomGet<ChurnT, false>("random_get_table", 1'000'000);
    RandomGet<ChurnS, false>("random_get_sparse", 1'000'000);
    RandomGet<ChurnT, true>("random_get_universal", 1'000'000);
    std::fprintf(stderr, "# astra-storage sink=%f\n", (double)g_sink);
    return 0;
}
```

- [ ] **Step 5: Write the build script and the results formatter**

Create `bench-compare/build_storage.bat`. It uses the same flags as the harness's `build_fresh.bat`, but runs in place in this worktree:

```bat
@echo off
rem Builds bench_storage.exe (Release-equivalent: /O2 /GL /LTCG, NDEBUG, ASTRA_BUILD_DIST) in place.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /nologo /DASTRA_BUILD_DIST /I..\include /I..\vendor\Mosaic\include /I..\tests bench_storage.cpp /Fe:bench_storage.exe /link /LTCG advapi32.lib
echo BUILD_EXIT=%ERRORLEVEL%
```

Create `bench-compare/storage_results.ps1`:

```powershell
# Folds bench_storage round CSVs (lib,op,N,ns_per_op,items) into the RESULTS.md table:
# median [min, max] ns/op per (op, N) across rounds.
param([string]$Pattern = 'storage_round*.csv')

$rows = foreach ($file in Get-ChildItem -Path $PSScriptRoot -Filter $Pattern)
{
    Get-Content $file.FullName | Where-Object { $_ -match '^astra-storage,' } | ForEach-Object {
        $p = $_.Split(',')
        [pscustomobject]@{ Op = $p[1]; N = [int64]$p[2]; Ns = [double]$p[3] }
    }
}
if (-not $rows) { Write-Error "no rows matched $Pattern in $PSScriptRoot"; exit 1 }

'| op | N | median ns/op | min | max | rounds |'
'|---|---|---|---|---|---|'
$groups = $rows | Group-Object -Property Op, N | Sort-Object { $_.Group[0].Op }, { $_.Group[0].N }
foreach ($g in $groups)
{
    $v = @($g.Group | ForEach-Object { $_.Ns } | Sort-Object)
    $median = $v[[int][math]::Floor($v.Count / 2)]
    '| {0} | {1} | {2:N3} | {3:N3} | {4:N3} | {5} |' -f $g.Group[0].Op, $g.Group[0].N, $median, $v[0], $v[$v.Count - 1], $v.Count
}
```

- [ ] **Step 6: Build and smoke-run the hand-rolled bench**

Run, in order:
1. `cmd /c D:\dev\starworks\Astra-storage\bench-compare\build_storage.bat`
   - Expected last line: `BUILD_EXIT=0`. The output is `bench-compare\bench_storage.exe`, which is gitignored (`*.exe`).
2. `D:\dev\starworks\Astra-storage\bench-compare\bench_storage.exe | Out-File -Encoding utf8 D:\dev\starworks\Astra-storage\bench-compare\storage_smoke.csv`
   - Expected: **23** CSV rows starting `astra-storage,`:
     - `identity_churn_*`: 2;
     - `addremove_*`: 6;
     - `filter_*`: 6;
     - `sole_*`: 6;
     - `random_get_*`: 3.
   - The run takes a few minutes. stderr ends with `# astra-storage sink=...`.

Check the count with:
`(Get-Content bench-compare\storage_smoke.csv | Where-Object { $_ -match '^astra-storage,' }).Count` -> `23`

Delete the smoke file afterwards: `Remove-Item bench-compare\storage_smoke.csv`.

- [ ] **Step 7: Commit**

```powershell
git add benchmark/Benchmark.cpp bench-compare/bench_common.hpp bench-compare/bench_storage.cpp bench-compare/build_storage.bat bench-compare/storage_results.ps1
git commit -m "perf(storage): storage-policy benchmarks -- 48 B Identity structural churn Table vs Universal, 16 B add/remove churn Table vs Sparse at 1K/10K/100K, filtered View<Pos,S> and View<S> at 1/50/99% density, random Get per kind; Google Benchmark BM_Storage_* + the hand-rolled bench_storage harness" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 42: The S4 gate, RESULTS.md, spec status, README (s2 S4, s9.3)

**Files:**
- Create: `benchmark/s4_gate.ps1`, the gate comparator and the storage acceptance check.
- Modify: `bench-compare/RESULTS.md` (append the "Storage policies" section).
- Modify: `docs/superpowers/specs/2026-10-01-astra-storage-policies-design.md` (the Status line).
- Modify: `README.md` (Features bullet + a "Storage policies" section).

**Interfaces:**
- Consumes the Release `AstraBenchmark.exe` of `975cdb7` (built from a `git archive` export) and of this branch.
- Consumes `bench-compare/bench_storage.exe` and `storage_results.ps1` (Task 41).
- Produces the S4 verdict, recorded in RESULTS.md:
  1. Every Table-only benchmark is within +2 % of `975cdb7`, over 3 interleaved runs, noise-checked.
  2. Universal improves the 48 B structural churn measurably.
  3. Sparse add/remove churn beats Table at 1K, 10K and 100K.

**Ground rules (from RESULTS.md's methodology history):**
- Run on a quiet machine: AC power, High-performance plan, no build or IDE indexing, browsers closed.
- Interleave base and branch runs (base, branch, base, branch...), so drift hits both equally.
- Compare medians.
- A row whose base spread exceeds the threshold is NOISY. A NOISY row is re-measured, never waved through.
- If S4 fails, STOP and report with the numbers. Do not tune thresholds, drop rows or reword the criterion. S4 is a spec success criterion and the human decides.

- [ ] **Step 1: Write the gate script**

Create `benchmark/s4_gate.ps1`:

```powershell
# S4 regression gate + storage acceptance check (spec 2026-10-01 s2 S4, s9.3).
#
# Gate mode:       -Dir <folder with base_<r>.json and branch_<r>.json>
#   Every Google Benchmark present in both is compared on the MEDIAN real_time across rounds.
#   PASS  : branch within +Threshold % of base.
#   NOISY : over threshold, but the base's own round-to-round spread also exceeds it -> re-measure.
#   FAIL  : over threshold on a quiet row.
#   Exit 0 only when every row PASSes and no base benchmark is missing on the branch.
#
# Storage mode:    -StorageJson <AstraBenchmark --benchmark_filter=BM_Storage_ --benchmark_repetitions=3 json>
#   Prints the BM_Storage_ median table and checks S4 (2) Universal identity churn < Table, and
#   (3) Sparse add/remove churn < Table at 1K/10K/100K. Exit 0 only when both hold.
#
# -Markdown prints tables in RESULTS.md form.
param(
    [string]$Dir,
    [double]$Threshold = 2.0,
    [switch]$Markdown,
    [string]$StorageJson
)

function Get-Median([double[]]$xs)
{
    $s = @($xs | Sort-Object)
    return $s[[int][math]::Floor($s.Count / 2)]
}

if ($StorageJson)
{
    $json = Get-Content $StorageJson -Raw | ConvertFrom-Json
    $med = @{}
    '| benchmark | median real time | unit |'
    '|---|---|---|'
    foreach ($b in @($json.benchmarks | Where-Object { $_.aggregate_name -eq 'median' }))
    {
        $med[$b.run_name] = [double]$b.real_time
        '| {0} | {1:N1} | {2} |' -f $b.run_name, $b.real_time, $b.time_unit
    }
    $ok = $true
    ''
    foreach ($n in 10000, 100000)
    {
        $t = $med["BM_Storage_IdentityChurn_Table/$n"]; $u = $med["BM_Storage_IdentityChurn_Universal/$n"]
        $pass = ($t -and $u -and $u -lt $t)
        $ok = $ok -and $pass
        'S4(2) identity churn N={0}: universal {1:N0} vs table {2:N0} ({3:N1}% faster) -> {4}' -f $n, $u, $t, ((1 - $u / $t) * 100), $(if ($pass) { 'PASS' } else { 'FAIL' })
    }
    foreach ($n in 1000, 10000, 100000)
    {
        $t = $med["BM_Storage_AddRemoveChurn_Table/$n"]; $s = $med["BM_Storage_AddRemoveChurn_Sparse/$n"]
        $pass = ($t -and $s -and $s -lt $t)
        $ok = $ok -and $pass
        'S4(3) add/remove churn N={0}: sparse {1:N0} vs table {2:N0} ({3:N1}% faster) -> {4}' -f $n, $s, $t, ((1 - $s / $t) * 100), $(if ($pass) { 'PASS' } else { 'FAIL' })
    }
    if ($ok) { 'S4 STORAGE ACCEPTANCE: PASS'; exit 0 } else { 'S4 STORAGE ACCEPTANCE: FAIL'; exit 1 }
}

if (-not $Dir) { Write-Error 'pass -Dir (gate mode) or -StorageJson (storage mode)'; exit 2 }

function Read-Runs([string]$pattern)
{
    $map = @{}
    foreach ($file in @(Get-ChildItem -Path $Dir -Filter $pattern | Sort-Object Name))
    {
        $json = Get-Content $file.FullName -Raw | ConvertFrom-Json
        foreach ($b in $json.benchmarks)
        {
            if ($b.run_type -eq 'aggregate') { continue }   # keep per-repetition iteration rows only
            if (-not $map.ContainsKey($b.name)) { $map[$b.name] = New-Object System.Collections.Generic.List[double] }
            $map[$b.name].Add([double]$b.real_time)
        }
    }
    return $map
}

$base = Read-Runs 'base_*.json'
$branch = Read-Runs 'branch_*.json'
if ($base.Count -eq 0 -or $branch.Count -eq 0) { Write-Error "no base_*.json / branch_*.json in $Dir"; exit 2 }

$rows = @()
foreach ($name in @($base.Keys | Sort-Object))
{
    if (-not $branch.ContainsKey($name)) { continue }
    $b = [double[]]$base[$name].ToArray()
    $n = [double[]]$branch[$name].ToArray()
    $mb = Get-Median $b
    $mn = Get-Median $n
    $spread = ((($b | Measure-Object -Maximum).Maximum) - (($b | Measure-Object -Minimum).Minimum)) / $mb * 100.0
    $delta = ($mn - $mb) / $mb * 100.0
    $verdict = if ($delta -le $Threshold) { 'PASS' } elseif ($spread -gt $Threshold) { 'NOISY' } else { 'FAIL' }
    $rows += [pscustomobject]@{
        Benchmark = $name; Base = $mb; Branch = $mn; DeltaPct = $delta; BaseSpreadPct = $spread
        Rounds = '{0}/{1}' -f $b.Count, $n.Count; Verdict = $verdict
    }
}
$missing = @($base.Keys | Where-Object { -not $branch.ContainsKey($_) })

if ($Markdown)
{
    '| benchmark | 975cdb7 median | branch median | delta % | base spread % | rounds | verdict |'
    '|---|---|---|---|---|---|---|'
    foreach ($r in $rows)
    {
        '| {0} | {1:N1} | {2:N1} | {3:+0.00;-0.00} | {4:N2} | {5} | {6} |' -f $r.Benchmark, $r.Base, $r.Branch, $r.DeltaPct, $r.BaseSpreadPct, $r.Rounds, $r.Verdict
    }
}
else
{
    $rows | Format-Table -AutoSize | Out-String -Width 240
}

$pass = @($rows | Where-Object { $_.Verdict -eq 'PASS' }).Count
$noisy = @($rows | Where-Object { $_.Verdict -eq 'NOISY' })
$fail = @($rows | Where-Object { $_.Verdict -eq 'FAIL' })
'S4 GATE: {0} compared, {1} PASS, {2} NOISY, {3} FAIL, {4} missing on branch' -f $rows.Count, $pass, $noisy.Count, $fail.Count, $missing.Count
foreach ($m in $missing) { "  missing on branch: $m" }
foreach ($r in $noisy) { "  NOISY (re-measure): $($r.Benchmark)" }
foreach ($r in $fail) { "  FAIL: $($r.Benchmark) {0:+0.00}%" -f $r.DeltaPct }
if ($noisy.Count -gt 0 -or $fail.Count -gt 0 -or $missing.Count -gt 0) { exit 1 }
exit 0
```

- [ ] **Step 2: Build the base (`975cdb7`) benchmark from an export**

`git archive` only reads the object store: no worktree is added, and nothing under `D:\dev\starworks\Astra` is touched. Run in PowerShell:

```powershell
$base = 'D:\dev\starworks\Astra-storage-base'
if (Test-Path $base) { Remove-Item -Recurse -Force $base }
New-Item -ItemType Directory $base | Out-Null
git -C D:\dev\starworks\Astra-storage archive --format=tar -o "$base.tar" 975cdb7
tar -xf "$base.tar" -C $base
Remove-Item "$base.tar"
Push-Location $base
D:\dev\starworks\Arcane\ThirdParty\premake5\premake5.exe vs2026
Pop-Location
msbuild "$base\ide\AstraBenchmark.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=$base\ -m -nr:false
```

Expected:
- `Build succeeded.`;
- `D:\dev\starworks\Astra-storage-base\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe` exists.

The archive includes `vendor/` (tracked), so GoogleBenchmark builds from the same sources as the branch.

- [ ] **Step 3: Rebuild the branch benchmark at HEAD**

Run: `msbuild ide\AstraBenchmark.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`

Expected: `Build succeeded.` Run `git status --short` too: nothing tracked is modified, so the exe matches HEAD.

- [ ] **Step 4: Run the gate rounds, interleaved, on a quiet machine**

Pre-check the load with:
`Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 2 -MaxSamples 5`

The samples should mostly be under 10 %. If they are not, find the load, stop it, and re-check. Record the samples.

Then run:

```powershell
$root = 'D:\dev\starworks\Astra-storage'
$base = 'D:\dev\starworks\Astra-storage-base'
$gate = "$root\bin\s4-gate"
New-Item -ItemType Directory -Force $gate | Out-Null
foreach ($r in 1..3)
{
    & "$base\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe" --benchmark_out="$gate\base_$r.json" --benchmark_out_format=json
    & "$root\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe" --benchmark_filter=-BM_Storage_ --benchmark_out="$gate\branch_$r.json" --benchmark_out_format=json
}
```

Expected: six JSON files in `bin\s4-gate` (`bin/` is gitignored). The full suite takes several minutes per run, so budget about an hour.

- [ ] **Step 5: Evaluate the gate**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\s4_gate.ps1 -Dir bin\s4-gate`

Expected: last line `S4 GATE: <n> compared, <n> PASS, 0 NOISY, 0 FAIL, 0 missing on branch`, and exit code 0. `<n>` is 100: the base registers 37 benchmarks expanding to 100 rows (18 create/add/remove + 18 ForEach + 15 range-for + 18 parallel + 6 get + 15 relations + 10 scheduler).

If a row is **NOISY**, re-measure exactly those rows with 9 repetitions on both sides, interleaved:

```powershell
$root = 'D:\dev\starworks\Astra-storage'
$base = 'D:\dev\starworks\Astra-storage-base'
$names = '^(BM_ParallelIterateTwoComponents/1000000|BM_ForEachDescendant/100000)$'   # the NOISY names, from the report
$re = "$root\bin\s4-gate\rerun"
New-Item -ItemType Directory -Force $re | Out-Null
& "$base\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe" --benchmark_filter=$names --benchmark_repetitions=9 --benchmark_out="$re\base_1.json" --benchmark_out_format=json
& "$root\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe" --benchmark_filter=$names --benchmark_repetitions=9 --benchmark_out="$re\branch_1.json" --benchmark_out_format=json
powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\s4_gate.ps1 -Dir $re
```

Replace `$names` with the actual NOISY names from the Step 5 report. The two names shown are examples of the expected format, an anchored alternation.

A row that is still NOISY after re-measuring, or any **FAIL**, means S4 is not met:
- STOP;
- write the gate table into the task report;
- report to the human.

Never weaken the gate. Do not raise `-Threshold`, drop rows, or re-run until a lucky round passes. A FAIL is the R2 risk materialising (spec s11): investigate the Table path, for example a per-entity kind branch that was meant to be hoisted per view or per chunk.

When the gate passes, capture the markdown table for RESULTS.md:
`powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\s4_gate.ps1 -Dir bin\s4-gate -Markdown | Out-File -Encoding utf8 bin\s4-gate\gate.md`

If NOISY rows were re-measured, add the re-run table too:
`powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\s4_gate.ps1 -Dir bin\s4-gate\rerun -Markdown | Out-File -Encoding utf8 bin\s4-gate\gate_rerun.md`

- [ ] **Step 6: Storage benchmarks + the S4 storage acceptance (parts 2 and 3)**

Run:

```powershell
$root = 'D:\dev\starworks\Astra-storage'
$gate = "$root\bin\s4-gate"
& "$root\bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe" --benchmark_filter=BM_Storage_ --benchmark_repetitions=3 --benchmark_out="$gate\storage.json" --benchmark_out_format=json
powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\s4_gate.ps1 -StorageJson bin\s4-gate\storage.json | Out-File -Encoding utf8 bin\s4-gate\storage.md
Get-Content bin\s4-gate\storage.md -Tail 6
```

Expected tail:
- two `S4(2) identity churn ... -> PASS` lines (N = 10000, 100000);
- three `S4(3) add/remove churn ... -> PASS` lines (N = 1000, 10000, 100000);
- `S4 STORAGE ACCEPTANCE: PASS`.

Any FAIL is the same STOP-and-report as Step 5. S4 (2) says "improves measurably": a Universal median that is lower but within the base's own noise is reported as such, not claimed as a win.

- [ ] **Step 7: Run the hand-rolled storage rounds**

Run (same quiet-machine rules as Step 4):

```powershell
Push-Location D:\dev\starworks\Astra-storage\bench-compare
foreach ($r in 1..6) { .\bench_storage.exe | Out-File -Encoding utf8 "storage_round$r.csv" }
powershell -NoProfile -ExecutionPolicy Bypass -File .\storage_results.ps1 | Out-File -Encoding utf8 ..\bin\s4-gate\bench_storage.md
Pop-Location
Get-Content D:\dev\starworks\Astra-storage\bin\s4-gate\bench_storage.md
```

Expected: a markdown table with 23 rows, each `rounds` = 6. The round CSVs stay untracked, as the harness's other campaign CSVs are. Delete them after Step 8 with `Remove-Item bench-compare\storage_round*.csv`.

- [ ] **Step 8: Append the "Storage policies" section to `bench-compare/RESULTS.md`**

Collect the facts first:
- `git -C D:\dev\starworks\Astra-storage log -1 --format='%h %cs'` gives the short SHA and date;
- `(Get-CimInstance Win32_Processor).Name` gives the CPU;
- `[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)` gives the RAM in GB.

Append the section below at the END of `bench-compare/RESULTS.md`. Edit it with the Edit tool, which keeps the file's line endings. Every angle-bracket slot is filled mechanically from the named command or file. The tables are pasted VERBATIM from the generated `.md` files.

```markdown
## Storage policies (spec 2026-10-01; branch `feat/storage-policies` @ `<short SHA>`, measured <date>)

**Machine:** <CPU>, <RAM> GB, Windows 10 Pro 19045, MSVC v143 (VS 18 msbuild), Release (`/O2 /Oi /Ot /GL` + `/LTCG`, `/arch:AVX`). **Load pre-check:** <the five `% Processor Time` samples from Task 42 Step 4>.

**Protocol.**
- `975cdb7` was exported with `git archive` and built with the identical premake/msbuild Release configuration.
- Three interleaved rounds (base, branch, base, branch, base, branch) of the full Google Benchmark suite were run. The branch ran with `--benchmark_filter=-BM_Storage_`.
- Per benchmark, the gate compares the median `real_time` of the three rounds (`benchmark/s4_gate.ps1`).
- A row whose base spread exceeds 2 % is NOISY. NOISY rows were re-measured with 9 repetitions per side.

### S4 (1): Table-only benchmarks vs `975cdb7` (+2 % threshold)

<bin\s4-gate\gate.md, verbatim>

<only if Step 5 re-measured NOISY rows: the heading "Re-measured NOISY rows (9 repetitions per side)" and bin\s4-gate\gate_rerun.md, verbatim>

Verdict: `<the final "S4 GATE: ..." line of the passing gate run>`

### S4 (2) and (3): storage benchmarks (Google Benchmark, 3 repetitions, median)

<bin\s4-gate\storage.md, verbatim, including its S4(2) / S4(3) / ACCEPTANCE lines>

Workloads:
- `IdentityChurn` is one add + one remove of `Velocity` per entity, while a 48 B `std::string`-bearing Identity sits on every entity, as Table or Universal.
- `AddRemoveChurn` is one add + one remove of a 16 B component.
- `FilteredIter` is `View<Position, S>` over 100K entities, with S on 1/50/99 % (scattered).
- `SoleIter` is `View<S>` at the same densities; the Sparse variant is sparse-driven.
- `RandomGet` is a shuffled `GetComponent` per kind.

### Hand-rolled harness (`bench_storage.exe`, 6 rounds, median [min, max] ns/op)

<bin\s4-gate\bench_storage.md, verbatim>

ns/op is defined per op:
- `identity_churn_*`, `addremove_*`: per entity, for one add + one remove;
- `filter_*`: per scanned entity (1M);
- `sole_*`: per registry entity (1M);
- `random_get_*`: per lookup.

### Reading the numbers

<3-5 sentences, each citing a number from the tables above:>
- <where Sparse wins and by how much (churn);>
- <where Table stays ahead (dense iteration at 99 %, random get);>
- <what Universal buys on structural churn;>
- <that Table-only users see no regression (the gate).>

<No claim beyond the tables.>
```

- [ ] **Step 9: Mark the spec implemented**

In `docs/superpowers/specs/2026-10-01-astra-storage-policies-design.md`, replace line 4 (the `**Status:** DRAFT for user review (2026-10-01). ...` line) with:

```markdown
**Status:** IMPLEMENTED on `feat/storage-policies` (plan `docs/superpowers/plans/2026-10-01-astra-storage-policies.md`, Tasks 1-42; S4 gate and storage numbers in `bench-compare/RESULTS.md`, section "Storage policies"). Kept off `dev` until Arcane's T1-C7 has vendored `975cdb7`. Design approved 2026-10-01 (sections 1-2 explicitly, 3-7 as a whole); s12 items 1-10 implemented as written, s12 item 11 (cross-compiler template type-name hashes) still OPEN.
```

If the assembled plan file is named differently, cite its real path. Check with `Get-ChildItem docs\superpowers\plans\*storage*`.

- [ ] **Step 10: Document storage policies in `README.md`**

**10a.** In `## Features`, add this bullet directly after the `**Archetype-based storage**` bullet:

```markdown
- **Storage policies** - Per-component Table / Sparse / Universal layouts behind one API: Sparse components never fragment archetypes, Universal ones never move
```

**10b.** Insert this section directly BEFORE `### Query System`, which follows the "Archetype-Based Storage" list in `## Architecture Overview`:

````markdown
### Storage Policies

Every component uses one of three layouts. The public API is identical for all three -- `Add`/`Emplace`/`Remove`/`Get`/`Has`, `SetEnabled`, change detection, views and every query modifier, Relations filters, `CommandBuffer`, Save/Load -- and only performance differs. The single exception: a Universal component cannot be removed.

| Layout | Declared by | Data lives in | Fragments archetypes? | Good for |
|---|---|---|---|---|
| **Table** (default) | nothing | archetype chunk columns | yes | hot components iterated together |
| **Sparse** | the type: `static constexpr auto AstraStorage = Astra::Storage::Sparse;` | a per-registry sparse set; presence is mirrored into per-chunk bitsets | no | components that are toggled often, or held by few entities |
| **Universal** | the registry: `config.AddUniversal<T>(init, user)` | a paged column indexed by entity slot | no | components every entity has but hot loops rarely read (names, GUIDs) |

```cpp
struct Burning { float dps = 0.0f; static constexpr auto AstraStorage = Astra::Storage::Sparse; };
struct Identity { std::uint64_t guid = 0; std::string name; };

// Runs for every new entity that was NOT given an explicit Identity value.
void MintGuid(Astra::Entity e, void* component, void* /*user*/)
{
    static_cast<Identity*>(component)->guid = 0x9E3779B97F4A7C15ull ^ e.GetValue();
}

Astra::Registry::Config config;
config.AddUniversal<Identity>(&MintGuid);
Astra::Registry registry(config);

Astra::Entity e = registry.CreateEntity<Position>();   // Identity constructed, MintGuid ran
registry.AddComponent(e, Burning{5.0f});               // no archetype move
registry.CreateView<Position, Burning>().ForEach(      // Burning filters a 64-entity word at a time
    [](Astra::Entity, Position& p, Burning& b) { /* ... */ });
registry.RemoveComponent<Identity>(e);                 // refused: ASTRA_ASSERT in Debug, false in Release
```

Rules:
- **Universality is per registry.** A registry that does not declare `T` universal uses `T`'s trait. Registries that share one `ComponentRegistry` do not affect each other.
- **Sparse slots.** A registry holds at most `ASTRA_MAX_SPARSE_COMPONENTS` (default 32) Sparse components. Past the cap, the add asserts and is refused.
- **Tags.** Every tag may be enableable. Table tags use a bit-only column; Sparse tags keep the bit in the store.
- **Adding a present component** is a no-op that returns `true`, for every layout. A Universal component is always present.
- **Archives load whatever the storage was when they were written** (format v6). Values move into the layout the loading registry assigns. A Universal component missing from an archive is default-constructed and then `init`-ed. v5 archives load as all-Table.
- **Pointer validity** is unchanged: a component pointer is valid until the next structural change.
- **Choose with data.** `Debug::Inspector` reports each component's layout, population, pages and per-chunk presence blocks. Attach a `Debug::ChurnTracker` to get a "looks sparse-shaped" advisory for Table components that are toggled or rare-and-fragmenting. Nothing is promoted automatically.
- **Performance** numbers are in `bench-compare/RESULTS.md` ("Storage policies").
````

`Position` is the README's own example type from Quick Start. The example uses only public API.

- [ ] **Step 11: Final verification on the docs commit**

Run, in order:
1. `msbuild ide\AstraTest.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=D:\dev\starworks\Astra-storage\ -m -nr:false`
   - Expected: `Build succeeded.`
2. `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_shuffle --gtest_random_seed=0`
   - Expected: all PASSED, exit code 0. Record the seed line.
3. `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_also_run_disabled_tests --gtest_filter=StorageInvariantFuzz.DISABLED_LongRun`
   - Expected: PASSED. This is S3: the fuzzer is green at the branch's final state.
4. `git status --short`
   - Expected: only the files staged below are modified or added.
   - Untracked `bench-compare\*.exe` / `.obj` are gitignored.
   - `bench-compare\storage_round*.csv` were deleted in Step 8.

- [ ] **Step 12: Commit (two commits) and clean up**

```powershell
git add benchmark/s4_gate.ps1 bench-compare/RESULTS.md
git commit -m "perf(storage): S4 gate -- Table-only Google Benchmarks within 2% of 975cdb7 over 3 interleaved runs (noise-checked); Universal 48 B identity churn and Sparse add/remove churn wins recorded in RESULTS.md with machine and commit" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git add docs/superpowers/specs/2026-10-01-astra-storage-policies-design.md README.md
git commit -m "docs(storage): spec status -> implemented; README storage-policies section (Table / Sparse / Universal, declaration, rules, Inspector advisory)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
Remove-Item -Recurse -Force D:\dev\starworks\Astra-storage-base
```

Expected: two commits on `feat/storage-policies`, and NO push. The base export directory is gone. `bin\s4-gate` stays: it is untracked under `bin/` and is evidence for the task report.


