#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Tick.hpp"
#include "../Core/Tlsf.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    // Per-archetype column metadata, built once and shared by every chunk of the archetype.
    // The heavy ComponentDescriptor lives here once (via pointer into the archetype's canonical
    // list) instead of by-value in every chunk slot. Columns are storage-bearing components plus
    // one BIT-ONLY column (stride 0: disabled words, no data) per ENABLEABLE tag; non-enableable
    // tags have no column. Sorted ascending by id so cross-archetype moves can merge-join.
    struct ArchetypeColumnMeta
    {
        struct ColumnDesc
        {
            ComponentID id{0};
            uint32_t stride{0};                          // == descriptor->size (0 for an enableable tag's bit-only column)
            const ComponentDescriptor* descriptor{nullptr};
        };

        uint16_t  columnCount{0};                        // N: storage-bearing components + enableable-tag bit-only columns
        bool      isComplex{false};                      // any column not trivially copyable
        ColumnDesc columns[MAX_COMPONENTS]{};            // [0, columnCount) valid, ascending id
        int16_t   idToColumn[MAX_COMPONENTS];            // id -> column index, or -1 (absent OR non-enableable tag)

        // Enableable-components (spec §3/§4): the column ordinals (into `columns`)
        // whose component opted into ASTRA_ENABLEABLE. Only these carve per-chunk
        // disabled-bit words + do bit bookkeeping on relocation; enableableColumnCount
        // == 0 is the zero-cost early-out every non-enableable archetype takes. Built
        // once by Archetype::BuildColumnMeta after the ascending-id sort (so the
        // ordinals are final). Shared per-archetype, not per-chunk.
        uint16_t  enableableColumns[MAX_COMPONENTS]{};   // [0, enableableColumnCount) valid, ascending ordinal
        uint16_t  enableableColumnCount{0};

        // Change-tracked columns (spec 2026-09-10 §3.2): ordinals whose component opted
        // into AstraChangeTracked. Only these carve per-chunk {added, changed} tick
        // columns and copy ticks on relocation; trackedColumnCount == 0 is the zero-cost
        // early-out every untracked archetype takes. Built by Archetype::BuildColumnMeta.
        uint16_t  trackedColumns[MAX_COMPONENTS]{};
        uint16_t  trackedColumnCount{0};

        ArchetypeColumnMeta() { for (auto& c : idToColumn) c = -1; }
    };

    class ArchetypeChunkPool;
    class Archetype;

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

        // `wordCount` must be the value passed to the matching Acquire. Declared
        // noexcept for the chunk destructor, but the cache push_back can allocate: a
        // bad_alloc here is std::terminate (accepted; release must not fail).
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
        // 2^0 .. 2^19 words. The 1 MB chunk ceiling holds at most 2^20 slots
        // (ComputeCapacityForBytes with a 1-byte entity), which is 2^14 words, so the top
        // classes are headroom.
        static constexpr size_t CLASS_COUNT = 20;
        ASTRA_NODISCARD static size_t ClassOf(size_t words) noexcept { return static_cast<size_t>(std::bit_width(words - 1)); }

        std::vector<uint64_t*> m_free[CLASS_COUNT];
        size_t m_live = 0;
    };

    class ArchetypeChunk
    {
    public:
        ArchetypeChunk(ArchetypeChunk&& other) noexcept :
            m_memory(std::exchange(other.m_memory, nullptr)),
            m_capacity(other.m_capacity),
            m_count(other.m_count),
            m_entities(std::move(other.m_entities)),
            m_meta(other.m_meta),
            m_columnVersion(other.m_columnVersion),
            m_chunkSize(other.m_chunkSize),
            m_sparseBlocks(std::move(other.m_sparseBlocks)),
            m_sparseWords(other.m_sparseWords)
        {
            // Column is trivially copyable; a memberwise byte copy of the packed
            // array is correct (the bases point into m_memory, which we took over).
            std::memcpy(m_columns, other.m_columns, sizeof(m_columns));
            // Make the moved-from chunk inert so its destructor touches nothing.
            other.m_meta = nullptr;
            other.m_count = 0;
            other.m_sparseBlocks.clear();   // the blocks' words now belong to *this
        }
        
        ArchetypeChunk(const ArchetypeChunk&) = delete;
        ArchetypeChunk& operator=(const ArchetypeChunk&) = delete;

        ~ArchetypeChunk()
        {
            ReleaseSparseBlocks();   // presence words go back to the pool (spec s5.1)

            if (!m_meta) ASTRA_UNLIKELY
                return;  // moved-from (inert) chunk: nothing to destruct

            // Destruct every live element of every storage column.
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                std::byte* base = static_cast<std::byte*>(m_columns[c].base);
                const uint32_t stride = m_columns[c].stride;
                const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;

                for (size_t i = 0; i < m_count; ++i)
                {
                    desc.Destruct(base + i * stride);
                }
            }
        }

        // `tick` is the registry's current tick (Archetype::Now()), applied as the
        // coarse stamp to EVERY column of this chunk and as the per-entity ticks of
        // the new slot. Deliberately NO default: StampAllColumns is an unconditional
        // store, so a defaulted `1` would REWIND a column that was already stamped at
        // a later tick -- a silent change-detection false negative (final review).
        size_t AddEntity(Entity entity, Tick tick)
        {
            ASTRA_ASSERT(m_count < m_capacity, "Chunk is full, cannot add more entities");
            size_t index = m_count++;
            
            m_entities.push_back(entity);

            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                void* ptr = static_cast<std::byte*>(m_columns[c].base) + index * m_columns[c].stride;
                m_meta->columns[c].descriptor->DefaultConstruct(ptr);
            }

            StampAllColumns(tick);
            InitTicks(index, tick);

            return index;
        }
        
        // Helper to construct a single component with a value at a specific index
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

            const ComponentDescriptor& desc = *m_meta->columns[col].descriptor;
            void* ptr = static_cast<std::byte*>(m_columns[col].base) + index * m_columns[col].stride;

            // Use ConstructWith for optimal construction with value
            if constexpr (std::is_lvalue_reference_v<T>)
            {
                desc.ConstructWith(ptr, &value);
            }
            else
            {
                // For rvalues, we need temporary storage. Move (not ConstructWith/copy)
                // out of temp: it is a genuine local about to be destroyed regardless, and
                // moveConstruct is set unconditionally for every Component (move-constructible
                // is part of the concept), unlike constructWith/copyConstruct which are null
                // for move-only types -- ConstructWith would silently fall back to
                // DefaultConstruct for those, discarding the value (Theme G, found while
                // wiring the move-only Tracked test component through this path).
                using DecayedType = std::decay_t<T>;
                DecayedType temp(std::forward<T>(value));
                desc.MoveConstruct(ptr, &temp);
            }
        }
        
        // Add entity with components constructed directly with values
        template<typename... Components>
        size_t AddEntityWithComponents(Entity entity, Tick tick, Components&&... components)
        {
            ASTRA_ASSERT(m_count < m_capacity, "Chunk is full, cannot add more entities");
            size_t index = m_count++;
            
            m_entities.push_back(entity);
            
            // First, default construct all components that are in the archetype
            // but not provided in the parameter pack
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                const ComponentID id = m_meta->columns[c].id;

                // Check if this component is in our parameter pack
                bool willBeConstructed = ((TypeID<std::decay_t<Components>>::Value() == id) || ...);

                if (!willBeConstructed)
                {
                    void* ptr = static_cast<std::byte*>(m_columns[c].base) + index * m_columns[c].stride;
                    m_meta->columns[c].descriptor->DefaultConstruct(ptr);
                }
            }
            
            // Now construct the provided components with their values
            ((ConstructComponentAt(index, std::forward<Components>(components))), ...);

            StampAllColumns(tick);
            InitTicks(index, tick);

            return index;
        }
        
        // `tick`: see AddEntity -- no default, for the same version-rewind reason.
        void BatchAddEntities(std::span<const Entity> entities, Tick tick)
        {
            size_t count = entities.size();
            ASTRA_ASSERT(m_count + count <= m_capacity, "Batch add would exceed chunk capacity");

            m_entities.insert(m_entities.end(), entities.begin(), entities.end());

            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                std::byte* startPtr = static_cast<std::byte*>(m_columns[c].base) + m_count * m_columns[c].stride;
                m_meta->columns[c].descriptor->BatchDefaultConstruct(startPtr, count);
            }

            const size_t start = m_count;
            m_count += count;

            StampAllColumns(tick);

            // Per-entity tick init for tracked columns only: one compare keeps the
            // untracked create_batch hot path free of the per-slot loop.
            if (m_meta->trackedColumnCount) ASTRA_UNLIKELY
            {
                for (size_t i = start; i < m_count; ++i)
                    InitTicks(i, tick);
            }
        }
        
        void BatchMoveComponentsFrom(std::span<const size_t> dstIndices, const ArchetypeChunk& srcChunk, std::span<const size_t> srcIndices, const ComponentMask& componentsToMove)
        {
            ASTRA_ASSERT(dstIndices.size() == srcIndices.size(), "Destination and source index arrays must have the same size");
            size_t count = dstIndices.size();
            
            if (componentsToMove.None()) return;

            // Iterate the destination's storage columns; only move those that are
            // in the move set AND present as storage in the source chunk.
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                const ComponentID id = m_meta->columns[c].id;
                if (!componentsToMove.Test(id)) continue;

                const int sc = srcChunk.m_meta->idToColumn[id];
                if (sc < 0) continue;  // source lacks this component's storage

                void* dstBase = m_columns[c].base;
                void* srcBase = srcChunk.m_columns[sc].base;
                const uint32_t stride = m_columns[c].stride;
                const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;

                if (desc.is_trivially_copyable && AreIndicesContiguous(dstIndices) && AreIndicesContiguous(srcIndices))
                {
                    void* dstPtr = static_cast<std::byte*>(dstBase) + dstIndices[0] * stride;
                    void* srcPtr = static_cast<std::byte*>(srcBase) + srcIndices[0] * stride;
                    std::memcpy(dstPtr, srcPtr, count * stride);
                }
                else
                {
                    for (size_t i = 0; i < count; ++i)
                    {
                        void* dstPtr = static_cast<std::byte*>(dstBase) + dstIndices[i] * stride;
                        void* srcPtr = static_cast<std::byte*>(srcBase) + srcIndices[i] * stride;

                        if (desc.is_trivially_copyable)
                        {
                            std::memcpy(dstPtr, srcPtr, stride);
                        }
                        else
                        {
                            desc.MoveConstruct(dstPtr, srcPtr);
                        }
                    }
                }

                // Disabled-bit carry (Task 2): for an enableable shared column, copy
                // each src slot's bit into its dst slot. dst slots are freshly
                // allocated (born enabled), so SetDisabled sets them; src bits are
                // cleared by the src archetype's own swap-remove. Column `c` (dst) and
                // `sc` (src) share the same component id => same enableable-ness.
                if (desc.isEnableable) ASTRA_UNLIKELY
                {
                    for (size_t i = 0; i < count; ++i)
                        SetDisabled(c, dstIndices[i], srcChunk.IsDisabled(sc, srcIndices[i]));
                }

                // Tick carry (Task 6): a shared tracked column keeps each entity's own
                // {added, changed} across the batch move. dst slots were InitTicks'd to
                // Now() when claimed (BatchMoveEntitiesFrom); this overwrites the SHARED
                // columns only -- a newly added tracked column keeps Now().
                if (desc.isChangeTracked) ASTRA_UNLIKELY
                {
                    for (size_t i = 0; i < count; ++i)
                        CopyTicks(c, dstIndices[i], srcChunk, sc, srcIndices[i]);
                }
            }
        }
        
        static bool AreIndicesContiguous(std::span<const size_t> indices)
        {
            if (indices.size() <= 1)
                return true;
            for (size_t i = 1; i < indices.size(); ++i)
            {
                if (indices[i] != indices[i-1] + 1)
                {
                    return false;
                }
            }
            return true;
        }
        
        template<Component T>
        void BatchConstructComponent(std::span<const size_t> indices, const T& value)
        {
            // Empty (tag) component: nothing to construct. Must precede the column lookup: an
            // enableable tag HAS a (bit-only, stride-0) column, and the trivially-copyable path
            // below would memcpy sizeof(T) == 1 byte through its base.
            if constexpr (std::is_empty_v<T>)
                return;

            ComponentID id = TypeID<T>::Value();
            const int col = m_meta->idToColumn[id];

            // A column index < 0 means this component is absent from the archetype
            // OR is an empty (tag) component with no storage; either way there is
            // nothing to construct — never form a pointer from it.
            if (col < 0) ASTRA_UNLIKELY
                return;

            void* base = m_columns[col].base;
            const uint32_t stride = m_columns[col].stride;

            // Debug validation
            for (size_t idx : indices)
            {
                (void)idx; // only used by the assert below in debug builds
                ASTRA_ASSERT(idx < m_capacity, "BatchConstructComponent: index out of capacity");
                // Note: We allow idx >= m_count because entities might be in the process of being added
                // ASTRA_ASSERT(idx < m_count, "BatchConstructComponent: index out of current count");
            }
            
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                bool contiguous = true;
                for (size_t i = 1; i < indices.size(); ++i)
                {
                    if (indices[i] != indices[i-1] + 1)
                    {
                        contiguous = false;
                        break;
                    }
                }
                
                if (contiguous && indices.size() > 1)
                {
                    T* firstPtr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + indices[0] * stride));
                    new (firstPtr) T(value);

                    for (size_t i = 1; i < indices.size(); ++i)
                    {
                        T* ptr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + indices[i] * stride));
                        std::memcpy(ptr, firstPtr, sizeof(T));
                    }
                }
                else
                {
                    for (size_t idx : indices)
                    {
                        ASTRA_ASSERT(idx < m_capacity, "Index out of capacity");
                        T* ptr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + idx * stride));
                        std::memcpy(ptr, &value, sizeof(T));
                    }
                }
            }
            else
            {
                // Slow path: construct each component
                for (size_t idx : indices)
                {
                    ASTRA_ASSERT(idx < m_capacity, "Component index out of bounds");
                    T* ptr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + idx * stride));
                    new (ptr) T(value);
                }
            }
        }

        std::optional<Entity> RemoveEntity(size_t index)
        {
            ASTRA_ASSERT(index < m_count, "Entity index out of bounds");
            
            const size_t lastIndex = m_count - 1;
            std::optional<Entity> movedEntity;
            
            if (index != lastIndex) ASTRA_LIKELY
            {
                // Move last entity to this position
                m_entities[index] = m_entities[lastIndex];
                movedEntity = m_entities[index];
                
                // Move components column by column (every column has real storage)
                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    std::byte* base = static_cast<std::byte*>(m_columns[c].base);
                    const uint32_t stride = m_columns[c].stride;
                    const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;

                    void* dstPtr = base + index * stride;
                    void* srcPtr = base + lastIndex * stride;

                    // Destruct destination, move from source, then destruct the
                    // moved-from source slot (the same pairing Archetype::CompactChunks uses).
                    desc.Destruct(dstPtr);
                    desc.MoveConstruct(dstPtr, srcPtr);
                    desc.Destruct(srcPtr);
                }
            }
            else
            {
                // Just destruct the last entity's components column by column
                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    void* ptr = static_cast<std::byte*>(m_columns[c].base) + lastIndex * m_columns[c].stride;
                    m_meta->columns[c].descriptor->Destruct(ptr);
                }
            }

            // Disabled-bit carry (Task 2), mirroring the component swap-remove above.
            // For every enableable column: the moved (last) entity's bit fills the
            // vacated slot, then the tail slot is cleared so bits at slots >= the new
            // count stay 0 (invariant 2). SetDisabled keeps disabledCount == popcount,
            // and its per-slot delta makes the count arithmetic correct even in the
            // index == lastIndex (removing the last entity) case -- only the tail clear
            // runs then, dropping exactly the removed entity's own bit.
            for (uint16_t e = 0; e < m_meta->enableableColumnCount; ++e)
            {
                const uint16_t c = m_meta->enableableColumns[e];
                if (index != lastIndex) ASTRA_LIKELY
                    SetDisabled(c, index, IsDisabled(c, lastIndex));
                SetDisabled(c, lastIndex, false);
            }

            // Tick carry: the moved (last) entity's ticks fill the vacated slot. No tail
            // clear is needed (ticks are re-initialised when a slot is reused).
            if (index != lastIndex) ASTRA_LIKELY
                for (uint16_t t = 0; t < m_meta->trackedColumnCount; ++t)
                {
                    const uint16_t c = m_meta->trackedColumns[t];
                    m_columns[c].ticks[index] = m_columns[c].ticks[lastIndex];
                }

            // Remove last entity
            m_entities.pop_back();
            --m_count;

            return movedEntity;
        }
        
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
        
        template<Component T>
        ASTRA_FORCEINLINE auto GetComponentArray()
        {
            using BaseType = std::remove_const_t<T>;

            // For empty components (tags), return nullptr since they have no data
            // The iteration code will handle this specially
            if constexpr (std::is_empty_v<BaseType>)
            {
                return static_cast<BaseType*>(nullptr);
            }
            else
            {
                void* base = GetComponentArrayByID(TypeID<BaseType>::Value());
                if constexpr (std::is_const_v<T>)
                {
                    return reinterpret_cast<const BaseType*>(base);
                }
                else
                {
                    return reinterpret_cast<BaseType*>(base);
                }
            }
        }

        template<Component T>
        ASTRA_FORCEINLINE const std::remove_const_t<T>* GetComponentArray() const
        {
            using BaseType = std::remove_const_t<T>;

            // For empty components, return nullptr since they have no data
            if constexpr (std::is_empty_v<BaseType>)
            {
                return nullptr;
            }
            else
            {
                return reinterpret_cast<const BaseType*>(GetComponentArrayByID(TypeID<BaseType>::Value()));
            }
        }

        void* GetComponentArrayByID(ComponentID id) const
        {
            const int col = m_meta->idToColumn[id];
            return col < 0 ? nullptr : m_columns[col].base;
        }
        
        // ===================== Enableable-components (Task 2) =====================
        // SET bit == DISABLED. `column` is a storage-column ordinal (m_meta->columns
        // index), NOT a ComponentID -- callers resolve id -> column via idToColumn.
        // A non-enableable column has no word region: GetDisabledWords returns nullptr,
        // IsDisabled returns false, SetDisabled is a no-op returning false.

        // Raw word region for a column, or nullptr if the column is not enableable.
        // Tasks 3-5 read this directly for query filtering.
        ASTRA_NODISCARD uint64_t* GetDisabledWords(int column) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columns[column].disabledWords;
        }

        ASTRA_NODISCARD uint32_t GetDisabledCount(int column) const noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columns[column].disabledCount;
        }

        ASTRA_NODISCARD bool IsDisabled(int column, size_t index) const noexcept
        {
            const uint64_t* words = m_columns[column].disabledWords;
            if (!words) ASTRA_UNLIKELY
                return false;   // not enableable: everyone is enabled
            return (words[index >> 6] >> (index & 63)) & 1ull;
        }

        // Sets slot `index` of `column` to disabled/enabled, keeping disabledCount ==
        // popcount. Returns true IFF the bit actually changed (the signal-fire gate);
        // an idempotent set (or a non-enableable column) returns false.
        bool SetDisabled(int column, size_t index, bool disabled) noexcept
        {
            uint64_t* words = m_columns[column].disabledWords;
            if (!words) ASTRA_UNLIKELY
                return false;   // not enableable: nothing to store

            const size_t w = index >> 6;
            const uint64_t bit = 1ull << (index & 63);
            const bool cur = (words[w] & bit) != 0;
            if (cur == disabled)
                return false;   // no change

            if (disabled)
            {
                words[w] |= bit;
                ++m_columns[column].disabledCount;
            }
            else
            {
                words[w] &= ~bit;
                --m_columns[column].disabledCount;
            }
            return true;
        }

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

        // ===================== Change detection: chunk column versions =====================
        // One Tick per storage column (spec 2026-09-10 §3.2, coarse tier -- every
        // component pays 4 bytes per chunk per column). `column` is a storage-column
        // ORDINAL (m_meta->columns index), never a ComponentID. The region is carved
        // in the chunk arena right after the column data (see InitializeColumns) and
        // is zero-initialised: version 0 == "never stamped". Stamps are plain stores.

        ASTRA_NODISCARD ASTRA_FORCEINLINE Tick GetColumnVersion(int column) const noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columnVersion[column];
        }

        ASTRA_FORCEINLINE void StampColumn(int column, Tick tick) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            m_columnVersion[column] = tick;
        }

        // Every storage column at once: the structural-write stamp (a slot in this
        // chunk just received an entity -- create, move, deserialize).
        ASTRA_FORCEINLINE void StampAllColumns(Tick tick) noexcept
        {
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                m_columnVersion[c] = tick;
        }

        // Same-archetype relocation carry (CompactChunks):
        // keep whichever of the two versions is newer so a recently-written entity
        // that gets repacked into a fresh chunk is never read as unchanged.
        ASTRA_FORCEINLINE void FoldColumnVersion(int column, Tick other) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            if (IsNewer(other, m_columnVersion[column]))
                m_columnVersion[column] = other;
        }

        // Inspector accessor: byte offset of the version region within the arena.
        ASTRA_NODISCARD size_t GetColumnVersionOffset() const
        {
            return static_cast<size_t>(reinterpret_cast<const std::byte*>(m_columnVersion) -
                                       static_cast<const std::byte*>(m_memory));
        }

        // ===================== Change detection: per-entity ticks (tracked columns) =====================
        ASTRA_NODISCARD ASTRA_FORCEINLINE EntityTicks* GetTicks(int column) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columns[column].ticks;
        }
        ASTRA_NODISCARD ASTRA_FORCEINLINE const EntityTicks* GetTicks(int column) const noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columns[column].ticks;
        }
        ASTRA_NODISCARD ASTRA_FORCEINLINE bool IsTracked(int column) const noexcept
        {
            return m_columns[column].ticks != nullptr;
        }

        // A slot just received a NEW component set (create / batch create / added
        // component): every tracked column reads added == changed == tick.
        ASTRA_FORCEINLINE void InitTicks(size_t index, Tick tick) noexcept
        {
            for (uint16_t t = 0; t < m_meta->trackedColumnCount; ++t)
                m_columns[m_meta->trackedColumns[t]].ticks[index] = EntityTicks{tick, tick};
        }

        // Relocation carry: copy one slot's ticks for one shared tracked column. The two
        // columns share a ComponentID (hence tracked-ness); no-op when untracked.
        ASTRA_FORCEINLINE void CopyTicks(int dstColumn, size_t dstIndex,
                                         const ArchetypeChunk& src, int srcColumn, size_t srcIndex) noexcept
        {
            EntityTicks* d = m_columns[dstColumn].ticks;
            const EntityTicks* s = src.m_columns[srcColumn].ticks;
            if (d && s) ASTRA_UNLIKELY
                d[dstIndex] = s[srcIndex];
        }

        ASTRA_NODISCARD size_t GetTicksOffset(uint16_t column) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            const EntityTicks* t = m_columns[column].ticks;
            if (!t) return std::numeric_limits<size_t>::max();
            return static_cast<size_t>(reinterpret_cast<const std::byte*>(t) - static_cast<const std::byte*>(m_memory));
        }

        ASTRA_NODISCARD bool IsFull() const noexcept { return m_count >= m_capacity; }
        // Byte size of this chunk's arena. Chunks of one archetype no longer
        // share a single size (Phase 2), so memory accounting must sum this
        // per chunk instead of multiplying a chunk count by the pool's size.
        ASTRA_NODISCARD size_t GetChunkBytes() const noexcept { return m_chunkSize; }

        // Debug/inspection accessors (Inspector facade): byte offsets within the
        // chunk arena. Kept here so tools never re-derive InitializeColumns' layout.
        ASTRA_NODISCARD size_t GetColumnOffset(uint16_t column) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            return static_cast<size_t>(static_cast<const std::byte*>(m_columns[column].base) -
                                       static_cast<const std::byte*>(m_memory));
        }

        // Offset of the column's disabled-bit words, or size_t max if not enableable.
        // Word regions are 8-byte aligned (unlike column bases, which are
        // cache-line aligned) -- mirrors InitializeColumns' carve.
        ASTRA_NODISCARD size_t GetDisabledWordsOffset(uint16_t column) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            const uint64_t* words = m_columns[column].disabledWords;
            if (!words) return std::numeric_limits<size_t>::max();
            return static_cast<size_t>(reinterpret_cast<const std::byte*>(words) -
                                       static_cast<const std::byte*>(m_memory));
        }

        ASTRA_NODISCARD bool IsEmpty() const noexcept { return m_count == 0; }
        ASTRA_NODISCARD size_t GetCount() const noexcept { return m_count; }
        ASTRA_NODISCARD size_t GetCapacity() const noexcept { return m_capacity; }
        ASTRA_NODISCARD Entity GetEntity(size_t index) const { ASTRA_ASSERT(index < m_count, "Index out of count"); return m_entities[index]; }
        ASTRA_NODISCARD const std::vector<Entity>& GetEntities() const { return m_entities; }
        ASTRA_NODISCARD ASTRA_FORCEINLINE std::vector<Entity>& GetEntities() { return m_entities; }

        void SetCount(size_t count) noexcept { m_count = count; }

        void* GetComponentPointer(ComponentID id, size_t index) const
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS, "ComponentID out of bounds");
            ASTRA_ASSERT(index < m_count, "Index out of bounds");

            const int col = m_meta->idToColumn[id];
            if (col < 0) ASTRA_UNLIKELY
                return nullptr;  // absent or tag: no storage

            return static_cast<std::byte*>(m_columns[col].base) + index * m_columns[col].stride;
        }

    private:
        friend class ArchetypeManager;
        friend class Archetype;

        ArchetypeChunk(size_t entitiesPerChunk, const ArchetypeColumnMeta* meta, void* memory, size_t chunkSize) :
            m_memory(memory),
            m_capacity(entitiesPerChunk),
            m_count(0),
            m_meta(meta),
            m_chunkSize(chunkSize)
        {
            m_entities.reserve(m_capacity);
            std::memset(m_memory, 0, m_chunkSize);
            InitializeColumns();
        }

        // Words of disabled bits a column needs to cover `capacity` slots (64 slots
        // per 64-bit word). Kept alongside the carve so the archetype-side capacity
        // math (Archetype::ComputeLayoutBytesForCapacity) can mirror it exactly.
        ASTRA_NODISCARD static constexpr size_t WordsForCapacity(size_t capacity) noexcept
        {
            return (capacity + 63) / 64;
        }

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

            // Change-detection (spec 2026-09-10 §3.2): one Tick per storage column,
            // 8-byte aligned, immediately after the column data. Chunk memory was just
            // zeroed, so every column is born at version 0 == "never stamped" for free.
            // Archetype::ComputeLayoutBytesForCapacity mirrors this carve byte-for-byte,
            // and Archetype::Initialize folds its upper bound into m_alignmentOverhead so
            // the conservative capacity estimate stays a guaranteed fit.
            offset = (offset + 7) & ~size_t(7);
            m_columnVersion = reinterpret_cast<Tick*>(static_cast<std::byte*>(m_memory) + offset);
            offset += static_cast<size_t>(m_meta->columnCount) * sizeof(Tick);

            // Third loop: carve one disabled-bit-word region per ENABLEABLE column,
            // 8-byte aligned, immediately after the version region. Chunk memory was just
            // zeroed (ctor memset), so every entity is born ENABLED with ZERO writes --
            // the create/batch-create paths gain nothing (invariant 1). Non-enableable
            // columns keep disabledWords == nullptr. The archetype's capacity math has
            // already guaranteed these regions fit; the assert below is the net.
            const size_t words = WordsForCapacity(m_capacity);
            for (uint16_t e = 0; e < m_meta->enableableColumnCount; ++e)
            {
                const uint16_t c = m_meta->enableableColumns[e];
                offset = (offset + 7) & ~size_t(7);
                m_columns[c].disabledWords = reinterpret_cast<uint64_t*>(static_cast<std::byte*>(m_memory) + offset);
                offset += words * 8;
            }

            // Third loop: one {added, changed} tick column per CHANGE-TRACKED column,
            // 8-byte aligned. Zero-init == tick 0 == "never"; every slot is initialised
            // to Now() when an entity lands in it (InitTicks) or copied on a move.
            for (uint16_t t = 0; t < m_meta->trackedColumnCount; ++t)
            {
                const uint16_t c = m_meta->trackedColumns[t];
                offset = (offset + 7) & ~size_t(7);
                m_columns[c].ticks = reinterpret_cast<EntityTicks*>(static_cast<std::byte*>(m_memory) + offset);
                offset += m_capacity * sizeof(EntityTicks);
            }

            ASTRA_ASSERT(offset <= m_chunkSize, "Component layout exceeds chunk size");
        }

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

        // Ordinal-direct column addressing for merge-join callers that already
        // hold the column index (chunk columns are packed in m_meta's
        // ascending-id order -- Phase C invariant, single creation path).
        // Skips the idToColumn resolution GetComponentPointer performs.
        ASTRA_NODISCARD ASTRA_FORCEINLINE void* GetColumnPointer(uint16_t column, size_t index) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            ASTRA_ASSERT(index < m_count, "Entity index out of bounds");
            return static_cast<std::byte*>(m_columns[column].base) + index * m_columns[column].stride;
        }

        // Packed per-column storage descriptor. Fixed-capacity array (one slot per
        // possible component id) avoids a per-chunk heap allocation; only
        // [0, m_meta->columnCount) are live.
        //
        // Enableable columns additionally own a region of disabled-bit words carved
        // INSIDE the chunk arena (SET bit == DISABLED; zero-init == enabled) plus a
        // running disabledCount that is always == popcount(disabledWords). Non-
        // enableable columns keep disabledWords == nullptr and pay nothing. The
        // growth of the per-chunk Column footprint (16 -> 24 bytes) is accepted:
        // the recorded Phase-C packed-[N] Column deferral now also covers these two
        // fields. Since m_columns is a fixed MAX_COMPONENTS-slot array regardless of
        // how many columns are enableable, this is a flat +8 bytes/slot = +1KB/chunk
        // fixed metadata cost, always on -- the one part of this feature that isn't
        // pay-for-what-you-use. uint32_t count (not uint16) because capacity can
        // exceed 65535 at the 512KB chunk cap.
        //
        // Change-tracked columns (spec 2026-09-10 §3.2) likewise own a per-entity
        // {added, changed} tick column carved INSIDE the arena; untracked columns keep
        // ticks == nullptr. That is a further +8 bytes/slot of fixed per-chunk metadata
        // (Column 24 -> 32 bytes), also accepted: the packed-[N] Column deferral now
        // covers three fields (disabledCount, disabledWords, ticks).
        struct Column
        {
            void* base{nullptr};
            uint32_t stride{0};
            uint32_t disabledCount{0};        // == popcount(disabledWords[0, words)); 0 for non-enableable
            uint64_t* disabledWords{nullptr}; // enableable columns only; nullptr otherwise
            EntityTicks* ticks{nullptr};      // change-tracked columns only; nullptr otherwise
        };

        void* m_memory;
        size_t m_capacity;
        size_t m_count;
        std::vector<Entity> m_entities;
        const ArchetypeColumnMeta* m_meta{nullptr};  // shared per-archetype metadata (not owned)
        Column m_columns[MAX_COMPONENTS]{};          // [0, m_meta->columnCount) live
        Tick* m_columnVersion{nullptr};              // arena-carved, [0, columnCount) live; 0 == never
        size_t m_chunkSize;
        SmallVector<SparseBlock, 2> m_sparseBlocks;     // sorted by slot; lazily populated (spec s5.1)
        SparseWordFreelist* m_sparseWords{nullptr};     // the owning pool's freelist (set by CreateChunk)

        friend class ArchetypeChunkPool;
    };

    class ArchetypeChunkPool
    {
    public:
        using Chunk = ArchetypeChunk;

        static constexpr size_t DEFAULT_CHUNK_SIZE = 16 * 1024;  // 16KB default (fits in L1 cache)
        static constexpr size_t MIN_CHUNK_SIZE = 4 * 1024;       // 4KB minimum
        static constexpr size_t MAX_CHUNK_SIZE = 1024 * 1024;    // 1MB maximum

        // Grow-as-populate sizing CEILING default (study: 512KB, not the 1MB
        // MAX_CHUNK_SIZE absolute ceiling). Named so Config's NSDMI below and
        // Archetype::Deserialize's no-pool fallback (which cannot see a live
        // Config instance) share one literal instead of two that could diverge.
        static constexpr size_t DEFAULT_MAX_CHUNK_BYTES = 512 * 1024;

        // Configuration for pool behavior
        struct Config
        {
            size_t chunkSize = DEFAULT_CHUNK_SIZE;
            size_t chunksPerBlock = 128;
            size_t maxChunks = 4096;
            size_t initialBlocks = 0;
            bool useHugePages = true;

            // Grow-as-populate sizing policy (Phase 2 Unit C part 2): each NEW
            // chunk an archetype appends is sized from its current data footprint,
            // clamped to [minChunkBytes, maxChunkBytes]. See Archetype::NextChunkBytes.
            size_t minChunkBytes = MIN_CHUNK_SIZE;           // 4KB - first/smallest chunk
            size_t maxChunkBytes = DEFAULT_MAX_CHUNK_BYTES;  // sizing cap
            size_t growDivisor = 2;                          // chunk ~ archetypeBytes / growDivisor
        };
        
        struct Stats
        {
            size_t totalChunks = 0;
            size_t freeChunks = 0;
            size_t acquireCount = 0;
            size_t releaseCount = 0;
            size_t blockAllocations = 0;
            size_t failedAcquires = 0;
        };
        
        // Custom deleter for chunks
        struct ChunkDeleter
        {
            ArchetypeChunkPool* pool = nullptr;
            void* memory = nullptr;
            
            void operator()(Chunk* chunk) const
            {
                if (chunk) ASTRA_LIKELY
                {
                    delete chunk;
                    
                    if (pool && memory) ASTRA_LIKELY
                    {
                        pool->ReturnChunk(memory);
                    }
                }
            }
        };

        // Delegating overload instead of a default argument: gcc/clang reject a
        // default argument that needs Config's NSDMIs before the enclosing class is complete.
        ArchetypeChunkPool() : ArchetypeChunkPool(Config{}) {}

        explicit ArchetypeChunkPool(const Config& config) : m_config(config)
        {
            ASTRA_ASSERT(m_config.chunkSize >= MIN_CHUNK_SIZE && m_config.chunkSize <= MAX_CHUNK_SIZE, "Chunk size must be between 4KB and 1MB");
            ASTRA_ASSERT((m_config.chunkSize & (m_config.chunkSize - 1)) == 0, "Chunk size must be a power of 2");
            ASTRA_ASSERT(m_config.minChunkBytes >= MIN_CHUNK_SIZE, "minChunkBytes below the pool's absolute floor");
            ASTRA_ASSERT(m_config.maxChunkBytes <= MAX_CHUNK_SIZE, "maxChunkBytes above the pool's absolute ceiling");
            ASTRA_ASSERT(m_config.minChunkBytes <= m_config.maxChunkBytes, "minChunkBytes must not exceed maxChunkBytes");
            ASTRA_ASSERT(m_config.growDivisor >= 1, "growDivisor must be at least 1");

            if (m_config.chunksPerBlock == 0)
            {
                m_config.chunksPerBlock = std::max(size_t(1), HUGE_PAGE_SIZE / m_config.chunkSize);
            }
            if (m_config.maxChunks < m_config.chunksPerBlock)
            {
                m_config.maxChunks = m_config.chunksPerBlock;
            }

            // initialBlocks pre-allocates arenas. A failure here is not fatal:
            // the pool stays usable and CreateChunk retries the growth later.
            for (size_t i = 0; i < m_config.initialBlocks; ++i)
            {
                if (!GrowArena(0)) ASTRA_UNLIKELY
                    break;
            }
        }

        ~ArchetypeChunkPool()
        {
            // The Tlsf heap only indexes memory it was handed; releasing the
            // arenas here is what actually returns it to the OS.
            for (const ArenaRecord& arena : m_arenas)
            {
                FreeMemory(arena.memory, arena.size, arena.usedHugePages);
            }
        }

        ArchetypeChunkPool(const ArchetypeChunkPool&) = delete;
        ArchetypeChunkPool& operator=(const ArchetypeChunkPool&) = delete;

        // Move is also deleted: outstanding chunks carry a ChunkDeleter with a
        // raw `pool` back-pointer into this instance. Moving would relocate the
        // pool while those back-pointers still point at the old (freed) address,
        // causing a use-after-free/double-free the next time such a chunk is
        // destroyed. ArchetypeManager (the sole owner) is itself non-movable, so
        // nothing depends on these.
        ArchetypeChunkPool(ArchetypeChunkPool&& other) = delete;
        ArchetypeChunkPool& operator=(ArchetypeChunkPool&& other) = delete;

        // Creates a chunk of `chunkBytes` bytes holding up to `capacity` entities.
        // The two are independent parameters: the caller (Archetype) owns the
        // capacity-for-bytes layout math, the pool only owns the allocation.
        std::unique_ptr<Chunk, ChunkDeleter> CreateChunk(size_t capacity, size_t chunkBytes, const ArchetypeColumnMeta* meta)
        {
            ASTRA_ASSERT(capacity > 0, "Chunk capacity must be positive");

            void* memory = AllocateChunkBytes(chunkBytes);
            if (!memory) ASTRA_UNLIKELY
            {
                return nullptr;
            }

            auto* chunk = new Chunk(capacity, meta, memory, chunkBytes);
            chunk->m_sparseWords = &m_sparseWords;
            ChunkDeleter deleter{this, memory};
            return std::unique_ptr<Chunk, ChunkDeleter>(chunk, deleter);
        }

        void ReturnChunk(void* memory)
        {
            if (!memory) ASTRA_UNLIKELY
                return;

            // Tlsf::Free is O(1) and coalesces with free physical neighbours, so
            // no side table from memory back to a node is needed any more.
            m_tlsf.Free(memory);

            ASTRA_ASSERT(m_totalChunks.load(std::memory_order_relaxed) > 0, "ReturnChunk without a matching CreateChunk");
            // Saturating decrement in ALL builds, not just Debug: ReturnChunk is
            // public, and the assert above compiles out in Release/Dist. An
            // unpaired or foreign call must not underflow m_totalChunks to
            // SIZE_MAX -- that would make AllocateChunkBytes's `m_totalChunks >=
            // m_config.maxChunks` gate permanently true, silently bricking the
            // pool (every future CreateChunk returns nullptr) for good.
            const size_t liveBefore = m_totalChunks.load(std::memory_order_relaxed);
            if (liveBefore > 0) ASTRA_LIKELY
            {
                m_totalChunks.fetch_sub(1, std::memory_order_relaxed);
            }
            m_releaseCount.fetch_add(1, std::memory_order_relaxed);
        }

        ASTRA_NODISCARD size_t GetChunkSize() const { return m_config.chunkSize; }
        ASTRA_NODISCARD size_t GetMinChunkBytes() const noexcept { return m_config.minChunkBytes; }
        ASTRA_NODISCARD size_t GetMaxChunkBytes() const noexcept { return m_config.maxChunkBytes; }
        ASTRA_NODISCARD size_t GetGrowDivisor() const noexcept { return m_config.growDivisor; }

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

        ASTRA_NODISCARD Stats GetStats() const
        {
            Stats snapshot;
            // m_totalChunks counts LIVE chunks now (the block pool counted carved
            // capacity). Reporting live + "how many more chunks the free bytes
            // could serve" keeps the old reading of totalChunks as pool capacity,
            // which callers use to see that a returned chunk is retained, not
            // released to the OS.
            //
            // freeEquivalent is an UPPER BOUND on additional chunks servable
            // without growing, NOT an achievable count: free bytes can be
            // scattered as sub-chunk-sized tails across several arenas (TLSF's
            // boundary tags + good-fit search), so some of this "capacity" may
            // not be contiguous enough to actually serve a chunk. Do not change
            // this formula -- MemoryCleanupTest's
            // `stats2.totalChunks == stats1.totalChunks` assertion depends on
            // its exact arithmetic.
            const size_t live = m_totalChunks.load(std::memory_order_relaxed);
            const size_t freeEquivalent = m_tlsf.GetFreeBytes() / m_config.chunkSize;  // upper bound, see above
            snapshot.totalChunks = live + freeEquivalent;
            snapshot.freeChunks = freeEquivalent;
            snapshot.acquireCount = m_acquireCount.load(std::memory_order_relaxed);
            snapshot.releaseCount = m_releaseCount.load(std::memory_order_relaxed);
            snapshot.blockAllocations = m_blockAllocations.load(std::memory_order_relaxed);
            snapshot.failedAcquires = m_failedAcquires.load(std::memory_order_relaxed);
            return snapshot;
        }

        struct DefragmentResult
        {
            size_t blocksReleased = 0;
            size_t bytesFreed = 0;
            size_t blocksKept = 0;
            size_t chunksInUse = 0;
        };
        
        DefragmentResult Defragment()
        {
            m_sparseWords.Trim();
            DefragmentResult result;

            // Collect first: ForEachFullyFreeArena walks Tlsf's arena list by
            // reference, so RemoveArena must not run inside the callback.
            SmallVector<void*, 8> releasable;
            m_tlsf.ForEachFullyFreeArena([&releasable](void* base, size_t) { releasable.push_back(base); });

            // Old block-pool policy, restored: always keep ONE fully-free arena
            // in reserve (anti-thrash) whenever at least one exists -- even
            // while other arenas are still in use serving live chunks. Only the
            // remaining fully-free arenas are released.
            const size_t startIndex = releasable.empty() ? 0 : 1;
            for (size_t i = startIndex; i < releasable.size(); ++i)
            {
                void* base = releasable[i];

                // Find this pool's own record for the arena BEFORE detaching it
                // from Tlsf. Tlsf only ever reports arenas this pool registered,
                // so a miss here would mean the two records have diverged; by
                // checking first (rather than removing from Tlsf then failing to
                // find the record) a divergence leaves the arena registered and
                // owned by Tlsf instead of detaching-and-leaking it with no way
                // back. Unreachable by construction today.
                size_t j = 0;
                for (; j < m_arenas.size(); ++j)
                {
                    if (m_arenas[j].memory == base)
                        break;
                }
                if (j >= m_arenas.size()) ASTRA_UNLIKELY
                {
                    ASTRA_ASSERT(false, "TLSF reported an arena the pool has no record of");
                    continue;   // leave it registered in Tlsf: no detach, no leak
                }

                if (!m_tlsf.RemoveArena(base)) ASTRA_UNLIKELY
                    continue;   // became carved up again: leave it registered

                result.bytesFreed += m_arenas[j].size;
                FreeMemory(m_arenas[j].memory, m_arenas[j].size, m_arenas[j].usedHugePages);
                m_arenas[j] = m_arenas.back();
                m_arenas.pop_back();
                ++result.blocksReleased;
            }

            result.blocksKept = m_arenas.size();
            result.chunksInUse = m_totalChunks.load(std::memory_order_relaxed);
            return result;
        }

    private:
        // One OS region registered with the Tlsf heap. Tlsf never calls the OS
        // itself, so the pool keeps what it needs to hand the region back.
        struct ArenaRecord
        {
            void* memory = nullptr;
            size_t size = 0;
            bool usedHugePages = false;
        };

        // Acquires one region from the OS and registers it with the heap.
        // minBytes is the allocation that triggered the growth; the new arena
        // must be able to serve it.
        bool GrowArena(size_t minBytes)
        {
            // Arena sizing: the legacy chunksPerBlock x chunkSize hint, at least
            // enough for the request plus TLSF bookkeeping (front pad 64 +
            // sentinel 16 + rounding; 256 is comfortably safe), and always inside
            // the range AddArena accepts.
            constexpr size_t kArenaOverhead = 256;
            // Checked against MAX_REQUEST_BYTES (32MB), not MAX_ARENA_BYTES
            // (64MB): Allocate refuses any request whose internally-rounded
            // size exceeds MAX_REQUEST_BYTES, so a minBytes in
            // (MAX_REQUEST_BYTES, MAX_ARENA_BYTES] would pass an assert against
            // the arena ceiling, register an arena successfully, and then fail
            // the retry Allocate anyway -- silently, in Release. Harmless at
            // today's <=1MB chunk sizes; becomes reachable once chunk sizes vary.
            ASTRA_ASSERT(minBytes + kArenaOverhead <= Tlsf::MAX_REQUEST_BYTES, "Chunk request larger than the largest request TLSF can service");
            size_t want = std::max(m_config.chunksPerBlock * m_config.chunkSize, minBytes + kArenaOverhead);
            want = std::clamp(want, Tlsf::MIN_ARENA_BYTES, Tlsf::MAX_ARENA_BYTES);

            // Do NOT pre-round to huge pages here: AllocateMemory already rounds
            // internally when it takes the huge-page path (size >= 2MB), and small
            // arena hints (e.g. chunksPerBlock=4 in tests) must stay small for
            // parity with the old per-block allocation. Register r.size (actual).
            // No ZeroMem either: Chunk's constructor memsets its own bytes.
            AllocFlags flags = AllocFlags::None;
            if (m_config.useHugePages)
            {
                flags = flags | AllocFlags::HugePages;
            }

            AllocResult r = AllocateMemory(want, CACHE_LINE_SIZE, flags);
            if (!r.ptr) ASTRA_UNLIKELY
                return false;

            // AllocateMemory is at least cache-line aligned, which is what
            // AddArena demands; refuse-and-release rather than leak if it is not.
            if (!m_tlsf.AddArena(r.ptr, r.size)) ASTRA_UNLIKELY
            {
                FreeMemory(r.ptr, r.size, r.usedHugePages);
                return false;
            }

            m_arenas.push_back(ArenaRecord{r.ptr, r.size, r.usedHugePages});
            m_blockAllocations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Single allocation path for chunk storage. CreateChunk forwards the
        // caller's per-chunk byte size, which no longer has to be m_config.chunkSize.
        void* AllocateChunkBytes(size_t chunkBytes)
        {
            if (m_totalChunks.load(std::memory_order_relaxed) >= m_config.maxChunks) ASTRA_UNLIKELY
            {
                m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }

            void* p = m_tlsf.Allocate(chunkBytes);
            if (!p)
            {
                if (!GrowArena(chunkBytes)) ASTRA_UNLIKELY
                {
                    m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
                p = m_tlsf.Allocate(chunkBytes);
                if (!p) ASTRA_UNLIKELY
                {
                    m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
            }

            m_totalChunks.fetch_add(1, std::memory_order_relaxed);
            m_acquireCount.fetch_add(1, std::memory_order_relaxed);
            return p;
        }

        // Zeroes the counters of a moved-from pool so its GetStats() cannot
        // report chunks it no longer owns (its Tlsf and arenas are empty).
        void ResetCounters() noexcept
        {
            m_totalChunks.store(0, std::memory_order_relaxed);
            m_acquireCount.store(0, std::memory_order_relaxed);
            m_releaseCount.store(0, std::memory_order_relaxed);
            m_blockAllocations.store(0, std::memory_order_relaxed);
            m_failedAcquires.store(0, std::memory_order_relaxed);
        }

        Config m_config;
        Tlsf m_tlsf;
        SmallVector<ArenaRecord, 16> m_arenas;
        SparseWordFreelist m_sparseWords;

        std::atomic<size_t> m_totalChunks{0};       // LIVE chunks (see GetStats)
        std::atomic<size_t> m_acquireCount{0};
        std::atomic<size_t> m_releaseCount{0};
        std::atomic<size_t> m_blockAllocations{0};  // arenas acquired from the OS
        std::atomic<size_t> m_failedAcquires{0};
    };
}
