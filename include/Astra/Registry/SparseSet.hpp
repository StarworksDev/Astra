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

        // Precondition: `e` is the id's LIVE handle (the caller checked IsValid / the entity
        // record). The stale-entry eviction branch assumes the held handle is older.
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

        // Per-element ticks of exactly this handle: nullptr unless `e` is a current member
        // and the type is change-tracked (mirrors UniversalColumn::Ticks).
        ASTRA_NODISCARD EntityTicks* Ticks(Entity e) noexcept
        {
            const std::uint32_t d = Find(e);
            return (d != INVALID_INDEX && m_desc.isChangeTracked) ? &m_ticks[d] : nullptr;
        }

        ASTRA_NODISCARD const EntityTicks* Ticks(Entity e) const noexcept
        {
            const std::uint32_t d = Find(e);
            return (d != INVALID_INDEX && m_desc.isChangeTracked) ? &m_ticks[d] : nullptr;
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
            if (m_desc.size > 0 && !m_desc.is_trivially_destructible)
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
