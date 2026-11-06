#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#include "../Component/Component.hpp"
#include "../Entity/Entity.hpp"
#include "BinaryReader.hpp"
#include "BinaryWriter.hpp"

namespace Astra::StorageFormat
{
    // Format v6 ComponentStorage block (spec 2026-10-01 s8.2).
    inline constexpr uint8_t KIND_SPARSE    = 1;
    inline constexpr uint8_t KIND_UNIVERSAL = 2;

    // The flags byte records what the archive CARRIES (the IM-9 rule), so a reader
    // never infers stream layout from its own build's descriptor.
    inline constexpr uint8_t FLAG_HAS_DISABLED = 0x01;   // a disabled bitmap follows the values
    inline constexpr uint8_t FLAG_HAS_VALUES   = 0x02;   // per-element serializeVersioned values (absent for tags)
    inline constexpr uint8_t FLAG_MASK         = FLAG_HAS_DISABLED | FLAG_HAS_VALUES;

    // hash(8) + version(4) + kind(1) + flags(1) + elementCount(4)
    inline constexpr size_t BLOCK_HEADER_BYTES = 18;

    // The same section shape Archetype::SerializeColumn writes for an enableable
    // column: uint32 popcount, then max(1, count) bits rounded up to whole words.
    ASTRA_NODISCARD inline size_t DisabledWordCount(size_t count) noexcept
    {
        return (std::max<size_t>(1, count) + 63) / 64;
    }

    inline void WriteDisabledBitmap(BinaryWriter& w, const std::vector<uint8_t>& disabled)
    {
        std::vector<uint64_t> words(DisabledWordCount(disabled.size()), 0);
        uint32_t count = 0;
        for (size_t i = 0; i < disabled.size(); ++i)
        {
            if (!disabled[i]) continue;
            words[i >> 6] |= uint64_t{1} << (i & 63);
            ++count;
        }
        w(count);
        for (const uint64_t word : words) w(word);
    }

    // Refuse-not-trust, the same rule DeserializeColumn applies: the popcount must
    // match, and no bit may sit at or beyond `count`.
    ASTRA_NODISCARD inline bool ReadDisabledBitmap(BinaryReader& r, size_t count, std::vector<uint8_t>& out)
    {
        const size_t wordCount = DisabledWordCount(count);
        uint32_t diskCount = 0;
        r(diskCount);
        if (r.HasError() || r.CountExceedsRemaining(wordCount, sizeof(uint64_t)))
            return false;

        out.assign(count, 0);
        uint32_t popcount = 0;
        for (size_t w = 0; w < wordCount; ++w)
        {
            uint64_t word = 0;
            r(word);
            if (r.HasError())
                return false;
            popcount += static_cast<uint32_t>(std::popcount(word));
            for (uint64_t bits = word; bits != 0; bits &= bits - 1)
            {
                const size_t i = w * 64 + static_cast<size_t>(std::countr_zero(bits));
                if (i >= count)
                    return false;   // a bit beyond the live element count
                out[i] = 1;
            }
        }
        return popcount == diskCount;
    }

    // The shared "values, then the disabled bitmap" body of every reader that loads an
    // element run into a store (spec s8.2/s8.3): the v6 block reader and the diverted
    // Table-in-archive column. `acquire(i)` returns the constructed element to
    // deserialize over for the i-th entity (a sentinel for a tag; nullptr refuses) and
    // is called for every element, values or not; `setDisabled(i)` runs for each set bit
    // iff the loading type is enableable (otherwise the bitmap is validated and dropped,
    // IM-9). `hasValues`/`hasDisabled` are what the ARCHIVE carries, never this build's
    // descriptor: values this build's type cannot decode (a tag here) are refused.
    template<typename Acquire, typename SetDisabled>
    ASTRA_NODISCARD bool ReadElementRun(BinaryReader& r, size_t count, const ComponentDescriptor& desc,
                                        bool hasValues, bool hasDisabled,
                                        Acquire&& acquire, SetDisabled&& setDisabled)
    {
        if (hasValues && (desc.size == 0 || !desc.deserializeVersioned))
            return false;
        for (size_t i = 0; i < count; ++i)
        {
            void* target = acquire(i);
            if (!target)
                return false;
            if (hasValues && !desc.deserializeVersioned(r, target))
                return false;
        }
        if (hasDisabled)
        {
            std::vector<uint8_t> disabled;
            if (!ReadDisabledBitmap(r, count, disabled))
                return false;
            if (desc.isEnableable)
                for (size_t i = 0; i < count; ++i)
                    if (disabled[i]) setDisabled(i);
        }
        return !r.HasError();
    }
}

namespace Astra
{
    // Load-time diversion seam (spec s8.3). Archetype::Deserialize meets columns of
    // components the LOADING registry does not keep in tables and hands their
    // values to ComponentStorage through these callbacks (Archetype cannot see
    // Registry or ComponentStorage). Null hooks mean no diversion: every caller
    // that deserializes archetypes outside Registry::Load is unchanged.
    struct StorageLoadHooks
    {
        void* ctx = nullptr;
        bool  (*isDiverted)(void* ctx, ComponentID id) = nullptr;
        // Constructed element to deserialize over (EmptyComponentSentinel() for a
        // tag); nullptr refuses the load.
        void* (*acquire)(void* ctx, const ComponentDescriptor& desc, Entity e) = nullptr;
        void  (*setDisabled)(void* ctx, ComponentID id, Entity e) = nullptr;

        ASTRA_NODISCARD bool IsDiverted(ComponentID id) const { return isDiverted && isDiverted(ctx, id); }
    };

    // s8.3 bullet 2: values archived Sparse/Universal for a component the loading
    // registry keeps in tables. Elements are default-constructed in an aligned
    // buffer, deserialized over, and later moved into their destination columns by
    // ArchetypeManager::ApplyMigratedTableComponents. Whatever is not moved out is
    // destroyed with the column, so a failed load leaks nothing.
    class StagedTableColumn
    {
    public:
        StagedTableColumn(const ComponentDescriptor& desc, size_t count)
            : m_desc(desc), m_entities(count), m_disabled(count, 0), m_constructed(count, 0)
        {
            if (desc.size == 0 || count == 0)
                return;
            m_data = static_cast<std::byte*>(::operator new(desc.size * count, std::align_val_t{desc.alignment}));
            for (size_t i = 0; i < count; ++i)
            {
                desc.DefaultConstruct(m_data + i * desc.size);
                m_constructed[i] = 1;
            }
        }
        ~StagedTableColumn() { Release(); }

        StagedTableColumn(StagedTableColumn&& other) noexcept
            : m_desc(other.m_desc), m_data(std::exchange(other.m_data, nullptr)),
              m_entities(std::move(other.m_entities)), m_disabled(std::move(other.m_disabled)),
              m_constructed(std::move(other.m_constructed)) {}
        StagedTableColumn& operator=(StagedTableColumn&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_desc = other.m_desc;
                m_data = std::exchange(other.m_data, nullptr);
                m_entities = std::move(other.m_entities);
                m_disabled = std::move(other.m_disabled);
                m_constructed = std::move(other.m_constructed);
            }
            return *this;
        }
        StagedTableColumn(const StagedTableColumn&) = delete;
        StagedTableColumn& operator=(const StagedTableColumn&) = delete;

        ASTRA_NODISCARD const ComponentDescriptor& Descriptor() const noexcept { return m_desc; }
        ASTRA_NODISCARD size_t Size() const noexcept { return m_entities.size(); }
        ASTRA_NODISCARD Entity& EntityAt(size_t i) noexcept { return m_entities[i]; }
        ASTRA_NODISCARD Entity EntityAt(size_t i) const noexcept { return m_entities[i]; }
        ASTRA_NODISCARD void* At(size_t i) noexcept { return m_data ? m_data + i * m_desc.size : EmptyComponentSentinel(); }
        ASTRA_NODISCARD bool IsDisabled(size_t i) const noexcept { return m_disabled[i] != 0; }
        void SetDisabled(size_t i, bool disabled) noexcept { m_disabled[i] = disabled ? 1 : 0; }

        // Move-construct element i into raw column memory, then destroy the staged one.
        void MoveOut(size_t i, void* dst)
        {
            if (!m_data || !m_constructed[i])
                return;
            m_desc.MoveConstruct(dst, At(i));
            m_desc.Destruct(At(i));
            m_constructed[i] = 0;
        }

    private:
        void Release() noexcept
        {
            if (!m_data)
                return;
            for (size_t i = 0; i < m_constructed.size(); ++i)
                if (m_constructed[i]) m_desc.Destruct(m_data + i * m_desc.size);
            ::operator delete(m_data, std::align_val_t{m_desc.alignment});
            m_data = nullptr;
        }

        ComponentDescriptor m_desc;          // by value: the ResourceStorage lifetime rule
        std::byte* m_data = nullptr;
        std::vector<Entity> m_entities;
        std::vector<uint8_t> m_disabled;
        std::vector<uint8_t> m_constructed;
    };
}
