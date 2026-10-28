#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "../Core/Base.hpp"

namespace Astra::Detail
{
    /**
     * Match word `w` of the generalised extractor (spec 2026-10-01 s6.3):
     * AND over every require set, AND NOT the OR over every exclude set, with the
     * bits at or beyond `count` masked off. With no sets at all every valid bit is set.
     */
    ASTRA_FORCEINLINE uint64_t MatchWord(const uint64_t* const* exclude, size_t nExclude,
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
     * Generalised run extraction (spec 2026-10-01 s6.3), pull form: Next() yields every
     * maximal run [begin, end) of indices in [0, count) whose bit is set in EVERY `require`
     * set and in NO `exclude` set, ascending and stitched across word boundaries, then
     * false (and false again on every later call). Each set points at (count + 63) / 64
     * words. Tail bits are masked, so whole-word regions may be passed. Both lists empty:
     * the single run [0, count).
     *
     * Pull form, so a walk can be suspended between entities: the storage-path chunk walk
     * keeps one in its per-chunk state and steps it from an out-of-line helper that never
     * receives the user's functor (View::NextStorageEntity, Task 42 fix 2).
     */
    class MatchingRunCursor
    {
    public:
        MatchingRunCursor() noexcept = default;   // exhausted: Next() is false

        ASTRA_FORCEINLINE MatchingRunCursor(const uint64_t* const* exclude, size_t nExclude,
                          const uint64_t* const* require, size_t nRequire, size_t count) noexcept
            : m_exclude(exclude), m_require(require), m_nExclude(nExclude), m_nRequire(nRequire),
              m_count(count), m_numWords((count + 63) >> 6)
        {
        }

        ASTRA_FORCEINLINE bool Next(size_t& begin, size_t& end) noexcept
        {
            for (;;)
            {
                if (m_bits == 0ull)
                {
                    if (m_word >= m_numWords)
                    {
                        if (!m_open)
                            return false;
                        m_open = false;                      // a run reaching the last valid bit
                        begin = m_openStart;
                        end = m_count;
                        return true;
                    }
                    m_bits = MatchWord(m_exclude, m_nExclude, m_require, m_nRequire, m_word, m_count);
                    m_base = m_word << 6;
                    ++m_word;

                    // Stitch a run carried over from the previous word.
                    if (m_open)
                    {
                        if (m_bits & 1ull)
                        {
                            const int cont = std::countr_one(m_bits);
                            if (cont == 64)
                            {
                                m_bits = 0ull;               // whole word continues the run
                                continue;
                            }
                            m_open = false;
                            begin = m_openStart;
                            end = m_base + static_cast<size_t>(cont);
                            m_bits &= ~((uint64_t(1) << cont) - 1);   // consume the leading ones
                            return true;
                        }
                        m_open = false;                      // run ended at the word boundary
                        begin = m_openStart;
                        end = m_base;
                        return true;
                    }
                    continue;
                }

                // The next maximal one-run inside the current word.
                const int start = std::countr_zero(m_bits);
                const int len   = std::countr_one(m_bits >> start);
                const int stop  = start + len;               // exclusive, within (0, 64]
                if (stop == 64)
                {
                    m_open = true;                           // reaches the top: carry to the next word
                    m_openStart = m_base + static_cast<size_t>(start);
                    m_bits = 0ull;
                    continue;
                }
                begin = m_base + static_cast<size_t>(start);
                end = m_base + static_cast<size_t>(stop);
                m_bits &= ~(((uint64_t(1) << len) - 1) << start);   // clear [start, stop)
                return true;
            }
        }

    private:
        const uint64_t* const* m_exclude = nullptr;
        const uint64_t* const* m_require = nullptr;
        size_t   m_nExclude = 0;
        size_t   m_nRequire = 0;
        size_t   m_count = 0;
        size_t   m_numWords = 0;
        size_t   m_word = 0;           // next word to load
        size_t   m_base = 0;           // first index of the current word
        uint64_t m_bits = 0;           // unconsumed match bits of the current word
        size_t   m_openStart = 0;
        bool     m_open = false;       // a run left open at the top of the previous word
    };

    /**
     * Callback form of MatchingRunCursor: invokes `fn(begin, end)` for every run, in order.
     */
    template<class Fn>
    inline void ForEachMatchingRun(const uint64_t* const* exclude, size_t nExclude,
                                   const uint64_t* const* require, size_t nRequire,
                                   size_t count, Fn&& fn)
    {
        MatchingRunCursor cursor(exclude, nExclude, require, nRequire, count);
        size_t begin = 0, end = 0;
        while (cursor.Next(begin, end))
            fn(begin, end);
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
