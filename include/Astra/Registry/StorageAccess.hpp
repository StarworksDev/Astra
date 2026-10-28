#pragma once

#include <type_traits>

#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Core/Tick.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"
#include "ComponentStorage.hpp"

namespace Astra::Detail
{
    // The ONE per-type instance a store-kind tag hands out as its typed payload (the
    // convention ArchetypeChunk::GetComponent<T> uses for a column-less tag). Registry's
    // StoreGet / SparseTypedPtr and Relations' GetInStorage all return it, so a Sparse tag's
    // payload equals GetComponent<Tag>(e) on every path.
    template<typename T>
    ASTRA_NODISCARD T* StoreTagInstance() noexcept
    {
        static T s_tag{};
        return &s_tag;
    }

    // The ONE write stamp of a present Sparse element (spec s3.3): the element's `changed`
    // tick when tracked (inside SparseSet::MarkWritten) plus the component's coarse version.
    // Returns false for a tag, an absent or a stale member (nothing stamped).
    // `Par`: the caller may run on several workers sharing the coarse slot (RULINGS Task 28,
    // relaxed atomic_ref store); the per-element tick belongs to this entity alone.
    template<bool Par = false>
    inline bool StampSparseWrite(ComponentStorage& storage, SparseSet& set, ComponentID id, Entity e, Tick now) noexcept
    {
        if (!set.MarkWritten(e, now)) ASTRA_UNLIKELY
            return false;
        storage.StampCoarse<Par>(id, now);
        return true;
    }

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
    // Never creates a store (G2): a missing store means "not a member".
    template<typename T>
    ASTRA_NODISCARD bool HasInStorage(ArchetypeManager& am, Entity e, StorageKind kind)
    {
        using Bare = std::remove_const_t<T>;
        const ComponentStorage* storage = am.GetComponentStorage();
        const ComponentID id = TypeID<Bare>::Value();
        switch (kind)
        {
            case StorageKind::Universal:
            {
                const UniversalColumn* col = storage ? storage->FindUniversal(id) : nullptr;
                return col && col->IsLive(e);
            }
            case StorageKind::Sparse:
            {
                const SparseSet* set = storage ? storage->FindSparse(id) : nullptr;
                return set && set->Contains(e);
            }
            case StorageKind::Table:
                break;
        }
        return am.HasComponent<Bare>(e);
    }

    // Typed fetch through the manager, any kind (Relations yields). A non-const T is a
    // WRITE, as on the Table arm (GetComponentMut): Universal through MarkWritten, Sparse
    // through StampSparseWrite. Callable from parallel workers: it never creates a store,
    // and the shared stamp locations (coarse version, Universal page version) go through
    // the relaxed atomic_ref store (RULINGS Task 28; all workers write the same value) when
    // `Par` is set; the serial traversals take the plain stamps.
    // A present tag yields the shared per-type instance.
    template<typename T, bool Par = false>
    ASTRA_NODISCARD std::remove_const_t<T>* GetInStorage(ArchetypeManager& am, Entity e, StorageKind kind)
    {
        using Bare = std::remove_const_t<T>;
        constexpr bool Write = IsMutableYield<T>;
        ComponentStorage* storage = am.GetComponentStorage();
        const ComponentID id = TypeID<Bare>::Value();
        if (kind == StorageKind::Universal)
        {
            UniversalColumn* col = storage ? storage->FindUniversal(id) : nullptr;
            if (!col || !col->IsLive(e))
                return nullptr;
            if constexpr (std::is_empty_v<Bare>)
                return StoreTagInstance<Bare>();
            else
            {
                if constexpr (Write)
                    col->template MarkWritten<Par>(e, am.CurrentTick());
                return static_cast<Bare*>(col->Get(e));
            }
        }
        if (kind == StorageKind::Sparse)
        {
            SparseSet* set = storage ? storage->FindSparse(id) : nullptr;
            if (!set || !set->Contains(e))
                return nullptr;
            if constexpr (std::is_empty_v<Bare>)
                return StoreTagInstance<Bare>();
            else
            {
                if constexpr (Write)
                    (void)StampSparseWrite<Par>(*storage, *set, id, e, am.CurrentTick());
                return static_cast<Bare*>(set->Get(e));
            }
        }
        if constexpr (Write)
            return am.template GetComponentMut<Bare, Par>(e);
        else
            return am.template GetComponent<Bare>(e);
    }
}
