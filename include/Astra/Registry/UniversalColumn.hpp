#pragma once

#include <bit>
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
     * Universal-component storage (spec 2026-10-01 s4.2). There is one per component
     * that a registry declares universal.
     *
     * Layout: a paged, slot-indexed column with no indirection. The value of entity e
     * lives on page e.GetID() / PAGE_SLOTS, at slot e.GetID() % PAGE_SLOTS.
     *
     * Each page holds:
     *  - the aligned data block (none for an empty type);
     *  - a live bitmap;
     *  - the full Entity handle per slot, so a stale handle misses on its version;
     *  - a live count;
     *  - EntityTicks, iff the type is change-tracked;
     *  - a disabled bitmap, iff the type is enableable;
     *  - one coarse version Tick: the untracked change-detection tier, the twin of a
     *    chunk's column version.
     *
     * Pages are allocated when an id first lands in their range, and released when their
     * live count reaches zero. The store is type-erased through a descriptor held BY
     * VALUE (the ResourceStorage lifetime pattern). Values never move.
     */
    class UniversalColumn
    {
    public:
        static constexpr std::size_t PAGE_SLOTS = static_cast<std::size_t>(ASTRA_UNIVERSAL_PAGE_SLOTS);
        static constexpr std::size_t PAGE_WORDS = PAGE_SLOTS / 64;
        static_assert(PAGE_SLOTS >= 64 && std::has_single_bit(PAGE_SLOTS),
                      "ASTRA_UNIVERSAL_PAGE_SLOTS must be a power of two >= 64");

        explicit UniversalColumn(const ComponentDescriptor& desc) : m_desc(desc) {}
        ~UniversalColumn() { Clear(); }

        UniversalColumn(const UniversalColumn&) = delete;
        UniversalColumn& operator=(const UniversalColumn&) = delete;
        UniversalColumn(UniversalColumn&&) = delete;
        UniversalColumn& operator=(UniversalColumn&&) = delete;

        // The change-detection counter that construction stamps from
        // (ArchetypeManager::TickSource()). Null reads as tick 0 ("never").
        void SetTickSource(const Tick* tick) noexcept { m_tick = tick; }

        void* Construct(Entity e) { return ConstructImpl(e, nullptr); }
        void* ConstructMove(Entity e, void* src) { return ConstructImpl(e, src); }

        // Destroys e's value. An absent or stale handle is a no-op: an older version of a
        // recycled id never reaches the current occupant.
        void Destroy(Entity e)
        {
            Page* page = FindLive(e);
            if (!page) ASTRA_UNLIKELY
                return;
            const std::size_t slot = SlotOf(e);
            if (page->data)
                m_desc.Destruct(page->data + slot * m_desc.size);
            ClearBit(page->live, slot);
            if (page->disabled)
                ClearBit(page->disabled.get(), slot);
            page->entities[slot] = Entity::Invalid();
            --page->liveCount;
            --m_liveCount;
            if (page->liveCount == 0)
                m_pages[PageOf(e)].reset();
        }

        ASTRA_NODISCARD void* Get(Entity e) noexcept
        {
            Page* page = FindLive(e);
            return page ? SlotPtr(*page, SlotOf(e)) : nullptr;
        }

        ASTRA_NODISCARD const void* Get(Entity e) const noexcept
        {
            Page* page = FindLive(e);
            return page ? SlotPtr(*page, SlotOf(e)) : nullptr;
        }

        ASTRA_NODISCARD bool IsLive(Entity e) const noexcept { return FindLive(e) != nullptr; }

        ASTRA_NODISCARD bool IsDisabled(Entity e) const noexcept
        {
            const Page* page = FindLive(e);
            return page && page->disabled && TestBit(page->disabled.get(), SlotOf(e));
        }

        // SET == DISABLED, matching the chunk bits. Returns true IFF the state changed.
        // Returns false for an absent or stale handle, or a non-enableable type.
        bool SetDisabled(Entity e, bool disabled) noexcept
        {
            Page* page = FindLive(e);
            if (!page || !page->disabled) ASTRA_UNLIKELY
                return false;
            const std::size_t slot = SlotOf(e);
            if (TestBit(page->disabled.get(), slot) == disabled)
                return false;
            if (disabled) SetBit(page->disabled.get(), slot);
            else          ClearBit(page->disabled.get(), slot);
            return true;
        }

        ASTRA_NODISCARD EntityTicks* Ticks(Entity e) noexcept
        {
            Page* page = FindLive(e);
            return (page && page->ticks) ? &page->ticks[SlotOf(e)] : nullptr;
        }

        // Write-side stamp (Registry::GetComponent / Modified / SetIfNeq): bump the page
        // version and, for a tracked type, the slot's `changed`. Returns false for an
        // absent or stale handle, or for a tag, which has no value (the Table rule for a
        // column-less tag).
        bool MarkWritten(Entity e, Tick now) noexcept
        {
            if (m_desc.size == 0) ASTRA_UNLIKELY
                return false;
            Page* page = FindLive(e);
            if (!page) ASTRA_UNLIKELY
                return false;
            page->version = now;
            if (page->ticks)
                page->ticks[SlotOf(e)].changed = now;
            return true;
        }

        // Exact for a tracked type. For an untracked type, the page version is returned:
        // every live slot of a stamped page answers it, the same trade a chunk's column
        // version makes. Returns 0 ("never") when e is not live.
        ASTRA_NODISCARD Tick AddedTick(Entity e) const noexcept
        {
            const Page* page = FindLive(e);
            if (!page) return Tick{0};
            return page->ticks ? page->ticks[SlotOf(e)].added : page->version;
        }

        ASTRA_NODISCARD Tick ChangedTick(Entity e) const noexcept
        {
            const Page* page = FindLive(e);
            if (!page) return Tick{0};
            return page->ticks ? page->ticks[SlotOf(e)].changed : page->version;
        }

        ASTRA_NODISCARD std::size_t LiveCount() const noexcept { return m_liveCount; }

        ASTRA_NODISCARD std::size_t PageCount() const noexcept
        {
            std::size_t n = 0;
            for (const auto& page : m_pages)
                n += page ? 1u : 0u;
            return n;
        }

        // fn(Entity, const void* component) for every live slot, in ascending index
        // order. Walks the live bitmaps, not the EntityManager (Clear, destroy and
        // serialization use it).
        template<class Fn>
        void ForEachLive(Fn&& fn) const
        {
            for (const auto& owned : m_pages)
            {
                const Page* page = owned.get();
                if (!page)
                    continue;
                for (std::size_t w = 0; w < PAGE_WORDS; ++w)
                {
                    std::uint64_t bits = page->live[w];
                    while (bits != 0)
                    {
                        const std::size_t slot = (w << 6) + static_cast<std::size_t>(std::countr_zero(bits));
                        bits &= bits - 1;
                        fn(page->entities[slot], static_cast<const void*>(SlotPtr(*page, slot)));
                    }
                }
            }
        }

        // Destroys every live value and releases every page. The column itself, with its
        // descriptor and tick source, stays usable.
        void Clear()
        {
            if (m_desc.size > 0 && !m_desc.is_trivially_destructible)
                ForEachLive([this](Entity, const void* p) { m_desc.Destruct(const_cast<void*>(p)); });
            m_pages.clear();
            m_liveCount = 0;
        }

        ASTRA_NODISCARD const ComponentDescriptor& Descriptor() const noexcept { return m_desc; }

    private:
        struct Page
        {
            std::byte* data = nullptr;                    // PAGE_SLOTS * size, desc.alignment-aligned; null for a tag
            std::unique_ptr<Entity[]> entities;           // value-initialised: Entity::Invalid()
            std::unique_ptr<EntityTicks[]> ticks;         // iff change-tracked; zeroed (tick 0 == never)
            std::unique_ptr<std::uint64_t[]> disabled;    // iff enableable; zeroed (born enabled)
            std::uint64_t live[PAGE_WORDS] = {};
            std::uint32_t liveCount = 0;
            Tick version = 0;
            std::size_t alignment = 1;

            explicit Page(const ComponentDescriptor& desc) :
                entities(std::make_unique<Entity[]>(PAGE_SLOTS)),
                alignment(desc.alignment)
            {
                if (desc.size > 0)
                    data = static_cast<std::byte*>(::operator new(desc.size * PAGE_SLOTS, std::align_val_t{desc.alignment}));
                if (desc.isChangeTracked)
                    ticks = std::make_unique<EntityTicks[]>(PAGE_SLOTS);
                if (desc.isEnableable)
                    disabled = std::make_unique<std::uint64_t[]>(PAGE_WORDS);
            }

            ~Page()
            {
                if (data)
                    ::operator delete(data, std::align_val_t{alignment});
            }

            Page(const Page&) = delete;
            Page& operator=(const Page&) = delete;
        };

        static std::size_t PageOf(Entity e) noexcept { return static_cast<std::size_t>(e.GetID()) / PAGE_SLOTS; }
        static std::size_t SlotOf(Entity e) noexcept { return static_cast<std::size_t>(e.GetID()) % PAGE_SLOTS; }

        static bool TestBit(const std::uint64_t* words, std::size_t i) noexcept { return ((words[i >> 6] >> (i & 63)) & 1ull) != 0; }
        static void SetBit(std::uint64_t* words, std::size_t i) noexcept { words[i >> 6] |= (1ull << (i & 63)); }
        static void ClearBit(std::uint64_t* words, std::size_t i) noexcept { words[i >> 6] &= ~(1ull << (i & 63)); }

        // The page holding e's value IFF that slot is live AND holds exactly this handle
        // (version included). Every accessor funnels through here, which is what makes a
        // stale handle miss.
        Page* FindLive(Entity e) const noexcept
        {
            if (!e.IsValid()) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t pageIndex = PageOf(e);
            if (pageIndex >= m_pages.size()) ASTRA_UNLIKELY
                return nullptr;
            Page* page = m_pages[pageIndex].get();
            if (!page) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t slot = SlotOf(e);
            if (!TestBit(page->live, slot) || page->entities[slot] != e) ASTRA_UNLIKELY
                return nullptr;
            return page;
        }

        void* SlotPtr(const Page& page, std::size_t slot) const noexcept
        {
            if (m_desc.size == 0)
                return EmptyComponentSentinel();   // present tag: non-null, never dereferenced
            return page.data + slot * m_desc.size;
        }

        // s4.2 step 1. `src` == nullptr: default-construct; otherwise move-construct from it.
        void* ConstructImpl(Entity e, void* src)
        {
            ASTRA_ASSERT(e.IsValid(), "UniversalColumn::Construct: invalid entity");
            if (!e.IsValid()) ASTRA_UNLIKELY
                return nullptr;
            const std::size_t pageIndex = PageOf(e);
            if (pageIndex >= m_pages.size())
                m_pages.resize(pageIndex + 1);
            std::unique_ptr<Page>& owned = m_pages[pageIndex];
            if (!owned)
                owned = std::make_unique<Page>(m_desc);
            Page& page = *owned;
            const std::size_t slot = SlotOf(e);
            if (TestBit(page.live, slot)) ASTRA_UNLIKELY
            {
                // Precondition breach: the previous occupant was never destroyed (a destroy
                // path missed the store). In Release, destroy it rather than leak it.
                ASTRA_ASSERT(false, "UniversalColumn::Construct over a live slot: a destroy path missed the store");
                if (page.data)
                    m_desc.Destruct(page.data + slot * m_desc.size);
                --page.liveCount;
                --m_liveCount;
            }
            if (page.data)
            {
                void* ptr = page.data + slot * m_desc.size;
                if (src) m_desc.MoveConstruct(ptr, src);
                else     m_desc.DefaultConstruct(ptr);
            }
            SetBit(page.live, slot);
            if (page.disabled)
                ClearBit(page.disabled.get(), slot);   // born enabled
            page.entities[slot] = e;
            ++page.liveCount;
            ++m_liveCount;
            const Tick now = m_tick ? *m_tick : Tick{0};
            page.version = now;                        // the AddEntity StampAllColumns twin
            if (page.ticks)
                page.ticks[slot] = EntityTicks{now, now};
            return SlotPtr(page, slot);
        }

        ComponentDescriptor m_desc;
        std::vector<std::unique_ptr<Page>> m_pages;
        std::size_t m_liveCount = 0;
        const Tick* m_tick = nullptr;
    };
}
