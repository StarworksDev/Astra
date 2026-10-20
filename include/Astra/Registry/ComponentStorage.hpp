#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
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
    };
}
