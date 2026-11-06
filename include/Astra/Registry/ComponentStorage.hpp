#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "../Archetype/ArchetypeChunkPool.hpp"
#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Core/Assert.hpp"
#include "../Core/Base.hpp"
#include "../Core/Tick.hpp"
#include "../Entity/Entity.hpp"
#include "../Serialization/SerializationError.hpp"
#include "../Serialization/StorageMigration.hpp"
#include "SparseSet.hpp"
#include "UniversalColumn.hpp"

namespace Astra
{
    class BinaryWriter;
    class BinaryReader;
    class Registry;

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
            for (ComponentID id : m_sparseIds)
                m_sparseSets[id]->SetTickSource(tick);
        }

        ASTRA_NODISCARD bool HasUniversals() const noexcept { return !m_universal.empty(); }

        // Mask-only: is `id` declared universal in this registry. Reads no descriptor, so a
        // Table-trait Get/Has can decide its routing without touching the registry (G4).
        ASTRA_NODISCARD bool IsDeclaredUniversal(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS && m_universalMask.Test(id);
        }

        // True once any universal column or sparse set exists. The Registry's destroy paths
        // skip the storage walk entirely while this is false (G4: Table-only registries pay
        // one predictable branch).
        ASTRA_NODISCARD bool HasAnyStore() const noexcept { return m_storeCount != 0; }

        // Format v6 block (spec s8.2). Serialize is defined in ComponentStorageSerialization.hpp;
        // Deserialize out of line after class Registry (its body needs the complete Registry).
        // On refusal it returns false and, when `error` is given, says why: UnknownComponent
        // for an unregistered hash, CorruptedData (or the reader's own error) for malformed data.
        void Serialize(BinaryWriter& w) const;
        bool Deserialize(BinaryReader& r, class Registry& registry, SerializationError* error = nullptr);

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

        // ================= Sparse sets (spec 2026-10-01 s4.1) =================

        // Lookup without creation. Every read path uses this (RULINGS G2).
        ASTRA_NODISCARD SparseSet* FindSparse(ComponentID id) const noexcept
        {
            return id < MAX_COMPONENTS ? m_sparseSets[id].get() : nullptr;
        }

        // The set for a Sparse-kind id. It is created on first use, which acquires the
        // id's sparse slot (spec s3.2). Returns nullptr for any other kind, for an
        // unregistered id, and once ASTRA_MAX_SPARSE_COMPONENTS slots are taken
        // (AcquireSparseSlot asserts in Debug). The caller then refuses the component,
        // so Release never silently writes it elsewhere. Insert-path only.
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

        // Destroy walk (spec s7.3). Called after EntityDestroyed and BEFORE the archetype
        // swap-remove (whose SwapRemoveSparseBits fixes the chunk bits). The chunk's
        // presence blocks name exactly the sparse sets holding `e` (P1), so destroy costs
        // O(blocks in the chunk), not O(sparse components). Then e's slot is destructed
        // in every universal column. Non-creating lookups: a destroy never builds a store,
        // and a slot/column released by a module unload is skipped. No ComponentRemoved.
        // Returns true when at least one SPARSE membership was erased: the caller bumps
        // the sparse structural counter on that, never for a bystander (spec s4.3 parity).
        bool OnEntityDestroyed(Entity e, ArchetypeChunk& chunk, size_t index)
        {
            bool erasedSparse = false;
            for (const ArchetypeChunk::SparseBlock& b : chunk.GetSparseBlocks())
            {
                if (!b.present || !chunk.IsSparsePresent(b.slot, index))
                    continue;                                  // mirror-only block, or not a member
                if (SparseSet* set = FindSparse(ComponentOfSlot(b.slot)))
                    erasedSparse |= set->Erase(e);
            }
            for (const UniversalSpec& spec : m_universal)
                if (UniversalColumn* column = FindUniversal(spec.id))
                    column->Destroy(e);
            return erasedSparse;
        }

        // Registry::Clear (spec s7.3): destroys every element IN PLACE (cached Views stay
        // valid). The columns, their descriptors and the specs survive, and so do the
        // sparse-slot assignments.
        void Clear()
        {
            for (const UniversalSpec& spec : m_universal)
                if (UniversalColumn* column = FindUniversal(spec.id))
                    column->Clear();
            for (ComponentID id : m_sparseIds)
                m_sparseSets[id]->Clear();   // the set and its slot survive (spec s7.3)
        }

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
                std::erase(m_sparseIds, id);   // all-sets loops skip it now
                --m_storeCount;
            }
            if (auto& column = m_universalColumns[id])
            {
                column->Clear();
                column.reset();
                --m_storeCount;
            }
            m_coarseVersion[id] = Tick{0};     // a reloaded type starts with no history
            const int slot = SparseSlotOf(id);
            if (slot >= 0)
            {
                // Slot-free rule: reset BOTH table entries, then the count.
                m_componentOfSlot[static_cast<size_t>(slot)] = INVALID_COMPONENT;
                m_slotOf[id] = -1;
                --m_usedSlots;
            }
        }

        // Coarse change version per UNTRACKED Sparse component (spec s3.3): the analogue
        // of a Table chunk-column version, stamped on every add, mutable Get, Modified
        // and storing SetIfNeq of the component; read by IsChanged / IsAdded for an
        // untracked Sparse element. 0 == never. Survives Clear() (a version is a time,
        // not a value). SparseSet keeps no coarse tick of its own.
        // `Par`: accessed from a parallel view driver where every worker shares this slot
        // (relaxed atomic_ref, RULINGS Task 28).
        template<bool Par = false>
        ASTRA_NODISCARD Tick CoarseVersion(ComponentID id) const noexcept
        {
            if (id >= MAX_COMPONENTS)
                return Tick{0};
            if constexpr (Par)
                return LoadTickRelaxed(m_coarseVersion[id]);
            else
                return m_coarseVersion[id];
        }
        template<bool Par = false>
        void StampCoarse(ComponentID id, Tick tick) noexcept
        {
            if (id < MAX_COMPONENTS) ASTRA_LIKELY
            {
                if constexpr (Par)
                    StampTickShared(m_coarseVersion[id], tick);
                else
                    m_coarseVersion[id] = tick;
            }
        }

        // s8.3: Table-now values read from a v6 block, applied after archetype load.
        ASTRA_NODISCARD std::vector<StagedTableColumn>& PendingTableMigrations() noexcept { return m_pendingTable; }

    private:
        // One lookup loop: the mutable form goes through the const one.
        UniversalSpec* FindUniversalSpecMutable(ComponentID id) noexcept
        {
            return const_cast<UniversalSpec*>(std::as_const(*this).FindUniversalSpec(id));
        }

        std::shared_ptr<ComponentRegistry> m_registry;
        std::vector<UniversalSpec> m_universal;                              // deduplicated by id, declaration order
        ComponentMask m_universalMask{};                                     // id -> declared universal here
        std::array<int16_t, MAX_COMPONENTS> m_slotOf{};                      // id -> sparse slot, -1 = none
        std::array<ComponentID, MAX_SPARSE_COMPONENTS> m_componentOfSlot{};  // slot -> id, INVALID_COMPONENT = free
        size_t m_usedSlots = 0;
        std::array<std::unique_ptr<UniversalColumn>, MAX_COMPONENTS> m_universalColumns{};
        std::array<std::unique_ptr<SparseSet>, MAX_COMPONENTS> m_sparseSets{};
        std::vector<ComponentID> m_sparseIds;   // ids with a live set, in creation order
        const Tick* m_tickSource = nullptr;
        Tick m_coarseVersion[MAX_COMPONENTS]{};   // untracked-Sparse coarse change version, by id
        std::uint32_t m_storeCount = 0;   // columns + sparse sets created so far (HasAnyStore)
        std::vector<StagedTableColumn> m_pendingTable;   // s8.3 staged Table-now values (load only)
    };
}
