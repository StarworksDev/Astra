#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
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
            {
                // Mirror of Archetype::SerializeColumn's precondition: a hookless descriptor
                // would write nothing per element and desync the reader.
                ASTRA_ASSERT(c.desc->serializeVersioned, "WriteStoredPayload: component descriptor has no serialize hook");
                for (void* value : c.values) c.desc->serializeVersioned(w, value);
            }
            if (flags & StorageFormat::FLAG_HAS_DISABLED)
                StorageFormat::WriteDisabledBitmap(w, c.disabled);
        }

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
                if (SparseSet* set = storage.FindSparse(id)) set->SetDisabled(e, true);
                break;
            case StorageKind::Universal:
                if (UniversalColumn* column = storage.FindUniversal(id)) column->SetDisabled(e, true);
                break;
            default:
                break;
            }
        }
    }

    // Format v6 ComponentStorage block (spec s8.2; layout in BinaryArchive.hpp's
    // v6 note). Deterministic: blocks sorted by hash, elements by entity id.
    // Reads stores through FindSparse/FindUniversal only, so a save never creates a
    // store (RULINGS Task 31).
    inline void ComponentStorage::Serialize(BinaryWriter& w) const
    {
        std::vector<Detail::StoredComponent> stored;

        // Selects by the stores that EXIST, never by a live KindOf(id) descriptor read: a held
        // set whose descriptor now says Table (a module swap) must still be saved, not
        // silently dropped (RULINGS Task 32). Each store carries its own descriptor copy.
        for (size_t raw = 0; raw < MAX_COMPONENTS; ++raw)
        {
            const auto id = static_cast<ComponentID>(raw);
            SparseSet* set = FindSparse(id);
            UniversalColumn* column = set ? nullptr : FindUniversal(id);
            if (set)
            {
                if (set->Size() == 0) continue;

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
            else if (column)
            {
                if (column->LiveCount() == 0) continue;

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
