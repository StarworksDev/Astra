#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Astra::Detail
{
    /**
     * Match word `w` of the generalised extractor (spec 2026-10-01 s6.3):
     * AND over every require set, AND NOT the OR over every exclude set, with the
     * bits at or beyond `count` masked off. With no sets at all every valid bit is set.
     */
    inline uint64_t MatchWord(const uint64_t* const* exclude, size_t nExclude,
                              const uint64_t* const* require, size_t nRequire,
                              size_t w, size_t count) noexcept
    {
        uint64_t m = ~uint64_t(0);
        for (size_t k = 0; k < nRequire; ++k)
            m &= require[k][w];
        uint64_t ex = 0;
        for (size_t k = 0; k < nExclude; ++k)
            ex |= exclude[k][w];
        m &= ~ex;
        const size_t validBits = count - (w << 6);       // in [1, 64] for the last word
        if (validBits < 64)
            m &= (uint64_t(1) << validBits) - 1;
        return m;
    }

    /**
     * Generalised run extraction (spec 2026-10-01 s6.3). Invokes `fn(begin, end)` for
     * every maximal run of indices in [0, count) whose bit is set in EVERY `require`
     * set and in NO `exclude` set. Runs are ascending and stitched across word
     * boundaries, exactly like the enabled-run walk this generalises. Each set points
     * at (count + 63) / 64 words. Tail bits are masked, so whole-word regions may be
     * passed. Both lists empty: the single run [0, count).
     */
    template<class Fn>
    inline void ForEachMatchingRun(const uint64_t* const* exclude, size_t nExclude,
                                   const uint64_t* const* require, size_t nRequire,
                                   size_t count, Fn&& fn)
    {
        if (count == 0)
            return;

        const size_t numWords = (count + 63) >> 6;
        bool   open = false;    // a run left open at the top of the previous word
        size_t openStart = 0;

        for (size_t w = 0; w < numWords; ++w)
        {
            uint64_t e = MatchWord(exclude, nExclude, require, nRequire, w, count);
            const size_t base = w << 6;

            // Stitch a run carried over from the previous word.
            if (open)
            {
                if (e & 1ull)
                {
                    const int cont = std::countr_one(e);
                    if (cont == 64)
                        continue;                            // whole word continues the run
                    fn(openStart, base + static_cast<size_t>(cont));
                    open = false;
                    e &= ~((uint64_t(1) << cont) - 1);       // consume the leading ones
                }
                else
                {
                    fn(openStart, base);                     // run ended at the word boundary
                    open = false;
                }
            }

            // Scan the remaining maximal one-runs inside this word.
            while (e != 0ull)
            {
                const int start = std::countr_zero(e);
                const int len   = std::countr_one(e >> start);
                const int end   = start + len;               // exclusive, within (0, 64]
                if (end == 64)
                {
                    open = true;                             // reaches the top: carry to next word
                    openStart = base + static_cast<size_t>(start);
                    break;
                }
                fn(base + static_cast<size_t>(start), base + static_cast<size_t>(end));
                e &= ~(((uint64_t(1) << len) - 1) << start); // clear [start, end)
            }
        }

        if (open)
            fn(openStart, count);
    }

    /**
     * Enabled-run extraction for enableable-components query filtering (spec §5).
     *
     * Given `setCount` word-sets (each a pointer to `(count + 63) / 64` 64-bit
     * words where a SET bit == DISABLED), invoke `fn(size_t begin, size_t end)`
     * for every maximal run of indices in `[0, count)` where NO set has a bit
     * set -- i.e. every maximal span of ENABLED-in-all entities. Runs are visited
     * in ascending index order and are stitched across word boundaries so a span
     * of enabled entities that straddles bit 63/64 is a single call (invariant:
     * visit order == ascending index within runs).
     *
     * `setCount == 0` yields the single run `[0, count)` (no constraint). Bits at
     * or beyond `count` in the final word are masked off, so callers may pass
     * whole-word regions without pre-clearing the tail.
     *
     * Pure + header-only so it is unit-testable through the view tests and shared
     * by both the serial and parallel filtered iteration paths. Uses the Mosaic
     * BitSet countr_zero/countr_one scan idiom generalised to ranges.
     *
     * Now a one-line wrapper over ForEachMatchingRun (exclude sets only), so every
     * existing caller is unchanged.
     */
    template<class Fn>
    inline void ForEachEnabledRun(const uint64_t* const* wordSets, size_t setCount,
                                  size_t count, Fn&& fn)
    {
        ForEachMatchingRun(wordSets, setCount, nullptr, 0, count, std::forward<Fn>(fn));
    }

    // Number of matching indices (View::Size over a filtered chunk).
    inline size_t CountMatching(const uint64_t* const* exclude, size_t nExclude,
                                const uint64_t* const* require, size_t nRequire, size_t count) noexcept
    {
        size_t total = 0;
        const size_t numWords = (count + 63) >> 6;
        for (size_t w = 0; w < numWords; ++w)
            total += static_cast<size_t>(std::popcount(MatchWord(exclude, nExclude, require, nRequire, w, count)));
        return total;
    }

    // Materialise the match words into `out` ((count + 63) / 64 words, tail cleared):
    // the per-chunk mask a range-for iterator steps through (Task 29).
    inline void WriteMatchWords(const uint64_t* const* exclude, size_t nExclude,
                                const uint64_t* const* require, size_t nRequire,
                                size_t count, uint64_t* out) noexcept
    {
        const size_t numWords = (count + 63) >> 6;
        for (size_t w = 0; w < numWords; ++w)
            out[w] = MatchWord(exclude, nExclude, require, nRequire, w, count);
    }
} // namespace Astra::Detail
