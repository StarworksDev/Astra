#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Core/Tick.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    /**
     * Lightweight sentinel type for View iteration.
     */
    struct ViewSentinel {};

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

    /**
     * Iterable wrapper for View iteration.
     * Provides begin()/end() for range-based for loop support.
     *
     * @tparam Components The required component types to iterate over
     */
    template<typename... Components>
    class ViewIterable
    {
    public:
        class Iterator
        {
        public:
            // STL iterator traits
            using iterator_category = std::forward_iterator_tag;
            using value_type = std::tuple<Entity, Components&...>;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type*;
            using reference = value_type&;

            Iterator() noexcept = default;

            // `now` is the registry's current tick, applied as the coarse
            // change-detection stamp to every non-const component column of every
            // chunk this iterator enters (View::begin() supplies it). Deliberately
            // NO default: the stamp is an unconditional store, so a defaulted `1`
            // would REWIND a column already stamped at a later tick -- a silent
            // change-detection false negative (final review). The empty iterator
            // (no archetypes, enters no chunk) is the default constructor.
            // Not noexcept: the hook path allocates the match words. With hooks, the iterator
            // holds a pointer into the View, so the View must outlive and not move during iteration.
            Iterator(Archetype* const* archetypes, size_t archetypeCount, Tick now, const ViewIterHooks* hooks = nullptr)
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

            /**
             * Sentinel comparison - simple integer comparison.
             * When done, m_entityIndex >= m_chunkEntityCount (both 0).
             */
            ASTRA_FORCEINLINE bool operator!=(ViewSentinel) const noexcept
            {
                return m_entityIndex < m_chunkEntityCount;
            }

            ASTRA_FORCEINLINE bool operator==(ViewSentinel) const noexcept
            {
                return m_entityIndex >= m_chunkEntityCount;
            }

            /**
             * Pre-increment operator - advances to next entity.
             * Single index increment on hot path, matching ForEach's pattern.
             */
            // Hot path: one more index increment than before (m_compIndex) and the
            // same single compare; m_runEnd == the chunk's count when no hooks are set.
            ASTRA_FORCEINLINE Iterator& operator++()
            {
                ++m_entityIndex;
                ++m_compIndex;
                if (m_entityIndex >= m_runEnd) [[unlikely]]
                {
                    NextRun();
                }
                return *this;
            }

            ASTRA_FORCEINLINE Iterator operator++(int)
            {
                Iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            /**
             * Dereference operator - returns tuple of entity and component references.
             * Uses index-based access matching ForEach's optimized pattern.
             */
            ASTRA_FORCEINLINE value_type operator*() const noexcept
            {
                return MakeEntityTuple(std::index_sequence_for<Components...>{});
            }

        private:
            void FindFirstValidPosition()
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

            void AdvanceChunk()
            {
                ++m_chunkIndex;
                FindFirstValidPosition();
            }

            // Past the current run: the next run of this chunk (hooks), else the next chunk.
            void NextRun()
            {
                if (m_hooks && SeekRun(m_entityIndex))
                    return;
                AdvanceChunk();
            }

            // Enter a non-empty chunk. False when no entity of it is visible (hooks only).
            bool EnterChunk(Archetype* archetype, size_t chunkIndex)
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

            // Coarse change-detection stamp (spec s3.3 row 1): every non-const component
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
            bool SeekRun(size_t from)
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

            // Empty components have no storage (array pointer is nullptr);
            // hand out a shared static instance instead — same contract as
            // Archetype::ForEach.
            template<typename T>
            ASTRA_FORCEINLINE T& DerefComponent(std::remove_const_t<T>* array) const noexcept
            {
                if constexpr (std::is_empty_v<std::remove_const_t<T>>)
                {
                    static std::remove_const_t<T> s_emptyInstance{};
                    return s_emptyInstance;
                }
                else
                {
                    return array[m_compIndex];
                }
            }

            /**
             * Index-based access matching ForEach's optimized pattern:
             * entities[i], array0[i], array1[i], ...
             */
            template<size_t... Is>
            ASTRA_FORCEINLINE value_type MakeEntityTuple(std::index_sequence<Is...>) const noexcept
            {
                return value_type{
                    m_entities[m_entityIndex],
                    DerefComponent<Components>(std::get<Is>(m_componentArrays))...
                };
            }

            // View-level state
            Archetype* const* m_archetypes = nullptr;
            size_t m_archetypeCount = 0;
            Tick m_now = 0;   // change-detection tick stamped on chunk entry; 0 ("never") only in the empty iterator, which enters no chunk
            const ViewIterHooks* m_hooks = nullptr;   // null = plain Table walk

            // Navigation state
            size_t m_archetypeIndex = 0;
            size_t m_chunkIndex = 0;
            size_t m_entityIndex = 0;

            // Cached chunk state
            size_t m_chunkEntityCount = 0;
            const Entity* m_entities = nullptr;
            std::tuple<std::remove_const_t<Components>*...> m_componentArrays{};

            // Storage-policy run-stepping (hooks mode); m_runEnd/m_compIndex are also the
            // plain walk's bound and component index (== m_chunkEntityCount / m_entityIndex).
            Archetype* m_archetype = nullptr;
            ArchetypeChunk* m_chunk = nullptr;
            std::vector<uint64_t> m_words;
            size_t m_runEnd = 0;
            size_t m_compIndex = 0;
        };

        // `now`: the registry's current tick for the iterator's chunk-entry stamp
        // (see Iterator's constructor); no default, for the same reason.
        ViewIterable(Archetype* const* archetypes, size_t count, Tick now) noexcept
            : m_archetypes(archetypes)
            , m_archetypeCount(count)
            , m_now(now)
        {
        }

        ASTRA_FORCEINLINE Iterator begin() const
        {
            return Iterator(m_archetypes, m_archetypeCount, m_now);
        }

        ASTRA_FORCEINLINE ViewSentinel end() const noexcept
        {
            return ViewSentinel{};
        }

    private:
        Archetype* const* m_archetypes;
        size_t m_archetypeCount;
        Tick m_now;
    };

    template<typename Tuple>
    struct ViewIterableFromTuple;

    template<typename... Components>
    struct ViewIterableFromTuple<std::tuple<Components...>>
    {
        using type = ViewIterable<Components...>;
        using Iterator = typename type::Iterator;
    };

    template<typename Tuple>
    using ViewIterableFromTuple_t = typename ViewIterableFromTuple<Tuple>::type;

    template<typename Tuple>
    using ViewIteratorFromTuple_t = typename ViewIterableFromTuple<Tuple>::Iterator;

} // namespace Astra
