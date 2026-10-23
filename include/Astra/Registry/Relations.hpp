#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <queue>
#include <type_traits>

#include "../Archetype/ArchetypeManager.hpp"
#include "../Container/FlatSet.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Simd.hpp"
#include "../Core/WorkScheduler.hpp"
#include "../Entity/Entity.hpp"
#include "Query.hpp"
#include "RelationshipGraph.hpp"
#include "StorageAccess.hpp"

namespace Astra
{
    class Registry;

    template<typename... QueryArgs>
    class Relations
    {
        static_assert(ValidQuery<QueryArgs...>, "Relations template arguments must be valid components or query modifiers");
        
        static constexpr bool HAS_FILTERING = sizeof...(QueryArgs) > 0;
        
        // Parallel execution threshold: below this, sequential inline is used.
        static constexpr size_t MIN_ENTITIES_FOR_PARALLEL = 1024;
        
        // Extract query information only if filtering
        using Classifier = std::conditional_t<HAS_FILTERING,
            Detail::QueryClassifier<QueryArgs...>,
                Detail::QueryClassifier<>>;
        using RequiredTuple = typename Classifier::RequiredComponents;
        using ExcludedTuple = typename Classifier::ExcludedComponents;
        using AnyGroups = typename Classifier::AnyGroups;
        using OneOfGroups = typename Classifier::OneOfGroups;

        static_assert(std::tuple_size_v<typename Classifier::WithComponents> == 0,
            "With<T> is not yet supported in Relations queries (it would be silently ignored). "
            "Use a plain required component, or filter after the relation query.");

        // Storage policies (spec 2026-10-01 s6.5): effective kind of every named type,
        // resolved once per Relations object (Universal is a per-registry fact). The
        // manager's typed HasComponent/GetComponent see Table columns only, so filters and
        // yields dispatch on this. Store POINTERS are never cached (module unload, Task 36).
        using PlanTypes = Detail::QueryPlanTypes<QueryArgs...>;
        static constexpr size_t NPlanSlots = std::tuple_size_v<PlanTypes> == 0 ? 1 : std::tuple_size_v<PlanTypes>;

        template<typename T>
        ASTRA_NODISCARD StorageKind KindOfT() const noexcept
        {
            return m_kinds[Detail::TupleIndexOf<std::remove_const_t<T>, PlanTypes>::value];
        }

        // The ComponentID of a named type, resolved once per Relations object: the Table
        // traversals hand it to the manager's *WithID fetches instead of paying the runtime
        // TypeID<T>::Value() lookup (an out-of-line call) several times per entity.
        template<typename T>
        ASTRA_NODISCARD ComponentID IdOfT() const noexcept
        {
            return m_ids[Detail::TupleIndexOf<std::remove_const_t<T>, PlanTypes>::value];
        }

        void ResolveKinds() noexcept
        {
            m_kinds.fill(StorageKind::Table);
            m_ids.fill(INVALID_COMPONENT);
            m_allTable = true;
            const ComponentStorage* storage = m_archetypeManager ? m_archetypeManager->GetComponentStorage() : nullptr;
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((m_kinds[Detail::TupleIndexOf<Ts, PlanTypes>::value] = Detail::EffectiveKindOf<Ts>(storage)), ...);
                ((m_ids[Detail::TupleIndexOf<Ts, PlanTypes>::value] = TypeID<Ts>::Value()), ...);
            }(static_cast<PlanTypes*>(nullptr));
            for (StorageKind k : m_kinds)
                m_allTable = m_allTable && k == StorageKind::Table;
        }

        // Presence of T on `entity`. `Fast` (every named type is Table-kind in this registry,
        // m_allTable, resolved once per Relations object and dispatched ONCE per traversal)
        // is the manager's direct typed test, exactly as before storage policies, with no
        // storage, kind or flag load per entity (spec s6.4 / s11 R2). Otherwise the
        // kind-dispatched test.
        template<bool Fast, typename T>
        ASTRA_FORCEINLINE bool HasT(Entity entity) const
        {
            if constexpr (Fast)
                return m_archetypeManager->HasComponentWithID(entity, IdOfT<T>());
            else
                return Detail::HasInStorage<T>(*m_archetypeManager, entity, KindOfT<T>());
        }

    public:
        Relations(std::shared_ptr<ArchetypeManager> manager, Entity entity,
                  std::shared_ptr<const RelationshipGraph> graph,
                  std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
            m_archetypeManager(manager),
            m_rootEntity(entity),
            m_relationsGraph(graph),
            m_scheduler(std::move(scheduler))
        {
            ResolveKinds();
        }
        
        /**
         * Check if the Relations object is still valid (Registry not destroyed)
         * @return true if both ArchetypeManager and RelationshipGraph are still alive
         */
        ASTRA_NODISCARD bool IsValid() const noexcept
        {
            return m_archetypeManager != nullptr && m_relationsGraph != nullptr;
        }
        
        /**
         * @brief Get the parent of the entity (filtered by components if applicable)
         * @return Parent entity if it exists and passes filter, Entity::Null() otherwise
         */
        ASTRA_FORCEINLINE Entity GetParent() const
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return Entity::Invalid();  // Registry destroyed
            
            Entity parent = m_relationsGraph->GetParent(m_rootEntity);
            if constexpr (!HAS_FILTERING)
            {
                return parent;
            }
            else
            {
                return (parent.IsValid() && PassesFilter(parent)) ? parent : Entity::Invalid();
            }
        }
        
        /**
         * @brief Get the graph-wide structure version (see RelationshipGraph::StructureVersion)
         * @return The current version, or 0 if the Registry has been destroyed
         */
        ASTRA_FORCEINLINE std::uint32_t StructureVersion() const noexcept
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return 0;  // Registry destroyed -- 0 matches this codebase's
                           // existing "no valid version" sentinel (TraversalCache::IsValid)

            return m_relationsGraph->StructureVersion();
        }

        ASTRA_FORCEINLINE auto GetChildren() const
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return RelationshipGraph::ChildrenContainer{};
            
            if constexpr (!HAS_FILTERING)
            {
                // No filtering - return direct reference
                return m_relationsGraph->GetChildren(m_rootEntity);
            }
            else
            {
                // With filtering - build filtered list
                RelationshipGraph::ChildrenContainer filtered;
                const auto& children = m_relationsGraph->GetChildren(m_rootEntity);
                for (Entity child : children)
                {
                    if (PassesFilter(child))
                    {
                        filtered.push_back(child);
                    }
                }
                return filtered;
            }
        }
        
        ASTRA_FORCEINLINE auto GetLinks() const
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return RelationshipGraph::LinksContainer{};
            
            if constexpr (!HAS_FILTERING)
            {
                // No filtering - return direct reference
                return m_relationsGraph->GetLinks(m_rootEntity);
            }
            else
            {
                // With filtering - build filtered list
                RelationshipGraph::LinksContainer filtered;
                const auto& links = m_relationsGraph->GetLinks(m_rootEntity);
                for (Entity linked : links)
                {
                    if (PassesFilter(linked))
                    {
                        filtered.push_back(linked);
                    }
                }
                return filtered;
            }
        }
        
        template<typename Func>
        ASTRA_FORCEINLINE void ForEachChild(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // Snapshot (value copy): m_relationsGraph->GetChildren() returns a
            // const ChildrenContainer& into RelationshipGraph's live m_children
            // map. A callback that destroys/reparents entities can mutate
            // (swap-and-pop) or free (map erase/rehash) that live storage, so
            // we must iterate a local copy, not the live reference.
            auto children = m_relationsGraph->GetChildren(m_rootEntity);

            // Early exit for empty
            if (children.empty()) ASTRA_UNLIKELY
                return;

            // Process children with proper component expansion
            ForEachChildImpl(children, std::forward<Func>(func), RequiredTuple{});
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ForEachDescendant(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // Snapshot (value copy): GetDescendantsCached() returns a
            // const TraversalCache& into RelationshipGraph's live cache map. A
            // callback that mutates the graph can invalidate/erase/rehash that
            // cache entry, so we copy the cache (including its entries vector)
            // before iterating.
            auto cache = m_relationsGraph->GetDescendantsCached(m_rootEntity);

            const size_t count = cache.entries.size();
            if (count == 0) ASTRA_UNLIKELY
                return;

            // Process with proper component expansion
            ForEachDescendantImpl(cache, count, std::forward<Func>(func), RequiredTuple{});
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ForEachAncestor(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // Snapshot (value copy) - see ForEachDescendant.
            auto cache = m_relationsGraph->GetAncestorsCached(m_rootEntity);

            const size_t count = cache.entries.size();
            if (count == 0) ASTRA_UNLIKELY
                return;

            // Process with proper component expansion
            ForEachAncestorImpl(cache, count, std::forward<Func>(func), RequiredTuple{});
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ForEachLink(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // Snapshot (value copy) - see ForEachChild.
            auto links = m_relationsGraph->GetLinks(m_rootEntity);

            if (links.empty()) ASTRA_UNLIKELY
                return;

            // Process with proper component expansion
            ForEachLinkImpl(links, std::forward<Func>(func), RequiredTuple{});
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEachDescendant(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // Astra creates no threads — parallelism requires an injected scheduler.
            // Fall back to the sequential path (which takes its own snapshot) before
            // paying for a cache copy when there's no scheduler at all.
            if (!m_scheduler)
            {
                return ForEachDescendant(std::forward<Func>(func));
            }

            // Snapshot (value copy): GetDescendantsCached() returns a TraversalCache
            // value copied under the cache lock. We must own a local copy here -- the
            // ParallelFor lambda reads cache.entries lock-free from worker threads, and
            // a concurrent insertion into the non-pointer-stable cache map would rehash
            // and dangle any reference into it (CR-1). The lambda captures this local
            // copy by reference; ParallelFor is synchronous, so the copy outlives every
            // worker.
            auto cache = m_relationsGraph->GetDescendantsCached(m_rootEntity);
            const size_t count = cache.entries.size();

            if (count < MIN_ENTITIES_FOR_PARALLEL) ASTRA_LIKELY
            {
                return ForEachDescendant(std::forward<Func>(func));
            }

            constexpr size_t kBatchSize = 64;
            auto run = [&](auto fastTag)
            {
            constexpr bool Fast = decltype(fastTag)::value;
            m_scheduler->ParallelFor(count, kBatchSize, [&](size_t begin, size_t end, uint32_t /*worker*/)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    const auto& entry = cache.entries[i];
                    Entity entity = entry.entity;
                    size_t depth = entry.depth;

                    if (PassesFilterK<Fast>(entity))
                    {
                        // Empty RequiredTuple covers the unfiltered case too
                        // (no QueryArgs => no required components).
                        if constexpr (std::tuple_size_v<RequiredTuple> == 0)
                        {
                            func(entity, depth);
                        }
                        else
                        {
                            InvokeParallelWithDepth<Fast>(entity, depth, func, RequiredTuple{});
                        }
                    }
                }
            });
            };
            if (m_allTable) ASTRA_LIKELY
                run(std::true_type{});
            else
                run(std::false_type{});
        }
        
    private:
        template<typename Container, typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachChildImpl(const Container& children, Func&& func, std::tuple<Components...>)
        {
            ForEachEntityList(children, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachDescendantImpl(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::tuple<Components...>)
        {
            ForEachCachedWithDepth(cache, count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachAncestorImpl(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::tuple<Components...>)
        {
            ForEachCachedWithDepth(cache, count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Container, typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachLinkImpl(const Container& links, Func&& func, std::tuple<Components...>)
        {
            ForEachEntityList(links, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Container, typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachEntityList(const Container& entities, Func&& func, std::index_sequence<Is...> is)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // The Table/other-kind choice is made once per traversal, not per entity.
            if (m_allTable) ASTRA_LIKELY
                ForEachEntityListK<true>(entities, std::forward<Func>(func), is);
            else
                ForEachEntityListCold(entities, std::forward<Func>(func), is);
        }

        // The Sparse/Universal-aware loop, kept out of line so the Table loop above is the
        // only code inlined into the caller (the pre-storage shape).
        template<typename Container, typename Func, size_t... Is>
        ASTRA_NOINLINE void ForEachEntityListCold(const Container& entities, Func&& func, std::index_sequence<Is...> is)
        {
            ForEachEntityListK<false>(entities, std::forward<Func>(func), is);
        }

        template<bool Fast, typename Container, typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachEntityListK(const Container& entities, Func&& func, std::index_sequence<Is...>)
        {
            for (Entity entity : entities)
            {
                if (PassesFilterK<Fast>(entity))
                {
                    if constexpr (!HAS_FILTERING)
                    {
                        func(entity);
                    }
                    else if constexpr (sizeof...(Is) == 0)
                    {
                        func(entity);
                    }
                    else
                    {
                        // Expand components using index_sequence
                        func(entity, *FetchYield<Fast, false, std::tuple_element_t<Is, RequiredTuple>>(entity)...);
                    }
                }
            }
        }
        
        template<typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachCachedWithDepth(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::index_sequence<Is...> is)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            // The Table/other-kind choice is made once per traversal, not per entity.
            if (m_allTable) ASTRA_LIKELY
                ForEachCachedWithDepthK<true>(cache, count, std::forward<Func>(func), is);
            else
                ForEachCachedWithDepthCold(cache, count, std::forward<Func>(func), is);
        }

        // Out of line, as ForEachEntityListCold.
        template<typename Func, size_t... Is>
        ASTRA_NOINLINE void ForEachCachedWithDepthCold(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::index_sequence<Is...> is)
        {
            ForEachCachedWithDepthK<false>(cache, count, std::forward<Func>(func), is);
        }

        template<bool Fast, typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachCachedWithDepthK(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::index_sequence<Is...>)
        {
            for (size_t i = 0; i < count; ++i)
            {
                const auto& entry = cache.entries[i];
                Entity entity = entry.entity;
                size_t depth = entry.depth;
                
                if (PassesFilterK<Fast>(entity))
                {
                    if constexpr (!HAS_FILTERING)
                    {
                        func(entity, depth);
                    }
                    else if constexpr (sizeof...(Is) == 0)
                    {
                        func(entity, depth);
                    }
                    else
                    {
                        // Expand components using index_sequence
                        func(entity, depth, *FetchYield<Fast, false, std::tuple_element_t<Is, RequiredTuple>>(entity)...);
                    }
                }
            }
        }
        
        template<bool Fast, typename Func, typename... Components>
        ASTRA_FORCEINLINE void InvokeParallelWithDepth(Entity entity, size_t depth, Func&& func, std::tuple<Components...>)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            if constexpr (sizeof...(Components) == 0)
            {
                func(entity, depth);
            }
            else
            {
                func(entity, depth, *FetchYield<Fast, true, Components>(entity)...);
            }
        }
        
        // The one fetch every traversal yield goes through (spec 2026-09-10 §3.3,
        // final-review Ruling M). A required component requested non-const is handed
        // out as a WRITE, exactly like a view's non-const yield: it is fetched through
        // ArchetypeManager::GetComponentMut, which stamps C's column version in the
        // entity's chunk and, for a change-tracked C, marks the entity's `changed`
        // tick -- so a write made inside ForEachChild/Descendant/Ancestor/Link (the
        // transform-propagation shape) is visible to Changed<C> at both tiers. A
        // `const C` request, or a tag (no column), is a plain read through the
        // non-stamping GetComponent and touches nothing. The return type is the same
        // C* the site received before (GetComponentMut<C> == GetComponent<C> plus the
        // stamp), so callbacks are unaffected. The parallel variant calls this from
        // workers over a dense descendant range (not chunk-partitioned), so a chunk's
        // column version can be stamped by several workers: GetInStorage stamps it with
        // the Par=true relaxed shared store (same value from every worker, RULINGS
        // Task 28; the parallel callers pass Par, the serial ones stamp plainly), while the per-entity `changed` tick has one owner and stays plain.
        template<bool Fast, bool Par, typename C>
        ASTRA_FORCEINLINE C* FetchYield(Entity entity) const
        {
            if constexpr (Fast)
            {
                // All-Table plan: the manager's direct fetch, the pre-storage code.
                if constexpr (Detail::IsMutableYield<C>)
                    return m_archetypeManager->template GetComponentMutWithID<C, Par>(entity, IdOfT<C>());
                else
                    return m_archetypeManager->template GetComponentWithID<C>(entity, IdOfT<C>());
            }
            else
            {
                // Any kind (spec 2026-10-01 s6.5): Table via the manager, Sparse/Universal via
                // their stores. A non-const request is still a write for every kind. `Par`
                // (the parallel traversals, RULINGS Task 28) selects the relaxed atomic_ref
                // stamps; the serial traversals take the plain ones.
                return Detail::GetInStorage<C, Par>(*m_archetypeManager, entity, KindOfT<C>());
            }
        }

        // Single-entity filter (Parent(), GetChildren(), GetLinks()): not a hot loop, so the
        // kind choice is a plain runtime branch.
        ASTRA_FORCEINLINE bool PassesFilter(Entity entity) const
        {
            return m_allTable ? PassesFilterK<true>(entity) : PassesFilterK<false>(entity);
        }

        template<bool Fast>
        ASTRA_FORCEINLINE bool PassesFilterK(Entity entity) const
        {
            if constexpr (!HAS_FILTERING)
            {
                return true;  // No filtering, always pass
            }
            else
            {
                // Check required components
                if constexpr (std::tuple_size_v<RequiredTuple> > 0)
                {
                    if (!HasAllRequired<Fast>(entity, RequiredTuple{}))
                    {
                        return false;
                    }
                }
                
                // Check excluded components
                if constexpr (std::tuple_size_v<ExcludedTuple> > 0)
                {
                    if (HasAnyExcluded<Fast>(entity, ExcludedTuple{}))
                    {
                        return false;
                    }
                }
                
                // Check Any groups
                if constexpr (std::tuple_size_v<AnyGroups> > 0)
                {
                    if (!CheckAnyGroups<Fast>(entity))
                    {
                        return false;
                    }
                }
                
                // Check OneOf groups
                if constexpr (std::tuple_size_v<OneOfGroups> > 0)
                {
                    if (!CheckOneOfGroups<Fast>(entity))
                    {
                        return false;
                    }
                }
                
                return true;
            }
        }
        
        // Simplified component checking helpers (only compiled if HAS_FILTERING)
        template<bool Fast, typename... Ts>
        bool HasAllRequired(Entity entity, std::tuple<Ts...>) const
        {
            if constexpr (sizeof...(Ts) == 0)
            {
                return true;
            }
            else
            {
                if (!m_archetypeManager) ASTRA_UNLIKELY
                    return false;  // Registry destroyed
                return (HasT<Fast, Ts>(entity) && ...);
            }
        }
        
        template<bool Fast, typename... Ts>
        bool HasAnyExcluded(Entity entity, std::tuple<Ts...>) const
        {
            if constexpr (sizeof...(Ts) == 0)
            {
                return false;
            }
            else
            {
                if (!m_archetypeManager) ASTRA_UNLIKELY
                    return false;  // Registry destroyed
                return (HasT<Fast, Ts>(entity) || ...);
            }
        }
        
        template<bool Fast>
        bool CheckAnyGroups(Entity entity) const
        {
            return CheckAnyGroupsImpl<Fast>(entity, std::make_index_sequence<std::tuple_size_v<AnyGroups>>{});
        }
        
        template<bool Fast, size_t... Is>
        bool CheckAnyGroupsImpl(Entity entity, std::index_sequence<Is...>) const
        {
            return (CheckSingleAnyGroup<Fast, Is>(entity) && ...);
        }
        
        template<bool Fast, size_t I>
        bool CheckSingleAnyGroup(Entity entity) const
        {
            using Group = std::tuple_element_t<I, AnyGroups>;
            return HasAnyInGroup<Fast>(entity, Group{});
        }
        
        template<bool Fast, typename... Ts>
        bool HasAnyInGroup(Entity entity, std::tuple<Ts...>) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return false;  // Registry destroyed
            return (HasT<Fast, Ts>(entity) || ...);
        }
        
        template<bool Fast>
        bool CheckOneOfGroups(Entity entity) const
        {
            return CheckOneOfGroupsImpl<Fast>(entity, std::make_index_sequence<std::tuple_size_v<OneOfGroups>>{});
        }
        
        template<bool Fast, size_t... Is>
        bool CheckOneOfGroupsImpl(Entity entity, std::index_sequence<Is...>) const
        {
            return (CheckSingleOneOfGroup<Fast, Is>(entity) && ...);
        }
        
        template<bool Fast, size_t I>
        bool CheckSingleOneOfGroup(Entity entity) const
        {
            using Group = std::tuple_element_t<I, OneOfGroups>;
            return CountInGroup<Fast>(entity, Group{}) == 1;
        }
        
        template<bool Fast, typename... Ts>
        size_t CountInGroup(Entity entity, std::tuple<Ts...>) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed
            return ((HasT<Fast, Ts>(entity) ? 1 : 0) + ...);
        }

        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        Entity m_rootEntity;
        std::shared_ptr<const RelationshipGraph> m_relationsGraph;
        std::shared_ptr<IWorkScheduler> m_scheduler;
        std::array<StorageKind, NPlanSlots> m_kinds{};
        std::array<ComponentID, NPlanSlots> m_ids{};
        bool m_allTable = true;   // every named type is Table-kind in this registry (ResolveKinds)
    };
}
