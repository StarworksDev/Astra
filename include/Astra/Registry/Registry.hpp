#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Commands/CommandBlockArena.hpp"
#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Component/ResourceStorage.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/Signal.hpp"
#include "../Core/TypeID.hpp"
#include "../Core/WorkScheduler.hpp"
#include "../Entity/Entity.hpp"
#include "../Entity/EntityManager.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/SerializationError.hpp"
#include "ComponentStorage.hpp"
#include "ComponentStorageSerialization.hpp"
#include <optional>
#include "Query.hpp"
#include "Relations.hpp"
#include "RelationshipGraph.hpp"
#include "StorageAccess.hpp"
#include "StorageSplit.hpp"
#include "View.hpp"

namespace Astra
{
    class CommandBuffer;   // friend: its create executors use the private creation funnel

    class Registry
    {
    public:
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
        {
            m_componentStorage->SetTickSource(m_archetypeManager->TickSource());
            m_archetypeManager->SetComponentStorage(m_componentStorage.get());
        }

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
        {
            m_componentStorage->SetTickSource(m_archetypeManager->TickSource());
            m_archetypeManager->SetComponentStorage(m_componentStorage.get());
        }

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
        {
            m_componentStorage->SetTickSource(m_archetypeManager->TickSource());
            m_archetypeManager->SetComponentStorage(m_componentStorage.get());
        }

        // A Registry is a heavy, stateful container: copying is not supported (the old
        // (const Registry&, Config) ctor looked like a copy but silently produced an empty
        // registry). To create a second world that shares this one's component-type
        // registrations, use ShareComponentRegistry() with the shared_ptr ctor.
        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;

        ~Registry() = default;
        
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
            ComponentID supplied[sizeof...(Components)];
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
            ConstructUniversalsForBatch(made);
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
                ConstructUniversalsForBatch(made);
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

        void DestroyEntity(Entity entity)
        {
            // Single validated fetch: version + archetype checks subsume the old
            // IsValid pre-check (same unified record slot -- W1). GetEntityRecord
            // hands back const for its read-only call sites; the record is
            // genuinely mutable memory here, so bridge constness once at this
            // consumer (same const_cast idiom used elsewhere in this file).
            auto* rec = const_cast<EntityRecord*>(m_archetypeManager->GetEntityRecord(entity));
            if (!rec) ASTRA_UNLIKELY
                return;

            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed)) ASTRA_UNLIKELY
            {
                m_signalManager.Emit<Events::EntityDestroyed>(entity);
            }

            // Spec s7.3: storage teardown after EntityDestroyed (handlers can still read
            // every component) and before the archetype swap-remove. Per-component
            // ComponentRemoved is NOT emitted on destroy (today's behaviour).
            if (m_componentStorage->HasAnyStore()) ASTRA_UNLIKELY
            {
                ASTRA_ASSERT(rec->chunk, "DestroyEntity: located record without a chunk");
                if (rec->chunk
                    && m_componentStorage->OnEntityDestroyed(entity, *rec->chunk, rec->location.GetEntityIndex()))
                    m_archetypeManager->BumpSparseStructuralCounter();   // a sparse holder's membership changed
            }

            m_archetypeManager->RemoveEntity(entity, rec);

            // Empty-graph fast path: a handful of size loads replace several hash probes.
            // A relation-holding entity (including one with only a cached traversal
            // entry) makes the graph non-empty by construction, so cleanup can never
            // be skipped for an entity that needs it.
            if (!m_relationshipGraph->Empty()) ASTRA_UNLIKELY
            {
                m_relationshipGraph->OnEntityDestroyed(entity);
            }

            m_entityManager.Destroy(entity, rec);
        }
        
        void DestroyEntities(std::span<Entity> entities)
        {
            if (entities.empty())
                return;
            
            std::vector<Entity> validEntities;
            
            size_t reserveSize = std::min(entities.size(), size_t(10000));
            validEntities.reserve(reserveSize);
            
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }

            // IM-1: a duplicated live entity in the span would be removed twice by the
            // archetype swap-with-last path. The first removal swaps the former tail into
            // the freed slot; the second removal of the same (still < count) slot index
            // evicts that swapped-in bystander -- an entity never in the destroy list --
            // and double-frees the intended one. De-duplicate before dispatching to the
            // archetype/graph/entity layers (also collapses duplicate signals below).
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());

            if (validEntities.empty())
                return;

            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed))
            {
                for (Entity entity : validEntities)
                {
                    m_signalManager.Emit<Events::EntityDestroyed>(entity);
                }
            }
                
            // Walk every doomed entity BEFORE any swap-remove; locations are stable until
            // RemoveEntities. The counter bumps only if a doomed entity HELD a sparse member.
            if (m_componentStorage->HasAnyStore()) ASTRA_UNLIKELY
            {
                bool sparseMembershipChanged = false;
                for (Entity entity : validEntities)
                {
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    if (rec && rec->chunk)
                        sparseMembershipChanged |= m_componentStorage->OnEntityDestroyed(entity, *rec->chunk, rec->location.GetEntityIndex());
                }
                if (sparseMembershipChanged)
                    m_archetypeManager->BumpSparseStructuralCounter();
            }

            m_archetypeManager->RemoveEntities(validEntities);
            
            for (Entity entity : validEntities)
            {
                m_relationshipGraph->OnEntityDestroyed(entity);
            }
            
            for (Entity entity : validEntities)
            {
                m_entityManager.Destroy(entity);
            }
        }

        ASTRA_NODISCARD bool IsValid(Entity entity) const noexcept
        {
            return m_entityManager.IsValid(entity);
        }

        // IM-24: a stale/destroyed handle makes ArchetypeManager return nullptr, which a
        // void return would swallow silently -- an undetectable write to a dead handle
        // (data loss). Report it via a bool (matching RemoveComponent/SetEnabled) rather
        // than a silent no-op OR a Debug abort: the project's misuse policy is a graceful,
        // testable result, never assert-and-abort. Returns true when the component is
        // present on the live entity afterward (added, or already present, or a tag);
        // false only for a stale/invalid handle. The bool return is not [[nodiscard]], so
        // existing statement-style callers are unaffected.
        // Debug tripwire for the cross-module aliasing class (an unpinned module
        // minting TypeIDs from its module-local default context and indexing THIS
        // registry with them -- e.g. a host exe that skipped SetTypeContext and
        // read Transform bytes through TypeID<Camera>). Hot accessors carry this
        // in Debug only; registration paths refuse all-config (ComponentRegistry
        // birth-context affinity).
        void AssertContextAffinity() const noexcept
        {
            ASTRA_ASSERT(GetTypeContext() == GetComponentRegistry()->GetBirthContext(),
                         "Registry touched from a module whose TypeContext is not this registry's "
                         "birth context -- call Astra::SetTypeContext in the calling module first");
        }

        template<Component T>
        bool AddComponent(Entity entity, const T& component)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity))
                return false;
            if (IsUniversalId(TypeID<T>::Value())) ASTRA_UNLIKELY
                return true;   // always present: today's present-component rule, no signal (s3.3, s12.2)

            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    if (!rec || !rec->chunk) ASTRA_UNLIKELY
                        return false;
                    const ComponentID id = TypeID<T>::Value();
                    if (const SparseSet* set = m_componentStorage->FindSparse(id); set && set->Contains(entity))
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

            T* newComponent = m_archetypeManager->AddComponent<T>(entity, component);

            if (newComponent)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), newComponent);
            }
            return true;
        }

        template<Component T, typename... Args>
        bool EmplaceComponent(Entity entity, Args&&... args)
        {
            AssertContextAffinity();
            // See AddComponent (IM-24): stale/invalid handle -> graceful false, no abort.
            if (!m_entityManager.IsValid(entity))
                return false;
            if (IsUniversalId(TypeID<T>::Value())) ASTRA_UNLIKELY
                return true;   // always present: see AddComponent

            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    if (!rec || !rec->chunk) ASTRA_UNLIKELY
                        return false;
                    const ComponentID id = TypeID<T>::Value();
                    if (const SparseSet* set = m_componentStorage->FindSparse(id); set && set->Contains(entity))
                        return true;                       // present: no-op, no signal (Table parity, s3.3)
                    void* element = nullptr;
                    if constexpr (std::is_empty_v<T>)
                    {
                        element = SparseAttach(entity, *rec, id, nullptr);
                    }
                    else
                    {
                        T value(std::forward<Args>(args)...);
                        element = SparseAttach(entity, *rec, id, &value);
                    }
                    if (!element) ASTRA_UNLIKELY
                        return false;                      // sparse-slot cap refused it
                    m_signalManager.Emit<Events::ComponentAdded>(entity, id, SparseTypedPtr<T>(element));
                    return true;
                }
            }

            T* component = m_archetypeManager->AddComponent<T>(entity, std::forward<Args>(args)...);

            if (component)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
            }
            return true;
        }

        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            AssertContextAffinity();
            if (IsUniversalId(TypeID<T>::Value())) ASTRA_UNLIKELY
                return m_entityManager.IsValid(entity) ? RefuseUniversalRemove() : false;
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    const ComponentID id = TypeID<T>::Value();
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
                    SparseSet* set = m_componentStorage->FindSparse(id);   // non-creating (G2)
                    if (!rec || !rec->chunk || !set || !set->Contains(entity))
                        return false;                      // stale handle or absent
                    if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved)) ASTRA_UNLIKELY
                    {
                        // BEFORE the erase, as for Table: the pointer is valid only for
                        // the handler's duration.
                        m_signalManager.Emit<Events::ComponentRemoved>(entity, id, SparseTypedPtr<T>(set->Get(entity)));
                        // A handler may have moved or destroyed the entity: re-validate.
                        rec = m_archetypeManager->GetEntityRecord(entity);
                        if (!rec || !rec->chunk || !set->Contains(entity))
                            return false;
                    }
                    return SparseDetach(entity, *rec, id);
                }
            }
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved)) ASTRA_UNLIKELY
            {
                // Cold arm: the signal needs the PRE-removal pointer. The validated
                // GetComponent doubles as the liveness + presence guard (nullptr for
                // stale handles and absent components alike).
                T* component = m_archetypeManager->GetComponent<T>(entity);
                if (!component)
                    return false;

                // Emit BEFORE removal: the pointer is only valid until the entity
                // migrates. Handlers must not retain it past their invocation.
                m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);

                return m_archetypeManager->RemoveComponent<T>(entity);
            }

            // Hot arm: ArchetypeManager validates once (version + archetype + mask).
            return m_archetypeManager->RemoveComponent<T>(entity);
        }
        
        template<Component T>
        void AddComponents(std::span<Entity> entities, const T& component)
        {
            if (entities.empty())
                return;
            if (IsUniversalId(TypeID<T>::Value())) ASTRA_UNLIKELY
                return;        // present on every entity: a no-op, no signal

            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            // IM-1: de-duplicate before the archetype layer -- a repeated entity would be
            // migrated twice (second move operates on a stale source slot, corrupting an
            // unrelated entity). See DestroyEntities for the full mechanism.
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());
            if (validEntities.empty())
                return;

            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    SparseBatchAdd<T>(std::span<const Entity>(validEntities.data(), validEntities.size()),
                                      [&component] { return T(component); });
                    return;
                }
            }

            m_archetypeManager->AddComponents<T>(validEntities, component);
            
            // Emit signals for all entities if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                ComponentID componentId = TypeID<T>::Value();
                for (Entity entity : validEntities)
                {
                    T* comp = m_archetypeManager->GetComponent<T>(entity);
                    if (comp)
                    {
                        m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, comp);
                    }
                }
            }
        }
        
        template<Component T, typename... Args>
        void EmplaceComponents(std::span<Entity> entities, Args&&... args)
        {
            if (entities.empty())
                return;
            if (IsUniversalId(TypeID<T>::Value())) ASTRA_UNLIKELY
                return;        // present on every entity: a no-op, no signal
            
            // Filter out invalid entities
            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());

            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }

            // IM-1: de-duplicate before the archetype layer (see DestroyEntities/AddComponents).
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());

            if (validEntities.empty())
                return;

            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                {
                    // One value from `args`, copied to each entity (as the Table batch does);
                    // so, like it, this needs a copyable T.
                    const T prototype(std::forward<Args>(args)...);
                    SparseBatchAdd<T>(std::span<const Entity>(validEntities.data(), validEntities.size()),
                                      [&prototype] { return T(prototype); });
                    return;
                }
            }

            // Batch add components
            m_archetypeManager->AddComponents<T>(validEntities, std::forward<Args>(args)...);
            
            // Emit signals if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (Entity entity : validEntities)
                {
                    T* component = m_archetypeManager->GetComponent<T>(entity);
                    if (component)
                    {
                        m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
                    }
                }
            }
        }
        
        template<Component T>
        size_t RemoveComponents(std::span<Entity> entities)
        {
            if (entities.empty())
                return 0;
            if (IsUniversalId(TypeID<T>::Value())) ASTRA_UNLIKELY
                return RefuseUniversalRemoveBatch(entities);
            if constexpr (StorageOfV<T> == Storage::Sparse)
            {
                if (ResolveKind<T>() == StorageKind::Sparse)
                    return SparseBatchRemove<T>(entities);
            }

            // Filter out invalid entities
            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());

            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        T* component = m_archetypeManager->GetComponent<T>(entity);
                        if (component)
                        {
                            // Emit before removal — see RemoveComponent.
                            m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);
                            validEntities.push_back(entity);
                        }
                    }
                }
            }
            else
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        validEntities.push_back(entity);
                    }
                }
            }

            // IM-1: de-duplicate before the archetype layer -- a repeated entity would be
            // migrated twice, the second move corrupting the bystander swapped into its
            // freed slot. See DestroyEntities for the full mechanism. (Note: in the
            // signal-enabled branch above, a duplicated entity still emits ComponentRemoved
            // once per occurrence -- a cosmetic double-signal, not the corruption IM-1
            // targets; see the follow-up flag in the report.)
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());

            if (validEntities.empty())
                return 0;

            // Batch remove components
            return m_archetypeManager->RemoveComponents<T>(validEntities);
        }

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

        // ---- Change detection, explicit write side (spec §3.3) ----
        // For code that holds a raw T* across frames: declare the write. Stamps T's
        // chunk column (and marks the entity for a change-tracked T -- Task 7).
        template<Component T>
        bool Modified(Entity entity)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity)) return false;
            return MarkWrittenRouted(entity, TypeID<T>::Value());
        }

        // Type-erased twin of Modified<T>: for hash/descriptor-driven writers (an
        // editor Inspector fanning out over a ComponentDescriptor, an undo Restore
        // through descriptor->deserialize) that hold a ComponentID but no T. Same
        // table as MarkWritten: false for an invalid handle, an absent component,
        // or a tag (no column); true after stamping the column and, for a tracked
        // column, marking the entity.
        bool Modified(Entity entity, ComponentID id)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity)) return false;
            return MarkWrittenRouted(entity, id);
        }

        // Per-entity change query OUTSIDE a view (spec 2026-09-11 Arcane adoption
        // s4). Tracked T: exact -- IsNewer(ticks[row].changed, since). Untracked
        // T: chunk-coarse -- IsNewer(column version, since), so every entity of a
        // stamped chunk answers true (documented, the same trade Changed<T> makes).
        // NEVER stamps: a read that must not count as a write. False for an
        // invalid handle, an absent component, or a tag.
        template<Component T>
        ASTRA_NODISCARD bool IsChanged(Entity entity, Tick since) const
        {
            AssertContextAffinity();
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
                return StoreTickNewer(entity, TypeID<T>::Value(), since, /*added=*/false);
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) return false;
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) return false;
            const int col = rec->archetype->GetColumnMeta().idToColumn[id];
            if (col < 0 || rec->archetype->GetColumnMeta().columns[col].stride == 0) return false;   // absent, or a tag (no value)
            if (const EntityTicks* t = rec->chunk->GetTicks(col))
                return IsNewer(t[rec->location.GetEntityIndex()].changed, since);
            return IsNewer(rec->chunk->GetColumnVersion(col), since);
        }

        // IsChanged's twin over the `added` tick. For an untracked T this is
        // IsChanged in effect (only the column version exists) -- the README's
        // Added<T>-on-untracked caveat, restated for the entity form.
        template<Component T>
        ASTRA_NODISCARD bool IsAdded(Entity entity, Tick since) const
        {
            AssertContextAffinity();
            if (RoutesToStore<T>()) ASTRA_UNLIKELY
                return StoreTickNewer(entity, TypeID<T>::Value(), since, /*added=*/true);
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) return false;
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) return false;
            const int col = rec->archetype->GetColumnMeta().idToColumn[id];
            if (col < 0 || rec->archetype->GetColumnMeta().columns[col].stride == 0) return false;   // absent, or a tag (no value)
            if (const EntityTicks* t = rec->chunk->GetTicks(col))
                return IsNewer(t[rec->location.GetEntityIndex()].added, since);
            return IsNewer(rec->chunk->GetColumnVersion(col), since);
        }

        // Compare-then-store opt-in: assigns and stamps ONLY when `value != current`.
        // Returns true iff a store happened. Deliberately not the default mark path
        // (the compare costs more than the store it skips -- spec §2.4).
        template<Component T>
        requires std::equality_comparable<T>
        bool SetIfNeq(Entity entity, const T& value)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity)) return false;
            T* current = RoutesToStore<T>() ? StoreGet<T>(entity)                       // non-stamping fetch,
                                            : m_archetypeManager->GetComponent<T>(entity);   // either storage
            if (!current) return false;
            if (*current == value) return false;
            *current = value;
            (void)MarkWrittenRouted(entity, TypeID<T>::Value());
            return true;
        }

        // ===================== Enableable components (Task 2) =====================
        // O(1) enable/disable of a component for one entity. SET bit == DISABLED;
        // components are born enabled. Has/GetComponent IGNORE these bits (existence
        // never lies) -- only IsEnabled / queries / Size observe them.
        //
        // Scheduling contract (spec §9): SetEnabled counts as WRITE access on T
        // (owner-thread immediate mutation, identical threading rules to other
        // immediate mutations such as AddComponent); IsEnabled and query-time
        // filtering count as READ access on T.

        // Sets whether component T is enabled on `entity`. Returns true iff `entity`
        // currently holds the enableable component T (regardless of whether the state
        // changed); false for a stale handle or a missing component. Fires the gated
        // ComponentEnabled/ComponentDisabled signal ONLY on a genuine state change --
        // an idempotent SetEnabled is silent.
        template<Component T>
        bool SetEnabled(Entity entity, bool enable)
        {
            static_assert(IsEnableableV<T>,
                "SetEnabled<T> requires an enableable component (opt in with `static constexpr bool AstraEnableable = true;`)");
            return SetEnabledByID(entity, TypeID<T>::Value(), enable);
        }

        // Returns true iff enableable component T is present on `entity` AND currently
        // enabled; false for stale / missing / disabled. Mirrors SetEnabled's table.
        template<Component T>
        ASTRA_NODISCARD bool IsEnabled(Entity entity) const
        {
            static_assert(IsEnableableV<T>,
                "IsEnabled<T> requires an enableable component (opt in with `static constexpr bool AstraEnableable = true;`)");
            return IsEnabledByID(entity, TypeID<T>::Value());
        }

        // Type-erased toggle -- the shared body the typed SetEnabled delegates to.
        bool SetEnabledByID(Entity entity, ComponentID componentId, bool enable)
        {
            if (componentId >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return false;
            // One validated record fetch (version + archetype); stale handle => null.
            auto* rec = const_cast<EntityRecord*>(m_archetypeManager->GetEntityRecord(entity));
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return false;
            // A registry with no store never reads a descriptor kind (one bool check).
            if (m_componentStorage->HasAnyStore())
            {
                if (const StorageKind kind = EnableKindOf(componentId); kind != StorageKind::Table) ASTRA_UNLIKELY
                    return SetEnabledInStore(entity, *rec, componentId, kind, enable);
            }
            if (!rec->archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;   // component absent
            const int col = rec->archetype->GetColumnMeta().idToColumn[componentId];
            if (col < 0) ASTRA_UNLIKELY
                return false;   // present tag: no storage column to carry a bit
            ArchetypeChunk* chunk = rec->chunk;
            if (!chunk->GetDisabledWords(col)) ASTRA_UNLIKELY
                return false;   // present but not enableable: nothing to toggle
            const size_t idx = rec->location.GetEntityIndex();
            const bool changed = chunk->SetDisabled(col, idx, !enable);   // SET == DISABLED
            if (changed)
            {
                // Fire only on genuine change, via the IsSignalEnabled-then-Emit idiom
                // (copy of DestroyEntity at Registry.hpp:233). The component pointer is
                // computed only when the relevant signal is actually live.
                const Signal flag = enable ? Signal::ComponentEnabled : Signal::ComponentDisabled;
                if (m_signalManager.IsSignalEnabled(flag)) ASTRA_UNLIKELY
                {
                    // A present tag's payload is EmptyComponentSentinel() on every by-ID path
                    // (Component.hpp); an enableable tag's bit-only column base is never handed out.
                    void* comp = rec->archetype->GetColumnMeta().columns[col].stride == 0
                        ? EmptyComponentSentinel()
                        : chunk->GetComponentPointer(componentId, idx);
                    if (enable) m_signalManager.Emit<Events::ComponentEnabled>(entity, componentId, comp);
                    else        m_signalManager.Emit<Events::ComponentDisabled>(entity, componentId, comp);
                }
            }
            return true;
        }

        // Type-erased query -- the shared body the typed IsEnabled delegates to.
        ASTRA_NODISCARD bool IsEnabledByID(Entity entity, ComponentID componentId) const
        {
            if (componentId >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return false;
            const auto* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return false;
            if (m_componentStorage->HasAnyStore())
            {
                if (const StorageKind kind = EnableKindOf(componentId); kind != StorageKind::Table) ASTRA_UNLIKELY
                {
                    if (kind == StorageKind::Sparse)
                    {
                        const SparseSet* set = m_componentStorage->FindSparse(componentId);   // a read never creates a store
                        return set && set->Contains(entity) && !set->IsDisabled(entity);
                    }
                    const UniversalColumn* column = m_componentStorage->FindUniversal(componentId);
                    return column && column->IsLive(entity) && !column->IsDisabled(entity);   // non-enableable: IsDisabled is false
                }
            }
            if (!rec->archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;   // absent => not enabled (behavior table)
            const int col = rec->archetype->GetColumnMeta().idToColumn[componentId];
            if (col < 0) ASTRA_UNLIKELY
                return true;    // present tag (no storage): always enabled
            if (!rec->chunk->GetDisabledWords(col)) ASTRA_UNLIKELY
                return true;    // present non-enableable column: always enabled
            return !rec->chunk->IsDisabled(col, rec->location.GetEntityIndex());
        }

        /**
         * Type-erased component addition for use by CommandBuffer.
         * Adds a component to an entity using the component ID and raw data pointer.
         * The component must already be registered in the ComponentRegistry.
         *
         * @param entity The entity to add the component to
         * @param componentId The ComponentID of the component to add
         * @param data Pointer to the source component data (will be copy-constructed)
         * @param dataSize Size of the component data (for validation)
         * @return true if component was added successfully, false otherwise
         */
        bool AddComponentByID(Entity entity, ComponentID componentId, const void* data, size_t dataSize)
        {
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return false;  // present on every entity: the by-id Table rule for a present component
            if (IsSparseId(componentId)) ASTRA_UNLIKELY
                return SparseAddByID(entity, componentId, data, dataSize, /*emit*/ true);

            bool result = m_archetypeManager->AddComponentByID(entity, componentId, data, dataSize);

            if (result && m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                // Get the newly added component pointer for the signal
                auto* record = m_archetypeManager->GetEntityRecord(entity);
                if (record)
                {
                    // We need to get the component descriptor to find the component
                    const auto* desc = m_componentRegistry->GetComponentDescriptor(componentId);
                    if (desc)
                    {
                        if (record->chunk)
                        {
                            void* actualPtr = nullptr;
                            if (desc->size == 0)
                            {
                                actualPtr = EmptyComponentSentinel();  // present tag: no data
                            }
                            else
                            {
                                void* compPtr = record->chunk->GetComponentArrayByID(componentId);
                                actualPtr = compPtr ? static_cast<std::byte*>(compPtr) + record->location.GetEntityIndex() * desc->size : nullptr;
                            }
                            if (actualPtr)
                                m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, actualPtr);
                        }
                    }
                }
            }

            return result;
        }

        /**
         * Type-erased batch component addition for use by CommandBuffer.
         * Note: This function does NOT emit ComponentAdded signals for performance.
         * Use individual AddComponentByID calls if signals are required.
         *
         * @param entities Span of entities to add the component to
         * @param componentId The ComponentID of the component to add
         * @param data Pointer to the source component data (will be copy-constructed to each entity)
         * @param dataSize Size of the component data (for validation)
         * @return Number of entities that successfully had the component added
         */
        size_t AddComponentsByID(std::span<Entity> entities, ComponentID componentId, const void* data, size_t dataSize)
        {
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return 0;
            if (IsSparseId(componentId)) ASTRA_UNLIKELY
            {
                size_t added = 0;
                for (Entity e : entities)
                    added += SparseAddByID(e, componentId, data, dataSize, /*emit*/ false) ? 1u : 0u;
                return added;
            }

            return m_archetypeManager->AddComponentsByID(entities, componentId, data, dataSize);
        }

        /**
         * Type-erased component removal for use by CommandBuffer.
         *
         * @param entity The entity to remove the component from
         * @param componentId The ComponentID of the component to remove
         * @return true if component was removed successfully, false otherwise
         */
        bool RemoveComponentByID(Entity entity, ComponentID componentId)
        {
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return m_entityManager.IsValid(entity) ? RefuseUniversalRemove() : false;
            if (IsSparseId(componentId)) ASTRA_UNLIKELY
                return SparseRemoveByID(entity, componentId, /*emit*/ true);

            // Get component pointer before removal for signal emission
            void* componentPtr = nullptr;
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                auto* record = m_archetypeManager->GetEntityRecord(entity);
                if (record)
                {
                    const auto* desc = m_componentRegistry->GetComponentDescriptor(componentId);
                    if (desc)
                    {
                        if (record->chunk)
                        {
                            if (desc->size == 0)
                            {
                                componentPtr = EmptyComponentSentinel();  // present tag: no data
                            }
                            else
                            {
                                void* compArray = record->chunk->GetComponentArrayByID(componentId);
                                if (compArray)
                                    componentPtr = static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
                            }
                        }
                    }
                }
            }

            if (componentPtr)
            {
                // Emit BEFORE removal so the pointer is still valid.
                m_signalManager.Emit<Events::ComponentRemoved>(entity, componentId, componentPtr);
            }

            return m_archetypeManager->RemoveComponentByID(entity, componentId);
        }

        /**
         * Type-erased batch component removal for use by CommandBuffer.
         * Note: This function does NOT emit ComponentRemoved signals for performance.
         * Use individual RemoveComponentByID calls if signals are required.
         *
         * @param entities Span of entities to remove the component from
         * @param componentId The ComponentID of the component to remove
         * @return Number of entities that successfully had the component removed
         */
        size_t RemoveComponentsByID(std::span<Entity> entities, ComponentID componentId)
        {
            if (IsUniversalId(componentId)) ASTRA_UNLIKELY
                return RefuseUniversalRemoveBatch(entities);
            if (IsSparseId(componentId)) ASTRA_UNLIKELY
            {
                size_t removed = 0;
                for (Entity e : entities)
                    removed += SparseRemoveByID(e, componentId, /*emit*/ false) ? 1u : 0u;
                return removed;
            }

            return m_archetypeManager->RemoveComponentsByID(entities, componentId);
        }

        // ====================== Reflection Integration ======================

        /**
         * Gets a component by type hash (for reflection/runtime access).
         * The type hash should come from TypeID<T>::Hash() for a registered component.
         *
         * @param entity The entity to get the component from
         * @param typeHash XXHash64 of the component type name
         * @return Pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetComponentByHash(Entity entity, uint64_t typeHash)
        {
            if (!m_entityManager.IsValid(entity))
                return nullptr;

            // Look up component ID from type hash
            auto result = m_componentRegistry->GetComponentIDFromHash(typeHash);
            if (result.IsErr())
                return nullptr;

            ComponentID componentId = *result.GetValue();
            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return nullptr;

            const auto* desc = m_componentRegistry->GetComponentDescriptor(componentId);
            if (!desc)
                return nullptr;

            // Presence is the archetype mask, not the chunk array (which is null for a
            // size==0 tag). This makes Get(...) != nullptr agree with Has(...).
            if (!record->archetype->GetMask().Test(componentId))
            {
                // Not in the mask: a Sparse/Universal member lives in its store (s3.3).
                bool present = false;
                return StoredElementByID(entity, componentId, present);
            }

            if (desc->size == 0)
                return EmptyComponentSentinel();     // present tag: no data

            if (!record->chunk)
                return nullptr;

            void* compArray = record->chunk->GetComponentArrayByID(componentId);
            if (!compArray)
                return nullptr;                      // defensive (should not happen for size > 0)

            return static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
        }

        /**
         * Gets a component by type hash (const version).
         *
         * @param entity The entity to get the component from
         * @param typeHash XXHash64 of the component type name
         * @return Const pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetComponentByHash(Entity entity, uint64_t typeHash) const
        {
            return const_cast<Registry*>(this)->GetComponentByHash(entity, typeHash);
        }

        /**
         * Checks if an entity has a component by type hash.
         *
         * @param entity The entity to check
         * @param typeHash XXHash64 of the component type name
         * @return true if the entity has the component
         */
        ASTRA_NODISCARD bool HasComponentByHash(Entity entity, uint64_t typeHash) const
        {
            if (!m_entityManager.IsValid(entity))
                return false;

            auto result = m_componentRegistry->GetComponentIDFromHash(typeHash);
            if (result.IsErr())
                return false;

            ComponentID componentId = *result.GetValue();
            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return false;

            if (record->archetype->GetMask().Test(componentId))
                return true;
            bool present = false;
            (void)StoredElementByID(entity, componentId, present);
            return present;
        }

        /**
         * Gets all component descriptors for an entity.
         * Useful for editor/inspector UI that needs to enumerate all components.
         *
         * @param entity The entity to query
         * @return Vector of ComponentDescriptor pointers for all components on the entity
         */
        ASTRA_NODISCARD std::vector<const ComponentDescriptor*> GetEntityComponents(Entity entity) const
        {
            std::vector<const ComponentDescriptor*> result;

            if (!m_entityManager.IsValid(entity))
                return result;

            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return result;

            const ComponentMask& mask = record->archetype->GetMask();

            // Iterate through all registered components and check if entity has them
            m_componentRegistry->ForEachComponent([&](ComponentID id, const ComponentDescriptor& desc)
            {
                if (mask.Test(id))
                {
                    result.push_back(&desc);
                    return;
                }
                bool present = false;
                (void)StoredElementByID(entity, id, present);
                if (present)
                    result.push_back(&desc);
            });

            return result;
        }

        /**
         * Information about a component on an entity for inspection.
         */
        struct ComponentInfo
        {
            const ComponentDescriptor* descriptor;  // Component type info and lifecycle ops
            const TypeMeta* meta;                   // Reflection metadata (may be nullptr if not reflected)
            void* data;                             // Pointer to the component data
        };

        /**
         * Inspects an entity and returns detailed information about all its components.
         * This is the primary method for editor/inspector UIs to enumerate and modify components.
         *
         * @param entity The entity to inspect
         * @return Vector of ComponentInfo for all components on the entity
         */
        ASTRA_NODISCARD std::vector<ComponentInfo> InspectEntity(Entity entity)
        {
            std::vector<ComponentInfo> result;

            if (!m_entityManager.IsValid(entity))
                return result;

            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return result;

            const ComponentMask& mask = record->archetype->GetMask();

            if (!record->chunk)
                return result;

            // Iterate through all registered components and collect info
            m_componentRegistry->ForEachComponent([&](ComponentID id, const ComponentDescriptor& desc)
            {
                if (!mask.Test(id))
                {
                    // A Sparse/Universal member lives in its store (s3.3).
                    bool present = false;
                    void* stored = StoredElementByID(entity, id, present);
                    if (!present)
                        return;
                    result.push_back(ComponentInfo{ &desc, desc.meta, desc.size > 0 ? stored : nullptr });   // a tag has no data
                    return;
                }

                ComponentInfo info;
                info.descriptor = &desc;
                info.meta = desc.meta;  // May be nullptr if type is not reflected

                // Get component data pointer
                void* compArray = record->chunk->GetComponentArrayByID(id);
                if (compArray && desc.size > 0)
                {
                    info.data = static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc.size;
                }
                else
                {
                    info.data = nullptr;  // Empty component
                }

                result.push_back(info);
            });

            return result;
        }

        /**
         * Inspects an entity (const version - returns const data pointers).
         * @param entity The entity to inspect
         * @return Vector of ComponentInfo for all components (data pointers are const)
         */
        ASTRA_NODISCARD std::vector<ComponentInfo> InspectEntity(Entity entity) const
        {
            return const_cast<Registry*>(this)->InspectEntity(entity);
        }

        /**
         * Information about a resource for inspection.
         */
        struct ResourceInfo
        {
            const ComponentDescriptor* descriptor;  // Resource type info and lifecycle ops
            const TypeMeta* meta;                   // Reflection metadata (may be nullptr if not reflected)
            void* data;                             // Pointer to the resource data
        };

        /**
         * Inspects all resources and returns detailed information.
         * This is the primary method for editor/inspector UIs to enumerate and modify resources.
         *
         * @return Vector of ResourceInfo for all active resources
         */
        ASTRA_NODISCARD std::vector<ResourceInfo> InspectResources()
        {
            std::vector<ResourceInfo> result;

            auto descriptors = m_resourceStorage.GetAllResources();
            result.reserve(descriptors.size());

            for (const auto* desc : descriptors)
            {
                ResourceInfo info;
                info.descriptor = desc;
                info.meta = desc->meta;  // May be nullptr if type is not reflected
                info.data = m_resourceStorage.GetByID(desc->id);
                result.push_back(info);
            }

            return result;
        }

        /**
         * Inspects all resources (const version - returns const data pointers).
         * @return Vector of ResourceInfo for all active resources
         */
        ASTRA_NODISCARD std::vector<ResourceInfo> InspectResources() const
        {
            return const_cast<Registry*>(this)->InspectResources();
        }

        /**
         * Gets a component by name (for debugging/scripting).
         * This is slower than GetComponentByHash as it requires a name lookup.
         *
         * @param entity The entity to get the component from
         * @param name Component type name (as returned by TypeID<T>::Name())
         * @return Pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetComponentByName(Entity entity, std::string_view name)
        {
            // Compute hash from name
            uint64_t hash = Detail::XXHash::XXHash64(name);
            return GetComponentByHash(entity, hash);
        }

        /**
         * Gets a component by name (const version).
         *
         * @param entity The entity to get the component from
         * @param name Component type name
         * @return Const pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetComponentByName(Entity entity, std::string_view name) const
        {
            return const_cast<Registry*>(this)->GetComponentByName(entity, name);
        }

        // Resource (singleton component) management
        
        template<Component T>
        ASTRA_NODISCARD T* GetResource() noexcept
        {
            AssertContextAffinity();
            return m_resourceStorage.Get<T>();
        }

        template<Component T>
        ASTRA_NODISCARD const T* GetResource() const noexcept
        {
            AssertContextAffinity();
            return m_resourceStorage.Get<T>();
        }

        template<Component T>
        T* SetResource(T&& resource)
        {
            AssertContextAffinity();
            ComponentID id = TypeID<T>::Value();
            bool isNew = !m_resourceStorage.Has<T>();
            
            T* result = m_resourceStorage.Set(std::forward<T>(resource));
            
            if (isNew)
            {
                m_signalManager.Emit<Events::ResourceAdded>(id, result);
            }
            else
            {
                m_signalManager.Emit<Events::ResourceUpdated>(id, result);
            }
            
            return result;
        }
        
        template<Component T, typename... Args>
        T* EmplaceResource(Args&&... args)
        {
            ComponentID id = TypeID<T>::Value();
            bool isNew = !m_resourceStorage.Has<T>();
            
            T* result = m_resourceStorage.Emplace<T>(std::forward<Args>(args)...);
            
            if (isNew)
            {
                m_signalManager.Emit<Events::ResourceAdded>(id, result);
            }
            else
            {
                m_signalManager.Emit<Events::ResourceUpdated>(id, result);
            }
            
            return result;
        }

        template<Component T>
        ASTRA_NODISCARD bool HasResource() const noexcept
        {
            return m_resourceStorage.Has<T>();
        }

        template<Component T>
        void RemoveResource()
        {
            ComponentID id = TypeID<T>::Value();
            T* resource = m_resourceStorage.Get<T>();
            
            if (resource)
            {
                m_signalManager.Emit<Events::ResourceRemoved>(id, resource);
                m_resourceStorage.Remove<T>();
            }
        }
        
        void ClearResources()
        {
            // Note: We don't emit individual ResourceRemoved events here for performance
            // If needed, users can listen to a bulk clear event or iterate resources first
            m_resourceStorage.Clear();
        }

        // ====================== Resource Reflection ======================

        /**
         * Gets a resource by type hash (for reflection/runtime access).
         * @param typeHash XXHash64 of the resource type name
         * @return Pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetResourceByHash(uint64_t typeHash)
        {
            return m_resourceStorage.GetResourceByHash(typeHash);
        }

        /**
         * Gets a resource by type hash (const version).
         * @param typeHash XXHash64 of the resource type name
         * @return Const pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetResourceByHash(uint64_t typeHash) const
        {
            return m_resourceStorage.GetResourceByHash(typeHash);
        }

        /**
         * Checks if a resource exists by type hash.
         * @param typeHash XXHash64 of the resource type name
         * @return true if the resource exists
         */
        ASTRA_NODISCARD bool HasResourceByHash(uint64_t typeHash) const
        {
            return m_resourceStorage.HasResourceByHash(typeHash);
        }

        /**
         * Gets all resource descriptors.
         * Useful for editor/inspector UI that needs to enumerate all resources.
         * @return Vector of ComponentDescriptor pointers for all active resources
         */
        ASTRA_NODISCARD std::vector<const ComponentDescriptor*> GetAllResources() const
        {
            return m_resourceStorage.GetAllResources();
        }

        /**
         * Gets a resource by ComponentID.
         * @param componentId The ComponentID of the resource
         * @return Pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetResourceByID(ComponentID componentId)
        {
            return m_resourceStorage.GetByID(componentId);
        }

        /**
         * Gets a resource by ComponentID (const version).
         * @param componentId The ComponentID of the resource
         * @return Const pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetResourceByID(ComponentID componentId) const
        {
            return m_resourceStorage.GetByID(componentId);
        }

        /**
         * Type-erased resource setting for use by CommandBuffer.
         * Sets a resource using the component ID and raw data pointer.
         * The component must already be registered in the ComponentRegistry.
         *
         * @param componentId The ComponentID of the resource
         * @param data Pointer to the source resource data (will be copy-constructed)
         * @param dataSize Size of the resource data (for validation)
         * @return true if resource was set successfully, false otherwise
         */
        bool SetResourceByID(ComponentID componentId, const void* data, size_t dataSize)
        {
            return m_resourceStorage.SetByID(componentId, data, dataSize);
        }

        /**
         * Type-erased resource removal for use by CommandBuffer.
         *
         * @param componentId The ComponentID of the resource to remove
         * @return true if resource was removed, false if it didn't exist
         */
        bool RemoveResourceByID(ComponentID componentId)
        {
            return m_resourceStorage.RemoveByID(componentId);
        }

        // View creation
        
        template<ValidQueryArg... QueryArgs>
        ASTRA_NODISCARD auto CreateView()
        {
            AssertContextAffinity();
            return View<QueryArgs...>(m_archetypeManager, m_workScheduler);
        }
        
        void Clear()
        {
            const bool hadStores = m_componentStorage->HasAnyStore();
            m_componentStorage->Clear();   // destroy every store element first, in place; slot assignments + universal declarations survive (spec s7.3)
            m_archetypeManager->Clear();   // in place -- keeps cached Views/Relations valid; chunks free their presence blocks
            if (hadStores) ASTRA_UNLIKELY
                m_archetypeManager->BumpSparseStructuralCounter();
            m_relationshipGraph->Clear();
            m_entityManager.Clear();
        }

        ASTRA_NODISCARD std::size_t Size() const noexcept
        {
            return m_entityManager.Size();
        }
        

        ASTRA_NODISCARD bool IsEmpty() const noexcept
        {
            return Size() == 0;
        }

        ASTRA_NODISCARD EntityManager& GetEntityManager() noexcept { return m_entityManager; }
        ASTRA_NODISCARD const EntityManager& GetEntityManager() const noexcept { return m_entityManager; }

        // A per-construction id (Astra "now" batch, 2026-09-30) from a static
        // atomic starting at 1, ParallelCommandBuffer's idiom. Load constructs
        // through the shared_ptr ctor, so a loaded registry gets a FRESH id.
        // Registry is non-copyable and non-movable, so an id is never
        // duplicated. It seeds nothing (not ticks, not StructureVersion).
        // Note: an inline static is per module image on Windows, so the id is
        // unique among registries constructed in one module.
        ASTRA_NODISCARD std::uint64_t GetInstanceId() const noexcept { return m_instanceId; }

        ASTRA_NODISCARD ComponentRegistry* GetComponentRegistry() noexcept { return m_componentRegistry.get(); }
        ASTRA_NODISCARD const ComponentRegistry* GetComponentRegistry() const noexcept { return m_componentRegistry.get(); }
        ASTRA_NODISCARD std::shared_ptr<ComponentRegistry> ShareComponentRegistry() const noexcept { return m_componentRegistry; }
        ASTRA_NODISCARD ArchetypeManager* GetArchetypeManager() noexcept { return m_archetypeManager.get(); }
        ASTRA_NODISCARD const ArchetypeManager* GetArchetypeManager() const noexcept { return m_archetypeManager.get(); }

        // Storage policies (spec 2026-10-01 s4): component kinds, sparse slots, and the
        // sparse / universal stores of THIS registry.
        ASTRA_NODISCARD ComponentStorage* GetComponentStorage() noexcept { return m_componentStorage.get(); }
        ASTRA_NODISCARD const ComponentStorage* GetComponentStorage() const noexcept { return m_componentStorage.get(); }

        // Change-detection time (spec 2026-09-10 §3.1): forwarded from the owning
        // ArchetypeManager so views (which hold the manager) and the registry agree.
        ASTRA_NODISCARD Tick CurrentTick() const noexcept { return m_archetypeManager->CurrentTick(); }
        Tick AdvanceTick() noexcept { return m_archetypeManager->AdvanceTick(); }

        /**
         * Block source for CommandBuffer storage (Commands C1 fix): stable
         * blocks that never move until released. Command buffers bound to
         * this Registry must not outlive it. This is a real destruction-order
         * requirement, not just a don't-use-after-free caveat: because
         * m_commandBlockArena is a Registry member, any CommandBuffer/
         * ParallelCommandBuffer/SystemScheduler declared BEFORE the Registry
         * in the same scope is destroyed AFTER it (reverse declaration order)
         * and will trip CommandBlockArena's outstanding-block assert on exit
         * -- declare Registry first.
         */
        ASTRA_NODISCARD CommandBlockArena& GetCommandBlockArena() noexcept { return m_commandBlockArena; }
        
        ASTRA_NODISCARD float GetFragmentationLevel() const
        {
            auto archetypes = m_archetypeManager->GetArchetypes();
            if (archetypes.empty())
                return 0.0f;

            // Fill-based, matching Archetype::GetFragmentationLevel: chunks no
            // longer share a capacity, so there is no single "optimal chunk
            // count" to compare against. Unused slots are what defragmentation
            // actually reclaims, so measure them directly.
            size_t totalEntities = 0;
            size_t totalCapacity = 0;

            for (const auto* arch : archetypes)
            {
                totalEntities += arch->GetEntityCount();
                totalCapacity += arch->GetTotalCapacity();
            }

            if (totalCapacity == 0) return 0.0f;

            return 1.0f - static_cast<float>(totalEntities) / static_cast<float>(totalCapacity);
        }
        
        struct DefragmentationOptions
        {
            size_t minArchetypesToKeep = 8;           // Never go below this many archetypes
            size_t maxArchetypesToRemove = 10;        // Limit per call (for incremental)
            
            bool defragmentChunks = true;             // Enable chunk coalescing
            float chunkUtilizationThreshold = 0.5f;   // Defragment archetypes when chunks fall below this utilization (0.5 = 50% full)
            size_t maxChunksToProcess = 100;          // Limit chunks processed per call
            
            bool defragmentPool = true;               // Enable memory pool defragmentation
            
            size_t maxEntitiesToMove = 10000;         // Total entity move budget
            bool incremental = false;                 // If true, strictly respect all limits
        };
        
        struct DefragmentationResult
        {
            size_t archetypesRemoved = 0;
            size_t chunksRemoved = 0;
            size_t entitiesMoved = 0;
            size_t archetypesProcessed = 0;
            size_t poolBlocksReleased = 0;
            size_t poolMemoryFreed = 0;
            float fragmentationBefore = 0.0f;
            float fragmentationAfter = 0.0f;
            
            ASTRA_NODISCARD bool DidWork() const noexcept 
            { 
                return archetypesRemoved > 0 || chunksRemoved > 0 || entitiesMoved > 0 || poolBlocksReleased > 0; 
            }
        };
        
        // Overload instead of a default argument: gcc/clang reject a default argument
        // that needs DefragmentationOptions' NSDMIs before the enclosing class is complete.
        DefragmentationResult Defragment() { return Defragment(DefragmentationOptions{}); }

        // Storage policies (spec s5.2): exhaustive check of
        //   P1 presence -- chunk present bit <=> sparse-set membership, both directions;
        //   P2 counts   -- presentCount/disabledCount == popcount of their words;
        //   P3 mirror   -- chunk disabled bit <=> the store's disabled flag
        //                  (enableable Sparse and Universal);
        // plus the tail rule (no bit at a chunk slot >= count). Returns "" when all
        // hold, else one line per violation (first 32). Every config (ledgered deviation
        // from the spec's "Debug-only", G9). Non-creating (G2): never builds a store or
        // takes a slot. O(entities x sparse slots): tests and the invariant fuzzer only.
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
            const ComponentStorage& storage = *m_componentStorage;

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

                        const SparseSet* set = kind == StorageKind::Sparse ? storage.FindSparse(id) : nullptr;
                        const UniversalColumn* col = kind == StorageKind::Universal ? storage.FindUniversal(id) : nullptr;
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
                                const bool member = set && set->Contains(e);
                                if (p != member) fail("P1 presence bit != sparse membership", e, b.slot, i);
                                if (d && !p)     fail("P3 disabled bit on a non-member", e, b.slot, i);
                                if (member && d != set->IsDisabled(e)) fail("P3 disabled mirror != sparse disabled flag", e, b.slot, i);
                            }
                            else if (kind == StorageKind::Universal)
                            {
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
                    const SparseSet* set = storage.FindSparse(id);
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
                    const UniversalColumn* col = storage.FindUniversal(id);
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

            // Universal side, spec-driven: an enableable Universal whose id never received a
            // sparse slot is invisible to both loops above, yet a disabled flag on it still
            // violates P3 (no slot means no mirror bit can exist). Find* only.
            for (const UniversalSpec& spec : storage.UniversalSpecs())
            {
                const UniversalColumn* col = storage.FindUniversal(spec.id);
                if (!col || !col->Descriptor().isEnableable) continue;
                const int slot = storage.SparseSlotOf(spec.id);
                col->ForEachLive([&](Entity e, const void*)
                {
                    if (!col->IsDisabled(e)) return;
                    const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
                    if (slot < 0)
                        fail("P3 universal disabled flag with no sparse slot (no mirror)", e, 0, rec && rec->chunk ? rec->location.GetEntityIndex() : 0);
                    else if (rec && rec->chunk && !rec->chunk->IsSparseDisabled(static_cast<uint16_t>(slot), rec->location.GetEntityIndex()))
                        fail("P3 universal disabled flag without its mirror bit", e, static_cast<uint16_t>(slot), rec->location.GetEntityIndex());
                });
            }

            if (violations > 32)
                out += "... " + std::to_string(violations - 32) + " more violation(s)\n";
            return out;
        }

        DefragmentationResult Defragment(const DefragmentationOptions& options)
        {
            DefragmentationResult result;
            
            // Calculate initial fragmentation
            result.fragmentationBefore = GetFragmentationLevel();
            
            size_t totalEntitiesMoved = 0;
            auto archetypes = m_archetypeManager->GetArchetypes();
            
            // Step 1: Defragment chunks within archetypes
            if (options.defragmentChunks)
            {
                size_t chunksProcessed = 0;
                
                for (auto* arch : archetypes)
                {
                    // Check limits
                    if (options.incremental)
                    {
                        if (chunksProcessed >= options.maxChunksToProcess) break;
                        if (totalEntitiesMoved >= options.maxEntitiesToMove) break;
                    }
                    
                    // Skip if archetype has few chunks or is already well-packed
                    if (arch->GetChunks().size() <= 1) continue;
                    
                    // Only process archetypes with significant fragmentation
                    // Fragmentation level: 0.0 = perfectly packed, 1.0 = all wasted space
                    // Skip if fragmentation is below threshold (e.g., < 50% wasted space)
                    float archFragmentation = arch->GetFragmentationLevel();
                    float fragmentationThreshold = 1.0f - options.chunkUtilizationThreshold;
                    if (archFragmentation < fragmentationThreshold) continue;
                    
                    // Rebuild-style compaction: repacks every live entity into
                    // fresh right-sized chunks and reports every one's new
                    // location (movedEntities covers the whole archetype). The
                    // SetEntityLocation loop below already handles that.
                    auto [chunksFreed, movedEntities] = arch->CompactChunks();
                    
                    // Update entity locations in ArchetypeManager
                    for (const auto& [entity, newLocation] : movedEntities)
                    {
                        m_archetypeManager->SetEntityLocation(entity, arch, newLocation);
                    }
                    
                    result.chunksRemoved += chunksFreed;
                    totalEntitiesMoved += movedEntities.size();
                    chunksProcessed += arch->GetChunks().size();
                    result.archetypesProcessed++;
                    
                    // Break if we've hit our entity move budget
                    if (options.incremental && totalEntitiesMoved >= options.maxEntitiesToMove)
                    {
                        break;
                    }
                }
                
                result.entitiesMoved = totalEntitiesMoved;
            }
            
            // Step 2: Remove empty archetypes
            // Only do this if we haven't exhausted our limits
            if (!options.incremental || totalEntitiesMoved < options.maxEntitiesToMove)
            {
                // Build defragmentation options for archetype manager
                ArchetypeManager::DefragmentOptions archOpts;
                archOpts.minArchetypesToKeep = options.minArchetypesToKeep;
                archOpts.maxArchetypesToRemove = options.maxArchetypesToRemove;
                
                // If we're in incremental mode and close to entity limit, reduce archetype removals
                if (options.incremental && totalEntitiesMoved > options.maxEntitiesToMove * 0.8f)
                {
                    archOpts.maxArchetypesToRemove = std::min(size_t(2), options.maxArchetypesToRemove);
                }
                
                // Defragment archetypes (currently removes empty ones, could be extended)
                auto archResult = m_archetypeManager->Defragment(archOpts);
                result.archetypesRemoved = archResult.emptyArchetypesRemoved;
            }
            
            // Step 3: Defragment memory pool to release unused blocks
            if (options.defragmentPool)
            {
                auto& pool = m_archetypeManager->GetChunkPool();
                auto poolResult = pool.Defragment();
                result.poolBlocksReleased = poolResult.blocksReleased;
                result.poolMemoryFreed = poolResult.bytesFreed;
            }
            
            // Calculate final fragmentation
            result.fragmentationAfter = GetFragmentationLevel();
            
            return result;
        }
        
        ASTRA_NODISCARD size_t GetArchetypeCount() const
        {
            return m_archetypeManager->GetArchetypeCount();
        }
        
        ASTRA_NODISCARD size_t GetArchetypeMemoryUsage() const
        {
            return m_archetypeManager->GetArchetypeMemoryUsage();
        }

        template<Component... Components>
        ASTRA_NODISCARD Archetype* FindArchetype() const
        {
            return m_archetypeManager->FindArchetype<Components...>();
        }
        
        ASTRA_NODISCARD Archetype* FindArchetype(const ComponentMask& mask) const
        {
            return m_archetypeManager->FindArchetype(mask);
        }
        
        ASTRA_NODISCARD auto GetAllArchetypes()
        {
            return m_archetypeManager->GetArchetypes();
        }

        template<typename... QueryArgs>
        ASTRA_NODISCARD Relations<QueryArgs...> GetRelations(Entity entity) const
        {
            return Relations<QueryArgs...>(m_archetypeManager, entity, m_relationshipGraph, m_workScheduler);
        }
        
        // Forwards to RelationshipGraph::SetParent (ordered children, `index`
        // as documented there) and returns its verdict. ParentChanged fires only
        // when the graph accepted AND the parent changed, so a same-parent
        // reorder and every refusal emit nothing. The bool is deliberately not
        // [[nodiscard]]: fire-and-forget callers need no churn.
        bool SetParent(Entity child, Entity parent, std::size_t index = RelationshipGraph::npos)
        {
            if (!m_entityManager.IsValid(child) || !m_entityManager.IsValid(parent))
                return false;

            const Entity previous = m_relationshipGraph->GetParent(child);
            if (!m_relationshipGraph->SetParent(child, parent, index))
                return false;

            if (previous != parent)
                m_signalManager.Emit<Events::ParentChanged>(child, parent);
            return true;
        }

        void RemoveParent(Entity child)
        {
            if (m_entityManager.IsValid(child))
            {
                // Get current parent before removal for signal
                Entity parent = m_relationshipGraph->GetParent(child);
                m_relationshipGraph->RemoveParent(child);
                
                // Emit parent changed signal (parent is now invalid)
                if (parent.IsValid())
                {
                    m_signalManager.Emit<Events::ParentChanged>(child, Entity::Invalid());
                }
            }
        }

        void AddChild(Entity parent, Entity child)
        {
            SetParent(child, parent);
        }

        bool RemoveChild(Entity parent, Entity child)
        {
            if (!m_entityManager.IsValid(parent) || !m_entityManager.IsValid(child))
                return false;
                
            Entity currentParent = m_relationshipGraph->GetParent(child);
            if (currentParent == parent)
            {
                RemoveParent(child);
                return true;
            }
            return false;
        }
        
        size_t RemoveAllChildren(Entity parent)
        {
            if (!m_entityManager.IsValid(parent))
                return 0;
                
            auto children = GetChildren(parent);
            for (Entity child : children)
            {
                RemoveParent(child);
            }
            return children.size();
        }

        ASTRA_NODISCARD Entity GetParent(Entity child) const
        {
            if (!m_entityManager.IsValid(child))
                return Entity::Invalid();
            return m_relationshipGraph->GetParent(child);
        }

        ASTRA_NODISCARD std::vector<Entity> GetChildren(Entity parent) const
        {
            if (!m_entityManager.IsValid(parent))
                return {};
                
            // Direct access to avoid copy overhead
            const auto& children = m_relationshipGraph->GetChildren(parent);
            return std::vector<Entity>(children.begin(), children.end());
        }
        
        template<typename Func>
        void ForEachChild(Entity parent, Func&& func) const
        {
            if (!m_entityManager.IsValid(parent))
                return;
                
            const auto& children = m_relationshipGraph->GetChildren(parent);
            for (Entity child : children)
            {
                func(child);
            }
        }

        ASTRA_NODISCARD size_t GetChildCount(Entity parent) const
        {
            if (!m_entityManager.IsValid(parent))
                return 0;
            return m_relationshipGraph->GetChildCount(parent);
        }
        
        ASTRA_NODISCARD bool HasChildren(Entity parent) const
        {
            if (!m_entityManager.IsValid(parent))
                return false;
            return m_relationshipGraph->HasChildren(parent);
        }
        
        ASTRA_NODISCARD bool HasParent(Entity child) const
        {
            return GetParent(child).IsValid();
        }
        
        ASTRA_NODISCARD bool IsParentOf(Entity parent, Entity child) const
        {
            return GetParent(child) == parent;
        }
        
        ASTRA_NODISCARD bool IsChildOf(Entity child, Entity parent) const
        {
            return GetParent(child) == parent;
        }

        void AddLink(Entity a, Entity b)
        {
            if (m_entityManager.IsValid(a) && m_entityManager.IsValid(b))
            {
                m_relationshipGraph->AddLink(a, b);
                
                // Emit link added signal
                m_signalManager.Emit<Events::LinkAdded>(a, b);
            }
        }
        
        void RemoveLink(Entity a, Entity b)
        {
            if (m_entityManager.IsValid(a) && m_entityManager.IsValid(b))
            {
                m_relationshipGraph->RemoveLink(a, b);
                
                // Emit link removed signal
                m_signalManager.Emit<Events::LinkRemoved>(a, b);
            }
        }

        ASTRA_NODISCARD RelationshipGraph& GetRelationshipGraph() { return *m_relationshipGraph; }
        ASTRA_NODISCARD const RelationshipGraph& GetRelationshipGraph() const { return *m_relationshipGraph; }

        // See RelationshipGraph::StructureVersion() -- graph-wide, not entity
        // scoped, so unlike GetParent/GetChildren/etc. above there's no
        // per-entity validity check to make here.
        ASTRA_NODISCARD std::uint32_t StructureVersion() const noexcept
        {
            return m_relationshipGraph->StructureVersion();
        }
        
        void EnableSignals(Signal signals)
        {
            m_signalManager.EnableSignals(signals);
        }
        
        void DisableSignals(Signal signals)
        {
            m_signalManager.DisableSignals(signals);
        }
        
        void SetEnabledSignals(Signal signals)
        {
            m_signalManager.SetEnabledSignals(signals);
        }

        ASTRA_NODISCARD Signal GetEnabledSignals() const
        {
            return m_signalManager.GetEnabledSignals();
        }
        
        ASTRA_NODISCARD SignalManager* GetSignalManager() noexcept
        {
            return &m_signalManager;
        }
        
        ASTRA_NODISCARD const SignalManager* GetSignalManager() const noexcept
        {
            return &m_signalManager;
        }
        
        // ====================== Serialization API ======================
        
        struct SaveConfig
        {
            CompressionMode compressionMode = CompressionMode::LZ4;
            Compression::CompressionLevel compressionLevel = Compression::CompressionLevel::Fast;
            size_t compressionThreshold = 1024; // Only compress blocks larger than this
        };
        
        // Overload instead of a default argument: gcc/clang reject a default argument
        // that needs SaveConfig's NSDMIs before the enclosing class is complete.
        Result<void, SerializationError> Save(const std::filesystem::path& path) const { return Save(path, SaveConfig{}); }

        Result<void, SerializationError> Save(const std::filesystem::path& path, const SaveConfig& config) const
        {
            BinaryWriter::Config writerConfig;
            writerConfig.compressionMode = config.compressionMode;
            writerConfig.compressionLevel = config.compressionLevel;
            writerConfig.compressionThreshold = config.compressionThreshold;
            
            BinaryWriter writer(path, writerConfig);
            if (writer.HasError())
            {
                return Result<void, SerializationError>::Err(SerializationError::IOError);
            }
            
            // Write header
            BinaryHeader header;
            header.entityCount = static_cast<uint32_t>(m_entityManager.Size());
            header.archetypeCount = static_cast<uint32_t>(m_archetypeManager->GetArchetypeCount());
            
            writer.WriteHeader(header);
            
            // Serialize components
            m_entityManager.Serialize(writer);
            m_archetypeManager->Serialize(writer);
            m_componentStorage->Serialize(writer);   // format v6 ComponentStorage block (s8.1)
            m_relationshipGraph->Serialize(writer);
            m_resourceStorage.Serialize(writer);   // format v2 resource block

            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<void, SerializationError>::Err(writer.GetError()) : 
                Result<void, SerializationError>::Ok();
        }
        
        Result<std::vector<std::byte>, SerializationError> Save() const { return Save(SaveConfig{}); }

        Result<std::vector<std::byte>, SerializationError> Save(const SaveConfig& config) const
        {
            std::vector<std::byte> buffer;
            
            BinaryWriter::Config writerConfig;
            writerConfig.compressionMode = config.compressionMode;
            writerConfig.compressionLevel = config.compressionLevel;
            writerConfig.compressionThreshold = config.compressionThreshold;
            
            BinaryWriter writer(buffer, writerConfig);
            
            // Write header
            BinaryHeader header;
            header.entityCount = static_cast<uint32_t>(m_entityManager.Size());
            header.archetypeCount = static_cast<uint32_t>(m_archetypeManager->GetArchetypeCount());
            
            writer.WriteHeader(header);
            
            // Serialize components
            m_entityManager.Serialize(writer);
            m_archetypeManager->Serialize(writer);
            m_componentStorage->Serialize(writer);   // format v6 ComponentStorage block (s8.1)
            m_relationshipGraph->Serialize(writer);
            m_resourceStorage.Serialize(writer);   // format v2 resource block

            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<std::vector<std::byte>, SerializationError>::Err(writer.GetError()) : 
                Result<std::vector<std::byte>, SerializationError>::Ok(std::move(buffer));
        }

        // Load validates structural integrity enough to fail cleanly -- returning
        // Err(SerializationError) -- on truncated, corrupt, or version-skewed input, without
        // out-of-bounds access, unbounded allocation, or hangs. It is NOT a security boundary:
        // it does not guarantee rejection of every maliciously-crafted archive, and callers must
        // not load saves from untrusted sources without their own validation.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(const std::filesystem::path& path, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(path);
            if (reader.HasError())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            }

            return LoadInternal(reader, std::move(componentRegistry), Config{});
        }

        static Result<std::unique_ptr<Registry>, SerializationError> Load(std::span<const std::byte> data, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry), Config{});
        }

        // 3.3: thread a Config (hence workScheduler) into the restored registry, so a
        // hot-reload restore keeps parallel iteration. Additive; the two-arg forms above
        // delegate with a default Config and are byte-for-byte unchanged.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(
            const std::filesystem::path& path,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            BinaryReader reader(path);
            if (reader.HasError())
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            return LoadInternal(reader, std::move(componentRegistry), config);
        }

        // In-memory data form of the 3.3 Config overload above.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(
            std::span<const std::byte> data,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry), config);
        }

    private:
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

        // True when T's value lives in a ComponentStorage store rather than a chunk column.
        // A Sparse-trait T always routes to a store: its set, or its universal column when
        // this registry declares it universal (decided at compile time). A Table-trait T
        // routes to a store only when declared universal: a bool load plus a mask test, and
        // no descriptor read, in a registry with no universal (G4).
        template<Component T>
        ASTRA_NODISCARD bool RoutesToStore() const noexcept
        {
            if constexpr (StorageOfV<T> == Storage::Sparse)
                return true;
            else
                return m_componentStorage->HasUniversals() && m_componentStorage->IsDeclaredUniversal(TypeID<T>::Value());
        }

        // D2's single exception (spec 2026-10-01 s3.3, s12.3): a universal component is
        // present on every entity and cannot be removed. Debug: ASTRA_ASSERT. Release:
        // false. Never a signal. Callers check the handle FIRST, so a stale handle is a
        // quiet miss, never a refusal.
        static bool RefuseUniversalRemove() noexcept
        {
            ASTRA_ASSERT(false, "RemoveComponent on a Universal component is refused: it is present on every entity (spec 2026-10-01 s3.3)");
            return false;
        }

        // Batch form: one refusal per call if any handle is live (a batch of only stale
        // handles misses quietly). Returns 0, the removed count. (A CommandBuffer batch
        // remove executes per entity, so it refuses once per live entity.)
        size_t RefuseUniversalRemoveBatch(std::span<Entity> entities) const noexcept
        {
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    (void)RefuseUniversalRemove();
                    break;
                }
            }
            return 0;
        }

        // True when `id` is declared Universal in this registry: a bool load plus a mask
        // test, and no descriptor read, so Table add/remove never pay for KindOf (false in
        // a registry with no universal).
        ASTRA_NODISCARD bool IsUniversalId(ComponentID id) const noexcept
        {
            return m_componentStorage->HasUniversals() && m_componentStorage->IsDeclaredUniversal(id);
        }

        // Kind of `id` for SetEnabled/IsEnabled, with no descriptor read: Universal by the
        // declared mask, Sparse iff the id owns a slot (a Sparse id with no slot has no set,
        // and the Table mask test answers the same false/false), else Table.
        ASTRA_NODISCARD StorageKind EnableKindOf(ComponentID id) const noexcept
        {
            if (IsUniversalId(id)) return StorageKind::Universal;
            return m_componentStorage->SparseSlotOf(id) >= 0 ? StorageKind::Sparse : StorageKind::Table;
        }

        // Storage policies (spec s7.2): SetEnabled for a Sparse or Universal id. The STORE
        // flag is the truth; on a genuine change the chunk's disabled bit for the
        // component's slot is updated as the P3 mirror. An enableable Universal component
        // acquires its slot on its first disable and gets present == nullptr mirror blocks.
        // Contract = the Table body's: true iff the live entity holds an enableable
        // component, whether or not the state changed; signals fire on a genuine change
        // only. Absent components create no set and take no slot (RULINGS G2).
        bool SetEnabledInStore(Entity entity, const EntityRecord& rec, ComponentID id, StorageKind kind, bool enable)
        {
            const ComponentDescriptor* desc = m_componentRegistry->GetComponentDescriptor(id);
            if (!desc || !desc->isEnableable) ASTRA_UNLIKELY
                return false;                                      // not enableable: nothing to toggle
            int slot = m_componentStorage->SparseSlotOf(id);
            bool changed = false;
            void* element = nullptr;
            if (kind == StorageKind::Sparse)
            {
                SparseSet* set = m_componentStorage->FindSparse(id);
                if (!set || !set->Contains(entity))
                    return false;                                  // absent
                changed = set->SetDisabled(entity, !enable);       // SET == DISABLED
                element = desc->size == 0 ? EmptyComponentSentinel() : set->Get(entity);
            }
            else
            {
                UniversalColumn* column = m_componentStorage->FindUniversal(id);
                if (!column || !column->IsLive(entity))
                    return false;                                  // stale or never constructed
                if (!enable && slot < 0) ASTRA_UNLIKELY
                {
                    slot = m_componentStorage->AcquireSparseSlot(id);
                    if (slot < 0) ASTRA_UNLIKELY
                        return false;                              // slot cap exhausted (AcquireSparseSlot asserts)
                }
                changed = column->SetDisabled(entity, !enable);
                element = desc->size == 0 ? EmptyComponentSentinel() : column->Get(entity);
            }
            if (changed)
            {
                ASTRA_ASSERT(slot >= 0, "SetEnabledInStore: a genuine change implies an assigned sparse slot");
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

        // Write-side stamp routed by kind: Table -> the chunk column (today's
        // ArchetypeManager::MarkWritten); store kinds -> the store. Same false paths
        // everywhere: invalid/stale handle, absent component, tag. A registry with no
        // store never reads a descriptor kind (one bool check).
        bool MarkWrittenRouted(Entity entity, ComponentID id)
        {
            if (m_componentStorage->HasAnyStore() && id < MAX_COMPONENTS
                && m_componentStorage->KindOf(id) != StorageKind::Table) ASTRA_UNLIKELY
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
                case StorageKind::Sparse:
                {
                    // A write to a present Sparse element (spec s3.3): the element's `changed`
                    // tick when tracked (inside MarkWritten), plus the component's coarse
                    // version. Tag, absent or stale: false.
                    SparseSet* set = m_componentStorage->FindSparse(id);
                    return set && Detail::StampSparseWrite(*m_componentStorage, *set, id, entity, CurrentTick());
                }
                default:
                    return false;
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
                case StorageKind::Sparse:
                {
                    // Tracked: the element's exact ticks. Untracked: the component's coarse
                    // version, so every member answers alike (IsAdded == IsChanged). Tag,
                    // absent or stale: false.
                    const SparseSet* set = m_componentStorage->FindSparse(id);
                    if (!set || set->Descriptor().size == 0 || !set->Contains(entity)) ASTRA_UNLIKELY
                        return false;
                    if (const EntityTicks* t = set->Ticks(entity))
                        return IsNewer(added ? t->added : t->changed, since);
                    return IsNewer(m_componentStorage->CoarseVersion(id), since);
                }
                default:
                    return false;
            }
        }

        // Non-stamping, type-erased store lookup: the element pointer (or
        // EmptyComponentSentinel() for a present tag), or nullptr when absent or stale.
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

        // The ONE per-type instance a store-kind tag hands out as its typed payload (the
        // convention ArchetypeChunk::GetComponent<T> uses for a column-less tag). StoreGet
        // and SparseTypedPtr both return it, so a Sparse tag's ComponentAdded/Removed
        // payload equals GetComponent<Tag>(e) on every path.
        template<Component T>
        ASTRA_NODISCARD static T* StoreTagInstance() noexcept
        {
            return Detail::StoreTagInstance<T>();   // shared with Relations' GetInStorage
        }

        // Storage-aware membership and element of one component id (spec s3.3, reflection
        // row). Sparse/Universal members are in no archetype mask, so every reflection API
        // asks the store when the mask misses. Sets `present`; returns the data pointer, or
        // EmptyComponentSentinel() for a present tag. A Table id (or an absent/stale
        // handle): nullptr, present == false. Delegates to StoreLookup (non-creating, G2).
        ASTRA_NODISCARD void* StoredElementByID(Entity entity, ComponentID id, bool& present) const noexcept
        {
            void* p = StoreLookup(entity, id);
            present = p != nullptr;
            return p;
        }

        // Typed twin of StoreLookup. A present tag yields the shared per-type instance.
        template<Component T>
        ASTRA_NODISCARD T* StoreGet(Entity entity) const noexcept
        {
            void* p = StoreLookup(entity, TypeID<T>::Value());
            if (!p)
                return nullptr;
            if constexpr (std::is_empty_v<T>)
                return StoreTagInstance<T>();
            else
                return static_cast<T*>(p);
        }

        // ---- Storage policies: the Sparse funnel (spec s7.2) ----

        // Effective kind of a Sparse-TRAIT type in this registry: Sparse, or Universal
        // when the Config declares it. Only called inside
        // `if constexpr (StorageOfV<T> == Storage::Sparse)`, so Table hot paths never pay
        // the registration + lookup. KindOf reads T's descriptor, hence the registration.
        template<Component T>
        ASTRA_NODISCARD StorageKind ResolveKind() const
        {
            m_componentRegistry->RegisterComponent<T>();
            return KindOfT<T>();
        }

        // The typed signal payload for a Sparse element: the element itself, or (for a
        // tag) the shared per-type instance (spec s7.4).
        template<Component T>
        ASTRA_NODISCARD static T* SparseTypedPtr(void* element) noexcept
        {
            if constexpr (std::is_empty_v<T>)
            {
                (void)element;
                return StoreTagInstance<T>();
            }
            else
            {
                return static_cast<T*>(element);
            }
        }

        // The ONE place a Sparse membership is created. Inserts `entity` into `id`'s set
        // (moving from `src`, or default-constructing when src == nullptr; always nullptr
        // for a tag), sets the presence bit at the entity's chunk slot (P1) and bumps the
        // sparse structural counter. Preconditions: `entity` is the LIVE handle and `rec`
        // its validated record; not yet a member. Returns the element pointer
        // (EmptyComponentSentinel() for a tag), or nullptr when the
        // ASTRA_MAX_SPARSE_COMPONENTS cap refuses the component (AcquireSparseSlot asserts
        // in Debug; Release refuses, spec s3.2).
        void* SparseAttach(Entity entity, const EntityRecord& rec, ComponentID id, void* src)
        {
            SparseSet* set = m_componentStorage->Sparse(id);   // acquires the slot on first use
            if (!set) ASTRA_UNLIKELY
                return nullptr;
            const int slot = m_componentStorage->SparseSlotOf(id);
            ASTRA_ASSERT(slot >= 0 && !set->Contains(entity), "SparseAttach: no slot, or entity is already a member");
            void* element = src ? set->InsertMove(entity, src) : set->Insert(entity);
            rec.chunk->SetSparsePresent(static_cast<uint16_t>(slot), rec.location.GetEntityIndex(), true);
            m_archetypeManager->BumpSparseStructuralCounter();
            // A tracked element's {added, changed} are stamped by the set on insert; an
            // untracked one is born into the component's coarse version.
            m_componentStorage->StampCoarse(id, CurrentTick());
            return set->Descriptor().size == 0 ? EmptyComponentSentinel() : element;
        }

        // Erases `entity` from `id`'s set and clears its presence bit (which also clears
        // its disabled mirror bit). ComponentRemoved is the caller's and fires BEFORE
        // this. Non-creating (G2). Returns false when `entity` is not a member.
        bool SparseDetach(Entity entity, const EntityRecord& rec, ComponentID id)
        {
            const int slot = m_componentStorage->SparseSlotOf(id);
            SparseSet* set = m_componentStorage->FindSparse(id);
            if (slot < 0 || !set || !set->Erase(entity)) ASTRA_UNLIKELY
                return false;
            rec.chunk->SetSparsePresent(static_cast<uint16_t>(slot), rec.location.GetEntityIndex(), false);
            m_archetypeManager->BumpSparseStructuralCounter();
            return true;
        }

        // True when `id` is a Sparse component in this registry (a Universal declaration
        // wins, so a Sparse-trait type declared Universal is not). Runtime twin of
        // ResolveKind<T>() == Sparse for the type-erased paths.
        ASTRA_NODISCARD bool IsSparseId(ComponentID id) const noexcept
        {
            return m_componentStorage->KindOf(id) == StorageKind::Sparse;
        }

        // Overwrites a default-constructed element with the type-erased source through
        // the descriptor's shared construction ladder (also MoveAndAddByID's).
        static void AssignErased(const ComponentDescriptor& desc, void* dst, const void* src)
        {
            if (!desc.is_trivially_copyable)
                desc.Destruct(dst);
            desc.ConstructFromErased(dst, src);
        }

        // By-id Sparse add with ArchetypeManager::AddComponentByID contract: false for
        // an unknown id, a size mismatch, a stale handle, an id the entity already holds,
        // or a slot-cap refusal. `emit` gates ComponentAdded (the batch form never emits).
        bool SparseAddByID(Entity entity, ComponentID id, const void* data, size_t dataSize, bool emit)
        {
            const ComponentDescriptor* desc = m_componentRegistry->GetComponentDescriptor(id);
            if (!desc) ASTRA_UNLIKELY
                return false;
            if (dataSize != desc->size && desc->size > 0) ASTRA_UNLIKELY
                return false;
            if (!m_entityManager.IsValid(entity)) ASTRA_UNLIKELY
                return false;                              // SparseSet::Insert takes only a LIVE handle
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return false;
            if (const SparseSet* set = m_componentStorage->FindSparse(id); set && set->Contains(entity))
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

        // By-id Sparse remove: ComponentRemoved (when `emit`) BEFORE the erase, with the
        // element pointer (the sentinel for a tag); false when stale or absent.
        // Non-creating (G2).
        bool SparseRemoveByID(Entity entity, ComponentID id, bool emit)
        {
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            SparseSet* set = m_componentStorage->FindSparse(id);
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

        // Typed batch add over de-duplicated live entities. Absent -> attach a fresh value
        // from make(); present -> untouched. Then ComponentAdded for every entity HOLDING
        // T: the exact post-pass rule of Registry::AddComponents for Table. A slot-cap
        // refusal is dropped in Release (AcquireSparseSlot asserts in Debug; the batch
        // add is void).
        template<Component T, typename MakeFn>
        void SparseBatchAdd(std::span<const Entity> entities, MakeFn&& make)
        {
            const ComponentID id = TypeID<T>::Value();
            for (Entity e : entities)
            {
                const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
                if (!rec || !rec->chunk) ASTRA_UNLIKELY
                    continue;
                if (const SparseSet* set = m_componentStorage->FindSparse(id); set && set->Contains(e))
                    continue;
                if constexpr (std::is_empty_v<T>)
                {
                    (void)make;
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
                SparseSet* set = m_componentStorage->FindSparse(id);
                if (!set)
                    return;
                for (Entity e : entities)
                    if (set->Contains(e))
                        m_signalManager.Emit<Events::ComponentAdded>(e, id, SparseTypedPtr<T>(set->Get(e)));
            }
        }

        // Typed batch remove, mirroring Registry::RemoveComponents for Table:
        // ComponentRemoved fires per occurrence BEFORE anything is erased (a duplicated
        // entity double-signals, the documented Table quirk); erasure runs once per
        // de-duplicated member. Returns the number removed. Non-creating (G2).
        template<Component T>
        size_t SparseBatchRemove(std::span<Entity> entities)
        {
            const ComponentID id = TypeID<T>::Value();
            SparseSet* set = m_componentStorage->FindSparse(id);
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

        // Batch twins of the two helpers above: the guard is tested once, then the loop runs.
        void ConstructUniversalsForBatch(std::span<const Entity> made)
        {
            if (m_componentStorage->HasUniversals()) ASTRA_UNLIKELY
            {
                for (const Entity entity : made)
                    m_componentStorage->ConstructUniversals(entity, nullptr, 0);
            }
        }

        void EmitUniversalAddedBatch(std::span<const Entity> made)
        {
            if (!m_componentStorage->HasUniversals()) ASTRA_LIKELY
                return;
            if (!m_signalManager.IsSignalEnabled(Signal::ComponentAdded)) ASTRA_LIKELY
                return;
            for (const Entity entity : made)
                EmitUniversalAdded(entity);
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
            // Release: a Sparse member past the 32-slot cap is dropped here (Sparse() returns
            // nullptr). Debug asserts in AcquireSparseSlot; a creator has no return channel
            // for spec s3.2's Release refusal (ledgered deviation).
            switch (m_componentStorage->KindOf(id))
            {
                case StorageKind::Table:
                    (void)m_archetypeManager->AddComponent<C>(entity);
                    break;
                case StorageKind::Sparse:
                    if (const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity); rec && rec->chunk) ASTRA_LIKELY
                    {
                        const SparseSet* set = m_componentStorage->FindSparse(id);
                        if (!set || !set->Contains(entity)) ASTRA_LIKELY
                            (void)SparseAttach(entity, *rec, id, nullptr);
                    }
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
                    if (const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity); rec && rec->chunk) ASTRA_LIKELY
                    {
                        const SparseSet* set = m_componentStorage->FindSparse(id);
                        if (!set || !set->Contains(entity)) ASTRA_LIKELY
                            moveInto([&](void* src) { return SparseAttach(entity, *rec, id, src); });
                    }
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

        // Batch twin of the single-entity tail: every EntityCreated first, then the pack's
        // ComponentAdded for every entity, then the universals' (today's batch order).
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
                    EmitPackAdded<Components...>(e);
            }
            EmitUniversalAddedBatch(made);   // today's batch order: the pack's ComponentAdded for every entity, then the universals'
        }

        /**
         * Internal helper to load registry from reader
         */
        static Result<std::unique_ptr<Registry>, SerializationError> LoadInternal(BinaryReader& reader, std::shared_ptr<ComponentRegistry> componentRegistry, const Config& config)
        {
            // Read and validate header
            auto headerResult = reader.ReadHeader();
            if (headerResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*headerResult.GetError());
            }

            // Deserialize EntityManager
            auto managerResult = EntityManager::Deserialize(reader);
            if (managerResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*managerResult.GetError());
            }

            // Construct with the caller's Config so its runtime policy (workScheduler,
            // resource storage) is honoured. Note: entityManagerConfig / chunkPoolConfig
            // are superseded below by the deserialized entity manager + archetypes (the
            // saved shape wins); only workScheduler-class fields survive Load.
            auto registry = std::make_unique<Registry>(componentRegistry, config);

            // Move the entity manager from unique_ptr. This restores versions into
            // registry->m_entityManager's record table BEFORE the ArchetypeManager
            // below is created against that same table -- the deserialize ordering
            // the shared-table design requires (versions first, then locations).
            registry->m_entityManager = std::move(*(*managerResult.GetValue()));

            // INVARIANT: EntityManager restores versions into the shared record table BEFORE
            // ArchetypeManager writes locations into the same slots (Task 5 / W1 unified record).
            // Create new ArchetypeManager pointing at the (now version-restored)
            // shared record table, then deserialize archetype/location into it.
            registry->m_archetypeManager = std::make_shared<ArchetypeManager>(componentRegistry, config.chunkPoolConfig, &registry->m_entityManager.GetRecordTable());
            // Storage policies (spec 2026-10-01 s4): rebuilt fresh beside the archetype manager,
            // from the CALLER's Config, so the loaded registry's universal set is the loading
            // build's (storage-agnostic load, s8.3), never the archive's. Must stay BEFORE
            // m_archetypeManager->Deserialize. (Redundant with the ctor's store today; load-bearing
            // from Task 9, when the tick source points into the rebuilt ArchetypeManager.)
            registry->m_componentStorage = std::make_shared<ComponentStorage>(componentRegistry, config.universal);
            // The stores stamp from the archetype manager's counter. Both were just
            // rebuilt, so re-point them; a stale pointer would be a use-after-free.
            registry->m_componentStorage->SetTickSource(registry->m_archetypeManager->TickSource());
            registry->m_archetypeManager->SetComponentStorage(registry->m_componentStorage.get());
            if (!registry->m_archetypeManager->Deserialize(reader))
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::CorruptedData);
            }

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
            
            // Deserialize RelationshipGraph
            auto graphResult = RelationshipGraph::Deserialize(reader);
            if (graphResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*graphResult.GetError());
            }
            registry->m_relationshipGraph = std::make_shared<RelationshipGraph>(std::move(*graphResult.GetValue()));

            // Resource block exists from format v2 onward.
            if (reader.GetVersion() >= 2)
            {
                if (!registry->m_resourceStorage.Deserialize(reader))
                {
                    return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::UnknownComponent);
                }
            }

            // Verify checksum
            auto checksumResult = reader.VerifyChecksum();
            if (checksumResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*checksumResult.GetError());
            }
            
            return Result<std::unique_ptr<Registry>, SerializationError>::Ok(std::move(registry));
        }
        
        EntityManager m_entityManager;
        std::shared_ptr<ComponentRegistry> m_componentRegistry;
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        // Storage policies (spec 2026-10-01 s4): built in every ctor from Config::universal,
        // cleared in place by Clear(), rebuilt fresh by LoadInternal. Declared right after
        // m_archetypeManager: every ctor initializes it in that order.
        std::shared_ptr<ComponentStorage> m_componentStorage;
        std::shared_ptr<RelationshipGraph> m_relationshipGraph;
        SignalManager m_signalManager;
        ResourceStorage m_resourceStorage;
        // Stable block source for CommandBuffer storage (Commands C1 fix). Holds
        // a std::mutex, so Registry is intentionally non-movable (documented).
        CommandBlockArena m_commandBlockArena;
        std::shared_ptr<IWorkScheduler> m_workScheduler;  // null = sequential inline fallback
        Config m_config;   // retained so Clear()/Load() preserve pool + storage policy
        inline static std::atomic<std::uint64_t> s_nextInstanceId{1};   // 0 is never assigned
        std::uint64_t m_instanceId;   // last member: initialized last in every ctor
    };

    // Format v6 block reader (stub). Every block is DECODED into scratch values
    // through the loading registry's descriptor and discarded (the
    // ResourceStorage::Deserialize transient-resource pattern, RULINGS Task 31):
    // that keeps the stream aligned and the full suite green until Task 32 replaces
    // this with the real reader. Unknown hash -> false (the caller maps it to
    // UnknownComponent); malformed data -> false with the reader error or not.
    inline bool ComponentStorage::Deserialize(BinaryReader& r, Registry& registry)
    {
        const ComponentRegistry* creg = registry.GetComponentRegistry();
        uint32_t stored = 0;
        r(stored);
        if (r.HasError() || !creg) return false;
        if (r.CountExceedsRemaining(stored, StorageFormat::BLOCK_HEADER_BYTES)) return false;

        for (uint32_t b = 0; b < stored; ++b)
        {
            uint64_t hash = 0;
            uint32_t version = 0;
            uint8_t kind = 0, flags = 0;
            uint32_t count = 0;
            r(hash)(version)(kind)(flags)(count);
            if (r.HasError()) return false;

            const ComponentDescriptor* desc = creg->GetComponentDescriptorByHash(hash);
            if (!desc) return false;
            if ((kind != StorageFormat::KIND_SPARSE && kind != StorageFormat::KIND_UNIVERSAL)
                || (flags & ~StorageFormat::FLAG_MASK) != 0 || count == 0)
                return false;

            // Same gate as the writer: LZ4 archives frame each component's payload.
            std::vector<uint8_t> inflated;
            std::optional<BinaryReader> sub;
            BinaryReader* in = &r;
            if (r.GetCompressionMode() == CompressionMode::LZ4)
            {
                auto blk = r.ReadCompressedBlock();
                if (blk.IsErr()) return false;
                inflated = std::move(*blk.GetValue());
                sub.emplace(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(inflated.data()), inflated.size()));
                sub->SetChecksumEnabled(false);
                in = &*sub;
            }

            if (in->CountExceedsRemaining(count, sizeof(Entity))) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                Entity e;
                (*in)(e);
            }
            if (in->HasError()) return false;

            if ((flags & StorageFormat::FLAG_HAS_VALUES) && desc->size > 0)
            {
                AllocResult scratch = AllocateMemory(desc->size, desc->alignment);
                if (!scratch.ptr) return false;
                bool ok = true;
                for (uint32_t i = 0; i < count && ok; ++i)
                {
                    desc->DefaultConstruct(scratch.ptr);
                    ok = desc->deserializeVersioned(*in, scratch.ptr);
                    desc->Destruct(scratch.ptr);
                }
                FreeMemory(scratch.ptr, desc->size);
                if (!ok) return false;
            }

            if (flags & StorageFormat::FLAG_HAS_DISABLED)
            {
                std::vector<uint8_t> disabled;
                if (!StorageFormat::ReadDisabledBitmap(*in, count, disabled)) return false;
            }
            if (in->HasError()) return false;
        }
        return !r.HasError();
    }
}
