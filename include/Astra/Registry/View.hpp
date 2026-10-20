#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/WorkScheduler.hpp"
#include "../Entity/Entity.hpp"
#include "ComponentStorage.hpp"
#include "EnabledRuns.hpp"
#include "Query.hpp"
#include "ViewIterator.hpp"

#include <bit>
#include <cstdint>

namespace Astra
{
    enum class QueryError { NotMatched, Empty, MultipleMatched };

    template<typename... QueryArgs>
    class View
    {
        static_assert(ValidQuery<QueryArgs...>, "View template arguments must be valid components or query modifiers");

        // Query type extraction - must be declared early for Iterator type
        using RequiredTypes = typename Detail::QueryClassifier<QueryArgs...>::RequiredComponents;
        using OptionalTypes = typename Detail::QueryClassifier<QueryArgs...>::OptionalComponents;
        using QueryBuilder = Astra::QueryBuilder<QueryArgs...>;  // qualified to avoid -Wchanges-meaning

        // ================= Enableable-components query filtering (spec §5) =================
        //
        // EnabledRequiredFilter: bare required enableable components (IncludeDisabled
        // opts out) -- drives run extraction + chunk skipping. EnabledOptionalFilter:
        // enableable Optional<T> -- pointer nulled per entity while disabled.
        //
        // HasEnabledFilter is the load-bearing compile-time gate: when it is false the
        // whole filtered path is never instantiated and ForEach/ParallelForEach/Size
        // compile to the exact pre-existing loops (invariant 1, zero cost when unused).
        using EnabledRequiredFilter = Detail::EnableableRequiredFilter_t<QueryArgs...>;
        using EnabledOptionalFilter = Detail::FilterEnableable_t<OptionalTypes>;

        static constexpr bool HasRequiredFilter = std::tuple_size_v<EnabledRequiredFilter> > 0;
        static constexpr bool HasOptionalFilter = std::tuple_size_v<EnabledOptionalFilter> > 0;
        static constexpr bool HasEnabledFilter  = HasRequiredFilter || HasOptionalFilter;

        // ================= Change-detection filters (spec 2026-09-10 §3.4) =================
        //
        // Changed<T>/Added<T> are match-only terms (T required, never yielded, zero
        // ViewAccess footprint) that reject whole chunks whose T column version is not
        // newer than the querying tick. HasChangeFilter is the compile-time gate, exactly
        // like HasEnabledFilter: a view naming no Changed/Added instantiates none of the
        // reject code. Iteration-only this stage: ForEach(ctx, fn) / Since(tick).ForEach.
        using ChangedTypes = typename Detail::QueryClassifier<QueryArgs...>::ChangedComponents;
        using AddedTypes   = typename Detail::QueryClassifier<QueryArgs...>::AddedComponents;
        static_assert(Detail::ChangeTermsAreRequired<QueryArgs...>,
            "Changed<T>/Added<T>: T must also be listed as a required component of this view "
            "(e.g. CreateView<const T, Changed<T>, ...>) -- filter what you fetch");
    public:
        static constexpr bool HasChangeFilter = (std::tuple_size_v<ChangedTypes> + std::tuple_size_v<AddedTypes>) > 0;
    private:
        // Any Changed<T>/Added<T> term over a change-TRACKED T: inside a chunk that
        // passed the version reject, that term is evaluated per entity from the tick
        // column (spec §3.4 step 3). False for every untracked change filter, which
        // stays chunk-granular and instantiates none of the per-entity mask code.
        template<typename Tuple> struct AnyTracked;
        template<typename... Ts> struct AnyTracked<std::tuple<Ts...>> : std::bool_constant<(IsChangeTrackedV<Ts> || ...)> {};
        static constexpr bool HasTrackedChangeTerm = AnyTracked<ChangedTypes>::value || AnyTracked<AddedTypes>::value;

        // Any yielded, non-const, change-tracked component: routes iteration through
        // the view-owned chunk loops (Archetype::ForEachStamped cannot yield Mut) and
        // switches the required yield to Mut<T>. False for every pre-existing view.
        template<typename Tuple> struct AnyTrackedYield;
        template<typename... Ts> struct AnyTrackedYield<std::tuple<Ts...>>
            : std::bool_constant<((Detail::IsMutableYield<Ts> && IsChangeTrackedV<Ts>) || ...)> {};
        static constexpr bool HasTrackedYield = AnyTrackedYield<RequiredTypes>::value || AnyTrackedYield<OptionalTypes>::value;

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
        // Any per-chunk filter at all: routes iteration through VisitChunkFiltered. A
        // Sparse argument always filters (presence words), so it joins the gate (s6.3).
        static constexpr bool HasChunkFilter = HasEnabledFilter || HasChangeFilter || HasSparseArg;

        // Sparse-trait members of the required (+ IncludeDisabled) and With<> sets --
        // the presence words a chunk must carry -- and of the Not<> set.
        using SparseRequiredTypes = Detail::FilterSparseTrait_t<decltype(std::tuple_cat(
            std::declval<RequiredTypes>(), std::declval<WithTypes>()))>;
        using SparseExcludedTypes = Detail::FilterSparseTrait_t<ExcludedTypes>;

        // Parallel execution thresholds - based on empirical testing
        static constexpr size_t AVG_ENTITIES_PER_CHUNK = 256;                           // Typical for 16KB chunks with ~50 byte entities
        static constexpr size_t MIN_CHUNKS_PER_THREAD = 4;                              // Each thread should process at least 4 chunks (64KB)
        static constexpr size_t MIN_CHUNKS_FOR_PARALLEL = MIN_CHUNKS_PER_THREAD * 2;    // Need enough for at least 2 threads

        // Derive entity thresholds from chunk-based values
        static constexpr size_t MIN_ENTITIES_QUICK_CHECK = AVG_ENTITIES_PER_CHUNK / 2;  // Less than half a chunk = definitely sequential
        static constexpr size_t MIN_ENTITIES_FOR_PARALLEL = MIN_CHUNKS_FOR_PARALLEL * AVG_ENTITIES_PER_CHUNK / 2;  // ~4 chunks worth

    public:
        explicit View(std::shared_ptr<ArchetypeManager> manager,
                      std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
            m_archetypeManager(manager),
            m_scheduler(std::move(scheduler)),   // null => sequential fallback
            m_lastRefreshCounter(0),
            m_lastGeneration(0)
        {
            BuildStoragePlan();
            if (manager)
            {
                CollectArchetypes();
                m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
                m_lastGeneration = m_archetypeManager->m_generation;
                m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            }
            // else: null manager -> empty/invalid view; all m_last* counters keep their
            // default-initialized 0 (View.hpp member initializers).
        }
        
        /**
         * Check if the View is still valid (Registry not destroyed)
         * @return true if the underlying ArchetypeManager is still alive
         */
        ASTRA_NODISCARD bool IsValid() const noexcept
        {
            return m_archetypeManager != nullptr;
        }

        /**
         * Invoke func(Entity, Components&...) -- or func(Components&...) (the
         * leading Entity is optional) -- for every entity matching this View.
         *
         * CONTRACT: structural mutation is NOT supported during this call. Do not
         * create or destroy entities, or add/remove components, from within func --
         * the Archetype/chunk pointers this loop walks are captured up front and are
         * not re-validated mid-iteration, so a direct structural change here is
         * undefined behavior. In-place edits to already-present component VALUES
         * (e.g. `pos.x += 1`) are fine.
         *
         * To change entity structure while iterating, record the changes into a
         * `CommandBuffer` and call `Execute()` AFTER this loop returns. Recording
         * into a CommandBuffer does not itself touch the ArchetypeManager, so a
         * properly deferred CommandBuffer never trips the Debug guard below.
         *
         * In Debug builds this is additionally enforced by an ASTRA_ASSERT that
         * compares the ArchetypeManager's structural-change counter before and
         * after the loop; the check (and the counter read) is compiled out
         * entirely in Release/Dist builds, so this contract carries zero Release
         * cost.
         *
         * Change detection (spec §3.4): a view with a Changed<T>/Added<T> filter
         * needs a "since" tick and refuses this overload at compile time -- call
         * ForEach(ctx, fn) from a SystemContext system, or Since(tick).ForEach(fn).
         */
        template<typename Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ForEach(ctx, fn) from a SystemContext system, or Since(tick).ForEach(fn).");
            ForEachSince(Tick{0}, std::forward<Func>(func));
        }

        /**
         * Tick-aware ForEach: `since` is taken from ctx.LastRun() (any TickContext --
         * SystemContext, or a test stand-in), so Changed<T>/Added<T> terms see exactly
         * what changed since this system last ran. Also valid on an unfiltered view.
         */
        template<TickContext Ctx, typename Func>
        ASTRA_FORCEINLINE void ForEach(const Ctx& ctx, Func&& func) { ForEachSince(ctx.LastRun(), std::forward<Func>(func)); }

        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(Func&& func)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ParallelForEach(ctx, fn) from a SystemContext system, or Since(tick).ParallelForEach(fn).");
            ParallelForEachSince(Tick{0}, std::forward<Func>(func));
        }

        template<TickContext Ctx, typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(const Ctx& ctx, Func&& func) { ParallelForEachSince(ctx.LastRun(), std::forward<Func>(func)); }

        /**
         * Explicit-tick form for Registry&-only systems and tests:
         * `view.Since(tick).ForEach(fn)` iterates with Changed<T>/Added<T> evaluated
         * against `tick`. A thin non-owning handle over this view; use it immediately.
         */
        class SinceView
        {
        public:
            SinceView(View& v, Tick since) noexcept : m_view(&v), m_since(since) {}
            template<typename Func> void ForEach(Func&& func)         { m_view->ForEachSince(m_since, std::forward<Func>(func)); }
            template<typename Func> void ParallelForEach(Func&& func) { m_view->ParallelForEachSince(m_since, std::forward<Func>(func)); }
        private:
            View* m_view;
            Tick  m_since;
        };
        ASTRA_NODISCARD SinceView Since(Tick since) noexcept { return SinceView(*this, since); }

    private:
        // ForEach with an explicit "since" tick: the one stamped walk behind
        // ForEach(fn), ForEach(ctx, fn) and Since(tick).ForEach(fn).
        template<typename Func>
        ASTRA_FORCEINLINE void ForEachSince(Tick since, Func&& func) { ForEachBody<true>(since, std::forward<Func>(func)); }

        // ParallelForEach with an explicit "since" tick (see ForEachSince).
        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEachSince(Tick since, Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            EnsureArchetypes();
            if (UsesStoragePath())
                ResolveStores();     // on this thread, before any worker reads m_stores

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;

            auto adapted = MakeEntityOptionalAdapter(func);

            // Quick check: if we have very few matching entities, don't even try parallel
            size_t quickCount = 0;
            for (Archetype* archetype : m_archetypes)
            {
                quickCount += archetype->GetEntityCount();
            }

            if (quickCount < MIN_ENTITIES_QUICK_CHECK)
            {
                return ForEachSince(since, adapted);
            }

            // No scheduler injected: Astra spawns no threads — run sequentially inline.
            if (!m_scheduler)
                return ForEachSince(since, adapted);

            std::vector<std::pair<Archetype*, size_t>> chunkWork;
            // Better estimation based on typical entities per 16KB chunk
            size_t estimatedChunks = (quickCount / AVG_ENTITIES_PER_CHUNK) + m_archetypes.size();
            chunkWork.reserve(estimatedChunks);
            size_t totalMatchingEntities = 0;
            
            for (Archetype* archetype : m_archetypes)
            {
                size_t chunkCount = archetype->GetChunkCount();
                for (size_t i = 0; i < chunkCount; ++i)
                {
                    size_t chunkEntityCount = archetype->GetChunkEntityCount(i);
                    if (chunkEntityCount > 0)
                    {
                        chunkWork.emplace_back(archetype, i);
                        totalMatchingEntities += chunkEntityCount;
                    }
                }
            }
            
            // Fall back to sequential for tiny workloads
            if (chunkWork.empty() || totalMatchingEntities < MIN_ENTITIES_FOR_PARALLEL || chunkWork.size() < MIN_CHUNKS_FOR_PARALLEL)
            {
                return ForEachSince(since, adapted);
            }

            m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
                [&](size_t begin, size_t end, uint32_t /*worker*/)
                {
                    for (size_t w = begin; w < end; ++w)
                    {
                        auto [archetype, chunkIndex] = chunkWork[w];
                        ParallelForEachChunkImpl(archetype, chunkIndex, adapted, since, RequiredTypes{}, OptionalTypes{});
                    }
                });
        }

    public:
        /**
         * Like ParallelForEach, but threads a per-chunk sub-context to the body
         * (Theme B2 Phase B, Task 3 -- the machinery behind
         * SystemContext::ParallelForEach). For each unit of chunk work at FLAT
         * chunkWork index `w`, builds `auto sub = factory(w);` and invokes
         * `body(entity, components..., sub)` for every entity in that chunk. The
         * factory runs ON the worker executing the chunk, so a sub-context whose
         * recorder is a per-thread CommandBuffer records into THAT worker's own
         * buffer.
         *
         * DETERMINISM (critical): the factory argument is the FLAT chunkWork
         * index `w`, NOT the per-archetype chunk index (chunkWork[w].second). In
         * a multi-archetype view chunk 0 of archetype A and chunk 0 of archetype
         * B share the per-archetype index 0, so stamping that would give two
         * chunks the same {insertionOrder, 0, ...} key -- and since each
         * sub-context restarts its recordSequence at 0, their commands would
         * collide and the flush's stable-sort tiebreak would be non-
         * deterministic. The flat `w` is globally unique across the whole view
         * AND deterministic (chunkWork is built by iterating the
         * deterministically-sorted archetypes x their chunks in order). The
         * per-archetype chunk index is still what selects the actual chunk.
         *
         * Additive sibling of ParallelForEach: REUSES ParallelForEachChunkImpl
         * for the chunk walk (both the no-optional ForEachChunk path and the
         * optional InvokeEntityCallback path invoke the callback as
         * `(entity, component-refs..., [optional ptrs...])`, which the wrapper
         * adapts by appending `sub`). Does NOT modify ParallelForEach itself.
         *
         * Unlike ParallelForEach, the null-scheduler / below-threshold case does
         * NOT delegate to ForEach (which has neither a per-chunk sub-context nor
         * a chunk index): it builds chunkWork unconditionally and walks it INLINE
         * IN FLAT ORDER, which is deterministic. The per-chunk work is factored
         * into one `runChunk(w)` local used by both the scheduler-dispatch and
         * the inline path so the two can't drift.
         *
         * RETURNS the number of chunk-work items processed (chunkWork.size()) --
         * i.e. the count of distinct flat indices `w` in [0, chunkWork.size())
         * the factory could have been called with, IDENTICAL on the scheduler-
         * dispatch and inline paths (both process the whole chunkWork). Every
         * early-return path returns 0. SystemContext::ParallelForEach uses this
         * to advance its per-scope iterationIndex band by exactly the number of
         * distinct iterationIndex values this call could have stamped, keeping
         * deferred-command SortKeys globally unique across sequential calls.
         * Callers that ignore the return value are unaffected (additive).
         *
         * Change detection (spec §3.4): a change-filtered view needs a "since"
         * tick -- SystemContext::ParallelForEach calls the Tick-taking overload
         * below; this one is compile-time refused on such a view.
         */
        template<typename Factory, typename Body>
        ASTRA_FORCEINLINE size_t ParallelForEachWithContext(Factory&& factory, Body&& body)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ParallelForEachWithContext(since, factory, body) (SystemContext::ParallelForEach does).");
            return ParallelForEachWithContext(Tick{0}, std::forward<Factory>(factory), std::forward<Body>(body));
        }

        /**
         * ParallelForEachWithContext with an explicit "since" tick for the view's
         * Changed<T>/Added<T> terms (ignored by an unfiltered view). Same contract
         * and return value as the two-argument form above.
         */
        template<typename Factory, typename Body>
        ASTRA_FORCEINLINE size_t ParallelForEachWithContext(Tick since, Factory&& factory, Body&& body)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed

            EnsureArchetypes();
            if (UsesStoragePath())
                ResolveStores();     // on this thread, before any worker reads m_stores

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return 0;

            size_t quickCount = 0;
            for (Archetype* archetype : m_archetypes)
            {
                quickCount += archetype->GetEntityCount();
            }

            // Build the chunk work list unconditionally (see method doc): both
            // the parallel-dispatch and the inline fallback path need it, and
            // the per-archetype chunk index it carries.
            std::vector<std::pair<Archetype*, size_t>> chunkWork;
            size_t estimatedChunks = (quickCount / AVG_ENTITIES_PER_CHUNK) + m_archetypes.size();
            chunkWork.reserve(estimatedChunks);
            size_t totalMatchingEntities = 0;

            for (Archetype* archetype : m_archetypes)
            {
                size_t chunkCount = archetype->GetChunkCount();
                for (size_t i = 0; i < chunkCount; ++i)
                {
                    size_t chunkEntityCount = archetype->GetChunkEntityCount(i);
                    if (chunkEntityCount > 0)
                    {
                        chunkWork.emplace_back(archetype, i);
                        totalMatchingEntities += chunkEntityCount;
                    }
                }
            }

            if (chunkWork.empty()) ASTRA_UNLIKELY
                return 0;

            // Per-chunk work, shared by the scheduler-dispatch and inline paths
            // so they can't drift. `w` is the FLAT chunkWork index -- the
            // iterationIndex stamped into the sub-context; chunkWork[w].second is
            // the per-archetype chunk index selecting the actual chunk.
            auto runChunk = [&](size_t w)
            {
                auto [archetype, chunkIndex] = chunkWork[w];
                auto sub = factory(static_cast<uint32_t>(w));
                auto wrapped = [&body, &sub](Astra::Entity e, auto&&... comps)
                {
                    body(e, std::forward<decltype(comps)>(comps)..., sub);
                };
                ParallelForEachChunkImpl(archetype, chunkIndex, wrapped, since, RequiredTypes{}, OptionalTypes{});
            };

            // No scheduler, or workload below the parallel thresholds: walk every
            // chunk inline, in flat order -- deterministic, no thread fan-out.
            if (!m_scheduler ||
                quickCount < MIN_ENTITIES_QUICK_CHECK ||
                totalMatchingEntities < MIN_ENTITIES_FOR_PARALLEL ||
                chunkWork.size() < MIN_CHUNKS_FOR_PARALLEL)
            {
                for (size_t w = 0; w < chunkWork.size(); ++w)
                {
                    runChunk(w);
                }
                return chunkWork.size();
            }

            m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
                [&](size_t begin, size_t end, uint32_t /*worker*/)
                {
                    for (size_t w = begin; w < end; ++w)
                    {
                        runChunk(w);
                    }
                });
            return chunkWork.size();
        }

        ASTRA_NODISCARD size_t Size() noexcept
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed

            EnsureArchetypes();

            size_t total = 0;
            if constexpr (!HasRequiredFilter && !HasSparseArg)
            {
                // No required enableable filter: entity count is unaffected by
                // disabled bits (optional filtering never removes entities), so
                // this is the pre-existing sum (invariant 1).
                for (Archetype* archetype : m_archetypes)
                {
                    total += archetype->GetEntityCount();
                }
            }
            else
            {
                if (UsesStoragePath())
                    ResolveStores();
                for (Archetype* archetype : m_archetypes)
                {
                    total += SizeFiltered(archetype);
                }
            }
            return total;
        }

        ASTRA_NODISCARD bool Empty() noexcept
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            return Size() == 0;
        }

        ASTRA_NODISCARD bool Contains(Entity e) const
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            return VisibleRecord(e) != nullptr;
        }

        // ============= Range-based for loop support =============

        /**
         * Iterator type for range-based for loops.
         * Uses the required components from the query.
         */
        using Iterator = ViewIteratorFromTuple_t<RequiredTypes>;

        /**
         * Begin iterator for range-based for loop.
         * Ensures archetypes are up-to-date before returning iterator.
         */
        ASTRA_FORCEINLINE Iterator begin()
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            // Ruling I: ViewIterator yields plain T&, which cannot mark a change-
            // tracked entity's `changed` tick -- every write through range-for would
            // be an exact-tier false negative. Refuse at compile time (same precedent
            // as the enableable filter below); teaching the iterator to yield Mut<T>
            // is a recorded follow-up.
            static_assert(!HasTrackedYield,
                "range-for over a change-tracked component requested non-const is not supported: the iterator yields T& "
                "and cannot mark the entity's changed tick. Use ForEach(fn) (yields Mut<T>), or request `const T` for a read-only walk.");
            // Range-based for cannot honor the enableable disabled-bit filter:
            // ViewIterator has no access to the per-chunk disabled words, so
            // `for (auto x : view)` would visit disabled entities that
            // ForEach()/Size() skip -- the two surfaces would silently disagree
            // (IM-7). Refuse at compile time when a REQUIRED enableable filter is
            // active; the body of this non-template member is only instantiated
            // when begin() is actually used, so ForEach()/Size()-only usage of
            // such a View still compiles. Optional-only enableable filters are
            // NOT gated: range-for never yields optional components and the
            // visited entity set is identical to ForEach()'s, so they do not
            // diverge.
            static_assert(!HasRequiredFilter,
                "Range-based for over a View with a required enableable-component filter is not "
                "supported: it would bypass disabled-bit filtering and disagree with ForEach()/Size(). "
                "Use ForEach() or ParallelForEach() instead.");

            if (!m_archetypeManager) ASTRA_UNLIKELY
                return Iterator{};   // empty iterator: no archetypes, enters (and stamps) no chunk

            EnsureArchetypes();
            // The iterator carries the current tick so it can apply the coarse
            // change-detection stamp on every chunk it enters (non-const yields only).
            return Iterator(m_archetypes.data(), m_archetypes.size(), m_archetypeManager->CurrentTick());
        }

        /**
         * End sentinel for range-based for loop.
         */
        ASTRA_FORCEINLINE ViewSentinel end() const noexcept
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            // See begin() (Ruling I): the iterator cannot mark a tracked entity.
            static_assert(!HasTrackedYield,
                "range-for over a change-tracked component requested non-const is not supported: the iterator yields T& "
                "and cannot mark the entity's changed tick. Use ForEach(fn) (yields Mut<T>), or request `const T` for a read-only walk.");
            // See begin(): range-for is compile-time refused on required
            // enableable-filtered views so it cannot silently diverge from
            // ForEach()/Size() (IM-7).
            static_assert(!HasRequiredFilter,
                "Range-based for over a View with a required enableable-component filter is not "
                "supported: it would bypass disabled-bit filtering and disagree with ForEach()/Size(). "
                "Use ForEach() or ParallelForEach() instead.");
            return ViewSentinel{};
        }

    private:
        // Random-access yield shape: each required -> a pointer (const preserved),
        // or a by-value Mut<R> for a non-const change-tracked R; each optional -> a
        // pointer. Mirrors ForEach's yielded arguments.
        template<typename R>
        using AccessElement = std::conditional_t<Detail::IsMutableYield<R> && IsChangeTrackedV<R>, Mut<R>, R*>;

        template<typename ReqTuple, typename OptTuple> struct AccessTupleImpl;
        template<typename... R, typename... O>
        struct AccessTupleImpl<std::tuple<R...>, std::tuple<O...>>
        {
            using type = std::tuple<AccessElement<R>..., O*...>;
        };

        template<typename R>
        ASTRA_FORCEINLINE AccessElement<R> BindRequired(const EntityRecord* rec) const
        {
            using Bare = std::remove_const_t<R>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
            {
                // Handing out the Mut does not mark; its first mutable use does, with
                // the tick current at this call.
                const int col = rec->archetype->GetColumnMeta().idToColumn[TypeID<Bare>::Value()];
                return Mut<R>(rec->archetype->template GetComponent<Bare>(rec->location),
                              rec->chunk->GetTicks(col) + rec->location.GetEntityIndex(),
                              m_archetypeManager->CurrentTick());
            }
            else
            {
                return rec->archetype->template GetComponent<Bare>(rec->location);   // Bare* -> R* (adds const if any)
            }
        }
        template<typename O>
        ASTRA_FORCEINLINE O* BindOptional(const EntityRecord* rec) const
        {
            using Bare = std::remove_const_t<O>;
            if (!rec->archetype->template HasComponent<Bare>())
                return nullptr;
            if constexpr (IsEnableableV<Bare>)
            {
                const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
                if (rec->chunk->IsDisabled(cm.idToColumn[TypeID<Bare>::Value()], rec->location.GetEntityIndex()))
                    return nullptr;
            }
            if constexpr (Detail::IsMutableYield<O> && IsChangeTrackedV<O>)
            {
                // Plan deviation 5: a present non-const tracked optional is a raw O*,
                // so it marks the entity unconditionally (see MarkTrackedOptional).
                const int col = rec->archetype->GetColumnMeta().idToColumn[TypeID<Bare>::Value()];
                rec->chunk->GetTicks(col)[rec->location.GetEntityIndex()].changed = m_archetypeManager->CurrentTick();
            }
            return rec->archetype->template GetComponent<Bare>(rec->location);
        }

        template<typename... R, typename... O, size_t... Ri, size_t... Oi>
        ASTRA_FORCEINLINE auto MakeAccessTuple(const EntityRecord* rec,
                                               std::tuple<R...>, std::tuple<O...>,
                                               std::index_sequence<Ri...>, std::index_sequence<Oi...>) const
        {
            return typename AccessTupleImpl<RequiredTypes, OptionalTypes>::type{
                BindRequired<std::tuple_element_t<Ri, RequiredTypes>>(rec)...,
                BindOptional<std::tuple_element_t<Oi, OptionalTypes>>(rec)...
            };
        }

        // Get()'s stamp: the optional-presence array is built from the entity's own
        // archetype, then the shared per-chunk stamp is applied to its chunk.
        template<size_t... Oi>
        ASTRA_FORCEINLINE void StampForGet(const EntityRecord* rec, std::index_sequence<Oi...> optSeq) const
        {
            std::array<bool, sizeof...(Oi)> hasOptional =
            {
                rec->archetype->template HasComponent<std::tuple_element_t<Oi, OptionalTypes>>()...
            };
            StampChunkForYields(rec->chunk, rec->archetype->GetColumnMeta(), hasOptional, m_archetypeManager->CurrentTick(),
                                std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{}, optSeq);
        }

    public:
        using AccessTuple = typename AccessTupleImpl<RequiredTypes, OptionalTypes>::type;

        /**
         * Filter-aware random access: returns pointers to the yielded components for
         * `e` (required -> T*, or Mut<T> for a non-const change-tracked T; Optional ->
         * T*), or Err(NotMatched) if `e` is absent, dead, filtered out, or disabled
         * under this view's enabled filter. The pointers point INTO live chunk storage
         * and are invalidated by any structural change (create/destroy/add/remove/
         * defragment) — do not retain them across one. In-place value edits through
         * the pointers are fine.
         *
         * Change detection: a NON-CONST requested component stamps the entity's chunk
         * (that column's coarse version) on the way out, even on a `const View` -- and
         * a change-tracked one is handed out as Mut<T>, which marks the entity on use.
         * The Mut<T> carries the tick current when Get() ran: a handle retained across
         * AdvanceTick() marks with that older tick, so do not keep one across frames
         * (mark later writes with Registry::Modified<T>). All-const requests stamp
         * nothing.
         */
        ASTRA_NODISCARD Result<AccessTuple, QueryError> Get(Entity e) const
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            const EntityRecord* rec = VisibleRecord(e);
            if (!rec) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::NotMatched);
            // Coarse change-detection stamp (spec §3.3): a non-const request is a
            // write, so stamp the entity's chunk for every non-const yielded column
            // that is present. All-const request: compiles to nothing.
            StampForGet(rec, std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{});
            return Result<AccessTuple, QueryError>::Ok(
                MakeAccessTuple(rec, RequiredTypes{}, OptionalTypes{},
                                std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{},
                                std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{}));
        }

        /**
         * Filter-aware exactly-one-match accessor: Err(Empty) if no entity
         * matches this view, Err(MultipleMatched) if more than one does,
         * otherwise Ok(Get(the one match)). Non-const because it reuses the
         * ForEach walk (which calls EnsureArchetypes()).
         *
         * Change detection (spec §3.3 row 2, Ruling E): a non-const requested
         * component stamps the returned entity's chunk (and a change-tracked one
         * comes back as Mut<T>, marking on use) -- exactly Get()'s behaviour, because
         * the one match is materialised through Get(). Only the RETURNED entity's
         * chunk is stamped; the count/locate walk below runs UNSTAMPED
         * (ForEachBody<false>) because counting is not a write -- so nothing is
         * marked on the Empty / MultipleMatched paths, and no chunk other than
         * the returned entity's is marked on success.
         */
        ASTRA_NODISCARD Result<AccessTuple, QueryError> Single()
        {
            static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");
            Entity found{};
            size_t count = 0;
            // Tick{0} is inert: Single() is refused above on change-filtered views.
            ForEachBody<false>(Tick{0}, [&](Entity e, auto&&...) { if (count == 0) found = e; ++count; });
            if (count == 0) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::Empty);
            if (count > 1) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::MultipleMatched);
            return Get(found);   // exactly one visible match; Get re-validates, materializes AND stamps its chunk
        }

    private:
        // The record iff `e` is alive AND structurally matches this view AND is
        // enabled-visible; else nullptr. No EnsureArchetypes needed — matching is
        // tested against the entity's OWN archetype mask.
        ASTRA_NODISCARD const EntityRecord* VisibleRecord(Entity e) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY return nullptr;
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
            if (!rec) return nullptr;   // dead/absent (GetEntityRecord checks version + archetype)
            if (!MatchesArchetype(rec->archetype->GetMask())) return nullptr;
            if constexpr (HasRequiredFilter)
            {
                if (!EnabledVisible(rec)) return nullptr;
            }
            return rec;
        }

        // False iff any required enableable component is DISABLED for this entity.
        // Only instantiated when HasRequiredFilter (guarded at the call site).
        ASTRA_NODISCARD bool EnabledVisible(const EntityRecord* rec) const
        {
            return EnabledVisibleImpl(rec, EnabledRequiredFilter{});
        }
        template<typename... Fs>
        ASTRA_NODISCARD bool EnabledVisibleImpl(const EntityRecord* rec, std::tuple<Fs...>) const
        {
            const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
            const size_t idx = rec->location.GetEntityIndex();
            bool disabled = false;
            ((disabled = disabled || rec->chunk->IsDisabled(cm.idToColumn[TypeID<Fs>::Value()], idx)), ...);
            return !disabled;
        }

        // Effective kind of one named type, from masks only: Universal iff the registry
        // declared it so (ComponentStorage::IsDeclaredUniversal tests the declaration mask
        // and reads no descriptor, so it is valid before T is registered); otherwise the
        // trait decides.
        template<typename T>
        StorageKind ResolveKind(ComponentStorage* storage) const
        {
            if (storage && storage->HasUniversals() && storage->IsDeclaredUniversal(TypeID<T>::Value()))
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
            [&]<typename... Ts>(std::tuple<Ts...>*)
            {
                ((m_kinds[PlanIndex<Ts>] = ResolveKind<Ts>(storage)), ...);
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

        struct ArchetypeEntityCountComparator
        {
            bool operator()(Archetype* a, Archetype* b) const
            {
                return a->GetEntityCount() > b->GetEntityCount();
            }
        };

        void EnsureArchetypes()
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            uint32_t currentCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            if (m_lastRefreshCounter == currentCounter)
            {
                return;
            }

            uint32_t removalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            if (m_lastGeneration == 0 || removalCounter != m_lastRemovalCounter)
            {
                // Archetypes were removed (or first refresh): cached pointers
                // may be stale — rebuild the whole list.
                CollectArchetypes();
                m_lastRemovalCounter = removalCounter;
            }
            else
            {
                auto newArchetypes = m_archetypeManager->GetArchetypesSince(m_lastGeneration);
                for (Archetype* arch : newArchetypes)
                {
                    if (MatchesArchetype(arch->GetMask()))
                    {
                        m_archetypes.push_back(arch);
                    }
                }
                std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
            }

            m_lastRefreshCounter = currentCounter;
            m_lastGeneration = m_archetypeManager->m_generation;
        }

        void CollectArchetypes()
        {
            m_archetypes.clear();
            if (!m_archetypeManager) ASTRA_UNLIKELY
            {
                return;  // Registry destroyed
            }

            auto archetypes = m_archetypeManager->GetArchetypes();

            m_archetypes.reserve(archetypes.size());

            for (Archetype* archetype : archetypes)
            {
                // NOTE: empty archetypes are deliberately KEPT — they may gain
                // entities later without any archetype-creation event, and
                // iterating an empty archetype is free (zero-count chunks).
                if (archetype->GetComponentCount() < m_matchCount) ASTRA_UNLIKELY
                {
                    continue;
                }
                if (MatchesArchetype(archetype->GetMask()))
                {
                    m_archetypes.push_back(archetype);
                }
            }

            std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
        }

        // Wrap a user callback so it can be invoked as (Entity, Comps&...): if the
        // callback also accepts the leading Entity, forward it; otherwise drop it.
        // Additive: an existing (Entity, Comps...) body hits the first branch and is
        // called identically. Resolves once per fixed component pack (per view type).
        template<typename Func>
        ASTRA_FORCEINLINE static auto MakeEntityOptionalAdapter(Func&& func)
        {
            return [&func](Astra::Entity e, auto&&... comps)
            {
                if constexpr (std::is_invocable_v<Func&, Astra::Entity, decltype(comps)...>)
                    func(e, std::forward<decltype(comps)>(comps)...);
                else if constexpr (std::is_invocable_v<Func&, decltype(comps)...>)
                    func(std::forward<decltype(comps)>(comps)...);
                else
                    static_assert(sizeof(Func) == 0,
                        "View callback must be invocable as (Entity, Comps&...) or (Comps&...). "
                        "Component params must be 'T&' (write) or 'const T&' (read); "
                        "Optional<T> supplies a 'T*' argument. "
                        "A change-tracked non-const component is yielded as Mut<T> (converts to T&; use .Read()/.Write()/.SetIfNeq()).");
            };
        }

        // Coarse change-detection stamp for a chunk this view is about to iterate:
        // every non-const required column, plus every non-const optional column that
        // is PRESENT on this archetype. Compiles to nothing for an all-const view:
        // the if constexpr gate below is the structural guarantee (no stamp code is
        // instantiated), same shape as the Archetype fast path's Stamp && AnyMutable.
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void StampChunkForYields(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                   const std::array<bool, sizeof...(OptIs)>& hasOptional, Tick now,
                                                   std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>) const
        {
            constexpr bool AnyMutable = (Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>> || ...)
                                     || (Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> || ...);
            if constexpr (AnyMutable)
            {
                ((Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>>
                    ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>>::Value()], now)
                    : void()), ...);
                (((Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> && hasOptional[OptIs])
                    ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>>::Value()], now)
                    : void()), ...);
            }
        }

        // The one archetype walk behind ForEach (Stamp=true) and the internal
        // unstamped pass (Stamp=false: Single()'s count/locate, which is not a
        // write -- Ruling E / spec §3.3 row 2). Mirrors Archetype::ForEachBody so the
        // stamped and unstamped walks cannot drift; with Stamp=false no stamp code
        // is instantiated anywhere below. `since` is the change-filter tick
        // (Changed<T>/Added<T> chunk reject); ignored by a view without one.
        template<bool Stamp, typename Func>
        ASTRA_FORCEINLINE void ForEachBody(Tick since, Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            EnsureArchetypes();
            if (UsesStoragePath())
                ResolveStores();     // before the Debug counter capture and before any chunk

#ifdef ASTRA_BUILD_DEBUG
            // Captured AFTER EnsureArchetypes() so its own (legitimate) refresh
            // of the counter is never mistaken for an in-loop structural change.
            const uint32_t debugStartStructuralChangeCounter =
                m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            // Sparse adds/removes never touch the archetype set, so they have their own
            // counter (Task 16); a sparse change inside the walk is reported like a table one.
            const uint32_t debugStartSparseCounter = m_archetypeManager->GetSparseStructuralCounter();
#endif

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;

            auto adapted = MakeEntityOptionalAdapter(func);
            for (Archetype* archetype : m_archetypes)
            {
                ForEachImpl<Stamp>(archetype, adapted, since, RequiredTypes{}, OptionalTypes{});
            }

#ifdef ASTRA_BUILD_DEBUG
            ASTRA_ASSERT(m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire) == debugStartStructuralChangeCounter &&
                         m_archetypeManager->GetSparseStructuralCounter() == debugStartSparseCounter,
                "Structural mutation (create/destroy entity, add/remove component) detected during View::ForEach. "
                "Defer structural changes into a CommandBuffer and call Execute() after the loop.");
#endif
        }

        template<bool Stamp, typename Func, typename... Required, typename... Optional>
        ASTRA_FORCEINLINE void ForEachImpl(Archetype* archetype, Func&& func, Tick since, std::tuple<Required...>, std::tuple<Optional...>)
        {
            if constexpr (!HasChunkFilter)
            {
                // No enableable (non-IncludeDisabled) type and no Changed/Added term
                // in the query: the filtered path below is not instantiated at all,
                // so this is the pre-existing loop (invariant 1) plus, when Stamp,
                // the coarse change-detection stamp of every non-const yielded
                // column per visited chunk (all-const: no stamp code). A tracked
                // non-const yield (Mut<T>) needs the view-owned loop below, which
                // already handles zero optionals.
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
                (void)since;
                if constexpr (sizeof...(Optional) == 0 && !HasTrackedYield)
                {
                    if constexpr (Stamp)
                        archetype->ForEachStamped<Required...>(std::forward<Func>(func));
                    else
                        archetype->ForEach<Required...>(std::forward<Func>(func));
                }
                else
                {
                    ForEachWithOptional<Stamp, Required..., Optional...>(archetype, std::forward<Func>(func), std::make_index_sequence<sizeof...(Required)>{}, std::make_index_sequence<sizeof...(Optional)>{});
                }
            }
            else
            {
                const Tick now = m_archetypeManager->CurrentTick();
                const auto& chunks = archetype->GetChunks();
                for (auto& chunk : chunks)
                {
                    VisitChunkFiltered<Stamp>(archetype, chunk.get(), func, now, since,
                                              std::make_index_sequence<sizeof...(Required)>{},
                                              std::make_index_sequence<sizeof...(Optional)>{});
                }
            }
        }
        
        template<bool Stamp, typename... Components, typename Func, size_t... RequiredTs, size_t... OptionalTs>
        ASTRA_FORCEINLINE void ForEachWithOptional(Archetype* archetype, Func&& func, std::index_sequence<RequiredTs...>, std::index_sequence<OptionalTs...>)
        {
            constexpr size_t OptionalCount = sizeof...(OptionalTs);
            std::array<bool, OptionalCount> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptionalTs, OptionalTypes>>()...
            };

            [[maybe_unused]] const Tick now = m_archetypeManager->CurrentTick();
            [[maybe_unused]] const auto& cm = archetype->GetColumnMeta();
            const auto& chunks = archetype->GetChunks();

            for (auto& chunk : chunks)
            {
                size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                {
                    continue;
                }

                if constexpr (Stamp)
                    StampChunkForYields(chunk.get(), cm, hasOptional, now,
                                        std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

                std::tuple<std::tuple_element_t<RequiredTs, RequiredTypes>*...> requiredPtrs =
                {
                    chunk->GetComponentArray<std::tuple_element_t<RequiredTs, RequiredTypes>>()...
                };
                std::tuple<std::tuple_element_t<OptionalTs, OptionalTypes>*...> optionalPtrs =
                {
                    (hasOptional[OptionalTs] ? chunk->GetComponentArray<std::tuple_element_t<OptionalTs, OptionalTypes>>() : nullptr)...
                };

                // Tick columns for Mut<T> / optional marks (tracked yields only).
                [[maybe_unused]] EntityTicks* reqTicks[sizeof...(RequiredTs) == 0 ? 1 : sizeof...(RequiredTs)] = {};
                [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptionalTs) == 0 ? 1 : sizeof...(OptionalTs)] = {};
                if constexpr (HasTrackedYield)
                    ResolveTrackedTicks(chunk.get(), cm, hasOptional, reqTicks, optTicks,
                                        std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

                const auto& entities = chunk->GetEntities();

                InvokeEntityCallback<Stamp>(entities, requiredPtrs, optionalPtrs, count, std::forward<Func>(func), std::make_index_sequence<sizeof...(RequiredTs)>{}, std::make_index_sequence<sizeof...(OptionalTs)>{},
                                            reqTicks, optTicks, now);
            }
        }

        template<typename Func, typename... Required, typename... Optional>
        ASTRA_FORCEINLINE void ParallelForEachChunkImpl(Archetype* archetype, size_t chunkIndex, Func&& func, Tick since, std::tuple<Required...>, std::tuple<Optional...>)
        {
            if constexpr (!HasChunkFilter)
            {
                // Pre-existing chunk walk (invariant 1) plus the coarse change-
                // detection stamp of every non-const yielded column for this chunk.
                // A tracked non-const yield (Mut<T>) takes the view-owned loop.
                if (m_anyUniversal) ASTRA_UNLIKELY
                {
                    // Interim until Task 28's parallel driver: a Universal member has no
                    // chunk column, so route the one chunk through the storage walk.
                    const auto& uchunks = archetype->GetChunks();
                    if (chunkIndex < uchunks.size())
                        VisitChunkStorage<true>(archetype, uchunks[chunkIndex].get(), func, m_archetypeManager->CurrentTick(), since,
                                                std::make_index_sequence<sizeof...(Required)>{},
                                                std::make_index_sequence<sizeof...(Optional)>{});
                    return;
                }
                (void)since;
                if constexpr (sizeof...(Optional) == 0 && !HasTrackedYield)
                {
                    archetype->ForEachChunkStamped<Required...>(chunkIndex, std::forward<Func>(func));
                }
                else
                {
                    ParallelForEachChunkWithOptional<Required..., Optional...>(archetype, chunkIndex, std::forward<Func>(func), std::make_index_sequence<sizeof...(Required)>{}, std::make_index_sequence<sizeof...(Optional)>{});
                }
            }
            else
            {
                // Same per-chunk three-tier filter as the serial path; partitioning
                // (which chunk this worker got) is unchanged.
                const auto& chunks = archetype->GetChunks();
                if (chunkIndex >= chunks.size()) ASTRA_UNLIKELY
                    return;
                VisitChunkFiltered<true>(archetype, chunks[chunkIndex].get(), func, m_archetypeManager->CurrentTick(), since,
                                         std::make_index_sequence<sizeof...(Required)>{},
                                         std::make_index_sequence<sizeof...(Optional)>{});
            }
        }
        
        template<typename... Components, typename Func, size_t... RequiredTs, size_t... OptionalTs>
        ASTRA_FORCEINLINE void ParallelForEachChunkWithOptional(Archetype* archetype, size_t chunkIndex, Func&& func, std::index_sequence<RequiredTs...>, std::index_sequence<OptionalTs...>)
        {
            constexpr size_t OptionalCount = sizeof...(OptionalTs);
            std::array<bool, OptionalCount> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptionalTs, OptionalTypes>>()...
            };
            
            const auto& chunks = archetype->GetChunks();
            if (chunkIndex >= chunks.size()) ASTRA_UNLIKELY
                return;

            auto& chunk = chunks[chunkIndex];
            size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;

            // One tick read per chunk: the coarse stamp and the Mut<T> marks agree.
            [[maybe_unused]] const Tick now = m_archetypeManager->CurrentTick();
            [[maybe_unused]] const auto& cm = archetype->GetColumnMeta();

            StampChunkForYields(chunk.get(), cm, hasOptional, now,
                                std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

            std::tuple<std::tuple_element_t<RequiredTs, RequiredTypes>*...> requiredPtrs =
            {
                chunk->GetComponentArray<std::tuple_element_t<RequiredTs, RequiredTypes>>()...
            };
            std::tuple<std::tuple_element_t<OptionalTs, OptionalTypes>*...> optionalPtrs =
            {
                (hasOptional[OptionalTs] ? chunk->GetComponentArray<std::tuple_element_t<OptionalTs, OptionalTypes>>() : nullptr)...
            };

            // Tick columns for Mut<T> / optional marks (tracked yields only).
            [[maybe_unused]] EntityTicks* reqTicks[sizeof...(RequiredTs) == 0 ? 1 : sizeof...(RequiredTs)] = {};
            [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptionalTs) == 0 ? 1 : sizeof...(OptionalTs)] = {};
            if constexpr (HasTrackedYield)
                ResolveTrackedTicks(chunk.get(), cm, hasOptional, reqTicks, optTicks,
                                    std::index_sequence<RequiredTs...>{}, std::index_sequence<OptionalTs...>{});

            const auto& entities = chunk->GetEntities();

            InvokeEntityCallback<true>(entities, requiredPtrs, optionalPtrs, count, std::forward<Func>(func), std::make_index_sequence<sizeof...(RequiredTs)>{}, std::make_index_sequence<sizeof...(OptionalTs)>{},
                                       reqTicks, optTicks, now);
        }

        // Empty (tag) required components have no storage, so
        // chunk->GetComponentArray<T>() returns nullptr for them. Indexing that
        // raw pointer (`ptr[i]`) would bind a reference to a null-derived address
        // (UB; UBSan traps). Mirror the plain path's Archetype::GetComponentValue
        // and hand out a shared static instance instead. For non-empty components
        // this force-inlines to the identical `array[index]`, so the hot path is
        // unchanged.
        template<typename T>
        ASTRA_FORCEINLINE static auto& RequiredElement(T* array, size_t index) noexcept
        {
            if constexpr (std::is_empty_v<T>)
            {
                static T s_emptyInstance{};
                return s_emptyInstance;
            }
            else
            {
                return array[index];
            }
        }

        // ============ Change-tracked yield (spec §3.3, instantiated only when HasTrackedYield) ============
        // Everything below the untracked branch of the two invoke helpers exists ONLY
        // for a view that yields a non-const change-tracked component; every other
        // view instantiates the pre-existing loop body byte for byte.

        // Yield shape of one required component: Mut<R> for a non-const change-
        // tracked R, otherwise the plain reference the pre-existing loops hand out.
        template<typename R> struct YieldType { using type = R&; };
        template<typename R> requires (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
        struct YieldType<R> { using type = Mut<R>; };

        // reqTicks[k] is the tick column for required k (nullptr unless tracked).
        template<size_t K, typename ReqTuple>
        ASTRA_FORCEINLINE typename YieldType<std::tuple_element_t<K, RequiredTypes>>::type
        YieldRequired(const ReqTuple& reqPtrs, [[maybe_unused]] EntityTicks* const* reqTicks, size_t i, [[maybe_unused]] Tick now) const noexcept
        {
            using R = std::tuple_element_t<K, RequiredTypes>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
                return Mut<R>(&std::get<K>(reqPtrs)[i], reqTicks[K] + i, now);
            else
                return RequiredElement(std::get<K>(reqPtrs), i);
        }

        // Plan deviation 5: a PRESENT non-const change-tracked optional is yielded as
        // a raw O* (no conversion hook to mark through), so it marks the entity
        // unconditionally. `opt` holds the pointers about to be handed out for entity
        // i -- the chunk base pointers on the unfiltered path, the per-entity pointers
        // on the enabled-filtered path (a disabled entity's pointer is nulled there
        // and is not "present", so it must not mark).
        template<size_t K, typename OptTuple>
        ASTRA_FORCEINLINE static void MarkTrackedOptional([[maybe_unused]] const OptTuple& opt, [[maybe_unused]] EntityTicks* const* optTicks,
                                                          [[maybe_unused]] size_t i, [[maybe_unused]] Tick now) noexcept
        {
            using O = std::tuple_element_t<K, OptionalTypes>;
            if constexpr (Detail::IsMutableYield<O> && IsChangeTrackedV<O>)
            {
                if (std::get<K>(opt)) optTicks[K][i].changed = now;
            }
        }
        template<typename OptTuple, size_t... OptIs>
        ASTRA_FORCEINLINE static void MarkTrackedOptionals([[maybe_unused]] const OptTuple& opt, [[maybe_unused]] EntityTicks* const* optTicks,
                                                           [[maybe_unused]] size_t i, [[maybe_unused]] Tick now, std::index_sequence<OptIs...>) noexcept
        {
            (MarkTrackedOptional<OptIs>(opt, optTicks, i, now), ...);
        }

        // Tick columns for the tracked yields of one chunk: reqTicks[k] / optTicks[k]
        // is the EntityTicks column for required / optional k, nullptr unless that
        // yield is non-const AND change-tracked (AND, for an optional, present on this
        // archetype). Same fold shape as StampChunkForYields; called only under
        // `if constexpr (HasTrackedYield)`.
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE static void ResolveTrackedTicks([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                                          [[maybe_unused]] const std::array<bool, sizeof...(OptIs)>& hasOptional,
                                                          [[maybe_unused]] EntityTicks** reqTicks, [[maybe_unused]] EntityTicks** optTicks,
                                                          std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>) noexcept
        {
            ((reqTicks[ReqIs] = (Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>> && IsChangeTrackedV<std::tuple_element_t<ReqIs, RequiredTypes>>)
                ? chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>>::Value()]) : nullptr), ...);
            ((optTicks[OptIs] = (hasOptional[OptIs] && Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> && IsChangeTrackedV<std::tuple_element_t<OptIs, OptionalTypes>>)
                ? chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>>::Value()]) : nullptr), ...);
        }

        // `Stamp` mirrors the chunk loops' parameter: with Stamp=false (Single()'s
        // count/locate pass -- Ruling E, extended to the exact tier) the deviation-5
        // optional mark is not instantiated either; a Mut<T> handed to the counting
        // lambda is never touched, so required yields need no gate.
        template<bool Stamp, typename EntitiesVec, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeEntityCallback(const EntitiesVec& entities, const ReqTuple& reqPtrs, const OptTuple& optPtrs,
                                                    size_t count, Func&& func, std::index_sequence<ReqIs...>, [[maybe_unused]] std::index_sequence<OptIs...> os,
                                                    [[maybe_unused]] EntityTicks* const* reqTicks = nullptr, [[maybe_unused]] EntityTicks* const* optTicks = nullptr,
                                                    [[maybe_unused]] Tick now = 0)
        {
            if constexpr (!HasTrackedYield)
            {
                // Pre-existing body, byte-identical: every untracked view lands here.
                for (size_t i = 0; i < count; ++i)
                {
                    func(entities[i], RequiredElement(std::get<ReqIs>(reqPtrs), i)..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...);
                }
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    if constexpr (Stamp)
                        MarkTrackedOptionals(optPtrs, optTicks, i, now, os);
                    // Materialise the yields as lvalues (a tuple of T&... / Mut<T>...) so
                    // `auto&`, `T&` (Mut's implicit conversion) and by-value `Mut<T>`
                    // parameters all bind; std::apply hands them over as T& / Mut<T>&.
                    std::tuple<typename YieldType<std::tuple_element_t<ReqIs, RequiredTypes>>::type...> req{ YieldRequired<ReqIs>(reqPtrs, reqTicks, i, now)... };
                    std::apply([&](auto&... r) { func(entities[i], r..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...); }, req);
                }
            }
        }

        // ============ Enableable-components filtered iteration (spec §5) ============
        // Everything below is instantiated ONLY when HasEnabledFilter is true (the
        // callers gate it with `if constexpr`); an unfiltered view never sees it.

        // Resolve one required enableable-filtered column: record its disabled-word
        // pointer and fold its all-enabled / all-disabled state into the flags.
        template<size_t F>
        ASTRA_FORCEINLINE void ResolveOneRequired(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, size_t count,
                                                  const uint64_t** reqWords, bool& allZero, bool& anyFull)
        {
            using T = std::tuple_element_t<F, EnabledRequiredFilter>;
            const int col = cm.idToColumn[TypeID<T>::Value()];  // required => present, enableable => has words
            reqWords[F] = chunk->GetDisabledWords(col);
            const uint32_t dc = chunk->GetDisabledCount(col);
            if (dc == static_cast<uint32_t>(count)) anyFull = true;   // fully disabled => empty intersection
            if (dc != 0) allZero = false;
        }

        // The three pack-fed parameters are [[maybe_unused]]: with an empty Fs
        // pack the fold is a no-op and gcc's -Wunused-but-set-parameter fires.
        template<size_t... Fs>
        ASTRA_FORCEINLINE bool ResolveRequiredFilter([[maybe_unused]] ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                     [[maybe_unused]] size_t count, [[maybe_unused]] const uint64_t** reqWords,
                                                     bool& allZero, std::index_sequence<Fs...>)
        {
            bool anyFull = false;
            allZero = true;
            (ResolveOneRequired<Fs>(chunk, cm, count, reqWords, allZero, anyFull), ...);
            return anyFull;
        }

        // A present, enableable optional with any disabled entity forces the mixed
        // path so its per-entity pointer can be nulled.
        template<size_t K, typename OptTuple>
        ASTRA_FORCEINLINE void CheckOptionalAllEnabled(const OptTuple& optPtrs, ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, bool& allZero)
        {
            using OptT = std::tuple_element_t<K, OptionalTypes>;
            if constexpr (IsEnableableV<OptT>)
            {
                if (std::get<K>(optPtrs) && chunk->GetDisabledCount(cm.idToColumn[TypeID<OptT>::Value()]) != 0)
                    allZero = false;
            }
        }

        // Per-entity optional pointer for the mixed path: null while the entity is
        // disabled in an enableable optional column; otherwise the usual present/
        // absent pointer. The bit test exists ONLY for enableable optionals (the
        // "second if constexpr"), so required-only / non-enableable-optional views
        // pay nothing.
        template<size_t K, typename OptTuple>
        ASTRA_FORCEINLINE auto FilteredOptionalArg(const OptTuple& optPtrs, size_t i, ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm)
        {
            using OptT = std::tuple_element_t<K, OptionalTypes>;
            OptT* base = std::get<K>(optPtrs);
            if constexpr (IsEnableableV<OptT>)
            {
                if (base && chunk->IsDisabled(cm.idToColumn[TypeID<OptT>::Value()], i))
                    return static_cast<OptT*>(nullptr);
            }
            return base ? &base[i] : static_cast<OptT*>(nullptr);
        }

        // Same `Stamp` gate as InvokeEntityCallback (Ruling E, extended).
        template<bool Stamp, typename EntitiesVec, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeEntityCallbackFiltered(const EntitiesVec& entities, const ReqTuple& reqPtrs, const OptTuple& optPtrs,
                                                            size_t begin, size_t end, ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                            Func&& func, std::index_sequence<ReqIs...>, [[maybe_unused]] std::index_sequence<OptIs...> os,
                                                            [[maybe_unused]] EntityTicks* const* reqTicks = nullptr, [[maybe_unused]] EntityTicks* const* optTicks = nullptr,
                                                            [[maybe_unused]] Tick now = 0)
        {
            if constexpr (!HasTrackedYield)
            {
                // Pre-existing body, byte-identical: every untracked view lands here.
                for (size_t i = begin; i < end; ++i)
                {
                    func(entities[i], RequiredElement(std::get<ReqIs>(reqPtrs), i)..., FilteredOptionalArg<OptIs>(optPtrs, i, chunk, cm)...);
                }
            }
            else
            {
                for (size_t i = begin; i < end; ++i)
                {
                    // Per-entity optional pointers first: an enableable tracked optional
                    // that is disabled for this entity is nulled here and must not mark.
                    std::tuple<std::tuple_element_t<OptIs, OptionalTypes>*...> opt{ FilteredOptionalArg<OptIs>(optPtrs, i, chunk, cm)... };
                    if constexpr (Stamp)
                        MarkTrackedOptionals(opt, optTicks, i, now, os);
                    std::tuple<typename YieldType<std::tuple_element_t<ReqIs, RequiredTypes>>::type...> req{ YieldRequired<ReqIs>(reqPtrs, reqTicks, i, now)... };
                    std::apply([&](auto&... r) { func(entities[i], r..., std::get<OptIs>(opt)...); }, req);
                }
            }
        }

        // ============ Change-detection chunk reject (spec §3.4, instantiated only when HasChangeFilter) ============

        // True iff every T in the tuple has a column version newer than `since` in
        // this chunk. Empty tuple => true (the fold's identity), which is why the
        // pack-fed parameters are [[maybe_unused]] (gcc -Wunused-but-set-parameter).
        template<typename... Ts>
        ASTRA_FORCEINLINE static bool AllNewer([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                               [[maybe_unused]] Tick since, std::tuple<Ts...>*) noexcept
        {
            return (IsNewer(chunk->GetColumnVersion(cm.idToColumn[TypeID<std::remove_const_t<Ts>>::Value()]), since) && ...);
        }
        ASTRA_FORCEINLINE static bool ChangeChunkPasses(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick since) noexcept
        {
            return AllNewer(chunk, cm, since, static_cast<ChangedTypes*>(nullptr))
                && AllNewer(chunk, cm, since, static_cast<AddedTypes*>(nullptr));
        }

        // ============ Change-detection per-entity tier (spec §3.4 step 3, instantiated only when HasTrackedChangeTerm) ============

        // Per-entity tier (spec §3.4 step 3): for one tracked term, set the bit of
        // every entity whose tick is NOT newer than `since`. Branchless per slot; the
        // resulting word set is unioned with the enabled-filter sets so the callback
        // is invoked in maximal runs of (enabled AND changed) entities.
        template<bool UseAdded>
        ASTRA_FORCEINLINE static void ExcludeNotNewer(const EntityTicks* ticks, size_t count, Tick since, uint64_t* excluded, bool& anyExcluded) noexcept
        {
            for (size_t i = 0; i < count; ++i)
            {
                const Tick t = UseAdded ? ticks[i].added : ticks[i].changed;
                const uint64_t bit = static_cast<uint64_t>(!IsNewer(t, since));
                excluded[i >> 6] |= bit << (i & 63);
                anyExcluded |= (bit != 0);
            }
        }

        // One excluded-mask pass per change-TRACKED term of the tuple; untracked
        // terms contribute nothing here (the chunk reject already decided them).
        // An empty tuple makes the fold a no-op, hence the [[maybe_unused]]s.
        template<bool UseAdded, typename... Ts>
        ASTRA_FORCEINLINE static void BuildExcluded([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] const ArchetypeColumnMeta& cm,
                                                    [[maybe_unused]] size_t count, [[maybe_unused]] Tick since,
                                                    [[maybe_unused]] uint64_t* excluded, [[maybe_unused]] bool& anyExcluded, std::tuple<Ts...>*) noexcept
        {
            ((IsChangeTrackedV<Ts>
                ? ExcludeNotNewer<UseAdded>(chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<Ts>>::Value()]), count, since, excluded, anyExcluded)
                : void()), ...);
        }

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

        // Pure-Table per-chunk filter for one chunk (shared by the serial and parallel paths):
        // the Changed/Added version reject, then the three-tier enabled-only filter.
        // count==0 chunks are no-ops. `now` is the caller's hoisted CurrentTick()
        // for the coarse change-detection stamp; Stamp=false (the internal count/
        // locate pass) instantiates no stamp at all. `since` is the change-filter
        // tick; a view without Changed/Added terms instantiates no reject code.
        template<bool Stamp, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void VisitChunkTable(Archetype* archetype, ArchetypeChunk* chunk, Func&& func, Tick now, Tick since,
                                                  std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq)
        {
            const size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;

            const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();

            if constexpr (HasChangeFilter)
            {
                // Chunk reject first, always (spec §2.2): a chunk whose T column is not
                // newer than `since` for ANY Changed/Added term has nothing for us.
                // Sits BEFORE the enabled whole-chunk reject and BEFORE the coarse
                // stamp: a rejected chunk is not "visited" and must not be stamped.
                // Unhinted on purpose: in the steady state most chunks are unchanged
                // per frame, so the reject is the common outcome, not the rare one.
                if (!ChangeChunkPasses(chunk, cm, since))
                    return;
            }
            else
                (void)since;

            std::array<bool, sizeof...(OptIs)> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptIs, OptionalTypes>>()...
            };
            std::tuple<std::tuple_element_t<ReqIs, RequiredTypes>*...> reqPtrs =
            {
                chunk->GetComponentArray<std::tuple_element_t<ReqIs, RequiredTypes>>()...
            };
            std::tuple<std::tuple_element_t<OptIs, OptionalTypes>*...> optPtrs =
            {
                (hasOptional[OptIs] ? chunk->GetComponentArray<std::tuple_element_t<OptIs, OptionalTypes>>() : nullptr)...
            };
            const auto& entities = chunk->GetEntities();

            constexpr size_t NReq = std::tuple_size_v<EnabledRequiredFilter>;
            const uint64_t* wordSets[NReq + 1];               // enabled sets + (optional) excluded set
            bool allZero = true;
            const bool anyReqFull = ResolveRequiredFilter(chunk, cm, count, wordSets, allZero, std::make_index_sequence<NReq>{});
            if (anyReqFull) ASTRA_UNLIKELY
                return;   // Tier 2: a required column is fully disabled -> skip whole chunk
            size_t setCount = NReq;

            // Per-entity change tier (spec §3.4 step 3): for every change-TRACKED
            // Changed<T>/Added<T> term, scan the tick column of this (version-passing)
            // chunk into an "excluded" word set (SET bit == entity fails the term) and
            // union it with the enabled sets below. Untracked terms were fully decided
            // by the chunk reject above. Deliberately AFTER the whole-chunk enabled
            // reject (an all-disabled chunk never pays the tick scan) and BEFORE the
            // Tier-1/Tier-3 split. Not a reject: the chunk is still "visited" (Ruling B).
            [[maybe_unused]] SmallVector<uint64_t, 64> excluded;   // 64 inline words = 4096 slots before a heap step
            if constexpr (HasTrackedChangeTerm)
            {
                excluded.assign((count + 63) >> 6, 0ull);
                bool anyExcluded = false;
                BuildExcluded<false>(chunk, cm, count, since, excluded.data(), anyExcluded, static_cast<ChangedTypes*>(nullptr));
                BuildExcluded<true >(chunk, cm, count, since, excluded.data(), anyExcluded, static_cast<AddedTypes*>(nullptr));
                if (anyExcluded)
                {
                    wordSets[setCount++] = excluded.data();
                    allZero = false;                       // a per-entity constraint exists: no Tier-1 whole-chunk call
                }
            }

            // Tick columns for Mut<T> / optional marks (tracked yields only); after
            // both whole-chunk rejects, like the stamp below (Ruling B).
            [[maybe_unused]] EntityTicks* reqTicks[sizeof...(ReqIs) == 0 ? 1 : sizeof...(ReqIs)] = {};
            [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptIs) == 0 ? 1 : sizeof...(OptIs)] = {};
            if constexpr (HasTrackedYield)
                ResolveTrackedTicks(chunk, cm, hasOptional, reqTicks, optTicks, reqSeq, optSeq);

            // Coarse change-detection stamp, deliberately AFTER both whole-chunk
            // rejects above (change version, enabled): a chunk this view skips
            // wholesale is not "visited" and must not be stamped (it would
            // manufacture downstream false positives).
            if constexpr (Stamp)
                StampChunkForYields(chunk, cm, hasOptional, now, reqSeq, optSeq);

            if constexpr (HasOptionalFilter)
            {
                (CheckOptionalAllEnabled<OptIs>(optPtrs, chunk, cm, allZero), ...);
            }

            if (allZero)
            {
                // Tier 1: all relevant columns fully enabled -> pre-existing body, no bit tests.
                InvokeEntityCallback<Stamp>(entities, reqPtrs, optPtrs, count, func, reqSeq, optSeq, reqTicks, optTicks, now);
                return;
            }

            // Tier 3: mixed -> runs of the required-enabled intersection (AND the
            // per-entity change mask when one was appended), per-entity optional
            // nulling inside the runs.
            Detail::ForEachEnabledRun(wordSets, setCount, count,
                [&](size_t begin, size_t end)
                {
                    InvokeEntityCallbackFiltered<Stamp>(entities, reqPtrs, optPtrs, begin, end, chunk, cm, func, reqSeq, optSeq, reqTicks, optTicks, now);
                });
        }

        // ================= Storage path (spec 2026-10-01 s6.3, s6.4) =================
        // ================= Storage path (spec 2026-10-01 s6.3, s6.4) =================
        // Taken by every view naming a Sparse type, and by any view whose registry
        // declares one of its types Universal (UsesStoragePath). A pure-Table view in a
        // Universal-free registry instantiates it (the dispatcher names it) but never
        // runs it: it pays one m_anyUniversal test per archetype walk.

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
            // Reads never create a store (RULINGS G2): a set or column that does not exist
            // yet means no entity holds / has been given the component.
            case StorageKind::Sparse:
                s.slot = storage->SparseSlotOf(id);
                s.sparse = storage->FindSparse(id);       // no slot => no member anywhere
                break;
            case StorageKind::Universal:
                s.universal = storage->FindUniversal(id);
                s.slot = storage->SparseSlotOf(id);       // enableable Universal: its disabled-mirror slot
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
            {
                ASTRA_ASSERT(f.nRequire < MaxWordSets, "View::ChunkFilter: require word-set capacity exceeded");
                f.require[f.nRequire++] = b->present;          // all-present needs no words
            }
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
            ASTRA_ASSERT(f.nExclude < MaxWordSets, "View::ChunkFilter: exclude word-set capacity exceeded");
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
            ASTRA_ASSERT(f.nExclude < MaxWordSets, "View::ChunkFilter: exclude word-set capacity exceeded");
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

        // ---- Fetching (spec s6.4) ----
        // Per-chunk element source of one yielded type; At() is the per-entity fetch.
        // The Table-vs-Sparse choice is made at COMPILE TIME from the trait (a Table
        // type reads base[i], a Sparse type the set's dense element by entity), so
        // neither pays a per-entity kind test. Universal is a per-chunk flag: a type
        // declared Universal in this registry reads its slot-indexed page element
        // through a page cursor (RULINGS Task 24: one loop-invariant branch accepted).
        template<typename T>
        struct Fetch
        {
            using Bare = std::remove_const_t<T>;
            static constexpr bool SparseTrait = StorageOfV<Bare> == Storage::Sparse;

            Bare*            base = nullptr;          // Table: column base in this chunk
            ArchetypeChunk*  chunk = nullptr;         // Table: for the disabled-bit test
            int              col = -1;                // Table: column ordinal, -1 = absent here
            EntityTicks*     ticks = nullptr;         // Table: tick column (tracked only)
            SparseSet*       sparse = nullptr;        // Sparse trait; null while no set exists
            UniversalColumn* universal = nullptr;     // declared Universal here; null while no column exists
            bool             isUniversal = false;     // the type is Universal in this registry
            UniversalColumn::Cursor cursor{};         // Universal page cursor

            ASTRA_FORCEINLINE Bare* At([[maybe_unused]] Entity e, [[maybe_unused]] size_t i) noexcept
            {
                if constexpr (std::is_empty_v<Bare>)
                {
                    static Bare s_emptyInstance{};    // a present tag (RequiredElement's contract)
                    return &s_emptyInstance;
                }
                else
                {
                    if (isUniversal) ASTRA_UNLIKELY
                        return universal ? static_cast<Bare*>(universal->GetCached(e, cursor)) : nullptr;
                    if constexpr (SparseTrait)
                        return sparse ? static_cast<Bare*>(sparse->Get(e)) : nullptr;
                    else
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
                            bool disabled;
                            if (isUniversal)
                                disabled = universal->IsDisabled(e);
                            else if constexpr (SparseTrait)
                                disabled = sparse->IsDisabled(e);
                            else
                                disabled = chunk->IsDisabled(col, i);
                            if (disabled)
                                return nullptr;
                        }
                    }
                    return p;
                }
            }
        };

        template<typename T>
        ASTRA_FORCEINLINE Fetch<T> MakeFetch(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, const StoreArray& st) const noexcept
        {
            using Bare = std::remove_const_t<T>;
            Fetch<T> f;
            const StoreRef& s = st[PlanIndex<Bare>];
            if (m_kinds[PlanIndex<Bare>] == StorageKind::Universal)
            {
                f.isUniversal = true;
                f.universal = s.universal;
            }
            else if constexpr (Fetch<T>::SparseTrait)
            {
                f.sparse = s.sparse;
            }
            else
            {
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

        // ---- Write stamps for Sparse/Universal yields (spec s6.4) ----
        // Mut<T> marks the element's own ticks (TicksFor). The helpers below stamp the
        // sources Registry::IsChanged reads for the NON-tracked and raw-pointer yields.

        // Coarse write stamp of a non-const UNTRACKED Sparse yield: the per-component
        // version IsChanged reads for untracked Sparse (Task 20), once per visited chunk
        // -- the Table column stamp's twin. SHARED LOCATION (Task 28 atomic_ref pass):
        // ComponentStorage's coarse slot for this component.
        // `Gated` (Optional yields): stamp only when this chunk holds the type -- Table
        // parity, where an Optional absent from the archetype has no column to stamp.
        // Required args are already guaranteed present by RequirePresent. A Sparse-trait
        // type declared Universal in this registry is skipped: its coarse version is
        // never read (StoreTickNewer reads the page version).
        template<typename T, bool Gated>
        ASTRA_FORCEINLINE void StampSparseCoarse([[maybe_unused]] ArchetypeChunk* chunk, [[maybe_unused]] ComponentStorage* storage,
                                                 [[maybe_unused]] Tick now) const
        {
            using Bare = std::remove_const_t<T>;
            if constexpr (Detail::IsMutableYield<T> && !IsChangeTrackedV<Bare> && StorageOfV<Bare> == Storage::Sparse)
            {
                if (!storage || m_kinds[PlanIndex<Bare>] != StorageKind::Sparse)
                    return;
                if constexpr (Gated)
                {
                    const StoreRef& s = m_stores[PlanIndex<Bare>];
                    if (s.slot < 0)
                        return;
                    const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(s.slot));
                    if (!b || b->presentCount == 0)
                        return;
                }
                storage->StampCoarse(TypeID<Bare>::Value(), now);
            }
        }

        // The "stamp a visit" helper: everything a visited (not whole-chunk-rejected)
        // chunk stamps once -- the Table column versions and the untracked-Sparse coarse
        // versions. Tasks 28 and 29 call this instead of repeating the folds.
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void StampVisit(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick now,
                                          std::index_sequence<ReqIs...> reqSeq, std::index_sequence<OptIs...> optSeq) const
        {
            StampChunkStorage(chunk, cm, now, reqSeq, optSeq);
            ComponentStorage* storage = m_archetypeManager->GetComponentStorage();
            (StampSparseCoarse<std::tuple_element_t<ReqIs, RequiredTypes>, false>(chunk, storage, now), ...);
            (StampSparseCoarse<std::tuple_element_t<OptIs, OptionalTypes>, true>(chunk, storage, now), ...);
        }

        // Per-entity write stamp of a non-const UNTRACKED Universal required yield: its
        // page version, the per-entity twin of the column stamp. A tracked one is handed
        // out as Mut<T>, which marks on use. SHARED LOCATION (Task 28 atomic_ref pass):
        // the Universal page version.
        template<typename T, typename F>
        ASTRA_FORCEINLINE void StampRequiredStore([[maybe_unused]] F& f, [[maybe_unused]] Entity e, [[maybe_unused]] Tick now) const noexcept
        {
            if constexpr (Detail::IsMutableYield<T> && !IsChangeTrackedV<std::remove_const_t<T>>)
            {
                if (f.isUniversal && f.universal)
                    f.universal->MarkWritten(e, now);
            }
        }

        // A PRESENT non-const Optional (plan deviation 5: a tracked one is yielded as a
        // raw pointer, so it marks unconditionally, for every kind); an untracked
        // Universal one bumps its page (SHARED LOCATION, Task 28). Untracked Sparse:
        // StampSparseCoarse; untracked Table: the chunk column stamp. `present` is the
        // pointer about to be handed out (null when absent or disabled).
        template<typename T, typename F>
        ASTRA_FORCEINLINE void StampOptionalStore([[maybe_unused]] F& f, [[maybe_unused]] const void* present,
                                                  [[maybe_unused]] Entity e, [[maybe_unused]] size_t i,
                                                  [[maybe_unused]] Tick now) const noexcept
        {
            if constexpr (Detail::IsMutableYield<T>)
            {
                using Bare = std::remove_const_t<T>;
                if (!present)
                    return;
                if (f.isUniversal)
                {
                    if (f.universal)
                        f.universal->MarkWritten(e, now);
                }
                else if constexpr (IsChangeTrackedV<Bare>)
                {
                    if constexpr (F::SparseTrait)
                    {
                        if (EntityTicks* t = f.sparse->Ticks(e))
                            t->changed = now;
                    }
                    else if (f.ticks)
                        f.ticks[i].changed = now;
                }
            }
        }

        // Tick slot for a Mut<T> yield, from the element's OWN store: the chunk's tick
        // column for Table, SparseSet::Ticks / UniversalColumn::Ticks otherwise. A tracked
        // type always has one; the scratch slot is only a Release-mode backstop so a
        // broken invariant cannot write through null.
        template<typename T>
        ASTRA_FORCEINLINE EntityTicks* TicksFor(Fetch<T>& f, Entity e, size_t i) const noexcept
        {
            EntityTicks* t = nullptr;
            if (f.isUniversal)
                t = f.universal ? f.universal->Ticks(e) : nullptr;
            else if constexpr (Fetch<T>::SparseTrait)
                t = f.sparse ? f.sparse->Ticks(e) : nullptr;
            else
                t = f.ticks ? f.ticks + i : nullptr;
            ASTRA_ASSERT(t != nullptr, "View: a change-tracked Mut<T> yield has no tick slot in its store");
            if (!t) ASTRA_UNLIKELY
            {
                thread_local EntityTicks s_backstop{};
                return &s_backstop;
            }
            return t;
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
            if constexpr (Stamp)
            {
                (StampRequiredStore<std::tuple_element_t<ReqIs, RequiredTypes>>(std::get<ReqIs>(req), e, now), ...);
                (StampOptionalStore<std::tuple_element_t<OptIs, OptionalTypes>>(std::get<OptIs>(opt), std::get<OptIs>(op), e, i, now), ...);
            }
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
                StampVisit(chunk, cm, now, reqSeq, optSeq);

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

        // Exact visible-entity count for a required-filtered view: enabled-in-all
        // required enableable columns, per chunk. Called only when HasRequiredFilter.
        size_t SizeFiltered(Archetype* archetype)
        {
            if (UsesStoragePath())
                return SizeStorage(archetype);
            const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
            size_t total = 0;
            for (auto& chunkPtr : archetype->GetChunks())
            {
                ArchetypeChunk* chunk = chunkPtr.get();
                const size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                    continue;

                constexpr size_t NReq = std::tuple_size_v<EnabledRequiredFilter>;
                const uint64_t* reqWords[NReq == 0 ? 1 : NReq];
                bool allZero = true;
                const bool anyFull = ResolveRequiredFilter(chunk, cm, count, reqWords, allZero, std::make_index_sequence<NReq>{});
                if (anyFull) continue;                    // 0 visible in this chunk
                if (allZero) { total += count; continue; }

                // Mixed: subtract popcount of the disabled union, tail-masked to count.
                const size_t numWords = (count + 63) >> 6;
                size_t disabled = 0;
                for (size_t w = 0; w < numWords; ++w)
                {
                    uint64_t dis = 0;
                    for (size_t s = 0; s < NReq; ++s)
                        dis |= reqWords[s][w];
                    const size_t validBits = count - (w << 6);
                    if (validBits < 64)
                        dis &= (uint64_t(1) << validBits) - 1;
                    disabled += static_cast<size_t>(std::popcount(dis));
                }
                total += count - disabled;
            }
            return total;
        }

        std::vector<Archetype*> m_archetypes;
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        std::shared_ptr<IWorkScheduler> m_scheduler;  // null = sequential inline fallback

        uint32_t m_lastRefreshCounter = 0;
        uint32_t m_lastGeneration = 0;
        uint32_t m_lastRemovalCounter = 0;

        // Storage plan (spec 2026-10-01 s6.1), built once by BuildStoragePlan().
        std::array<StorageKind, NPlanSlots> m_kinds{};
        ComponentMask m_matchRequired{};   // Table-kind required | With | Changed | Added
        ComponentMask m_matchExcluded{};   // Table-kind Not<>
        size_t m_matchCount = 0;           // m_matchRequired.Count(): CollectArchetypes' pre-filter
        bool m_anyUniversal = false;       // some named type is Universal in this registry
        bool m_neverMatches = false;       // Not<U> on a Universal U
        StoreArray m_stores{};             // resolved per walk (ResolveStores)
    };

    // Compile-time read/write access sets of a View's type args, for the scheduler
    // (Stage 2). With<T>/Not<T>/Any/OneOf contribute nothing (match-only). Masks are
    // built at runtime from the type-lists, mirroring how SystemScheduler harvests
    // component masks today.
    template<typename V> struct ViewAccess;

    template<typename... Args>
    struct ViewAccess<View<Args...>>
    {
        using Reads  = typename Detail::AccessReads<Args...>::type;
        using Writes = typename Detail::AccessWrites<Args...>::type;

        static ComponentMask ReadMask()  { return MaskOf(static_cast<Reads*>(nullptr)); }
        static ComponentMask WriteMask() { return MaskOf(static_cast<Writes*>(nullptr)); }

    private:
        template<typename... Ts>
        static ComponentMask MaskOf(std::tuple<Ts...>*) { return MakeComponentMask<Ts...>(); }
    };
} // namespace Astra