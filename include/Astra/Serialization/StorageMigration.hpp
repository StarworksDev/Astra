#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

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
}
