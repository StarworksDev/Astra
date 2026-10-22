#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
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
}
