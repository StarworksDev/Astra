#include <gtest/gtest.h>
#include <Astra/Registry/EnabledRuns.hpp>
#include <random>
#include <utility>
#include <vector>

namespace
{
    using Runs = std::vector<std::pair<size_t, size_t>>;

    // Brute-force reference: index i matches iff every require set has bit i and no
    // exclude set has it. Runs are maximal and ascending.
    Runs Reference(const std::vector<std::vector<uint64_t>>& ex, const std::vector<std::vector<uint64_t>>& req, size_t count)
    {
        Runs runs;
        auto bit = [](const std::vector<uint64_t>& w, size_t i) { return ((w[i >> 6] >> (i & 63)) & 1ull) != 0; };
        size_t i = 0;
        while (i < count)
        {
            auto match = [&](size_t k)
            {
                for (const auto& r : req) if (!bit(r, k)) return false;
                for (const auto& e : ex)  if (bit(e, k))  return false;
                return true;
            };
            if (!match(i)) { ++i; continue; }
            const size_t b = i;
            while (i < count && match(i)) ++i;
            runs.emplace_back(b, i);
        }
        return runs;
    }

    std::vector<const uint64_t*> Ptrs(const std::vector<std::vector<uint64_t>>& sets)
    {
        std::vector<const uint64_t*> p;
        for (const auto& s : sets) p.push_back(s.data());
        return p;
    }

    Runs Collect(const std::vector<std::vector<uint64_t>>& ex, const std::vector<std::vector<uint64_t>>& req, size_t count)
    {
        Runs runs;
        auto pe = Ptrs(ex);
        auto pr = Ptrs(req);
        Astra::Detail::ForEachMatchingRun(pe.data(), pe.size(), pr.data(), pr.size(), count,
            [&](size_t b, size_t e) { runs.emplace_back(b, e); });
        return runs;
    }

    // The same runs pulled one at a time (the storage-path chunk walk's form).
    Runs CollectCursor(const std::vector<std::vector<uint64_t>>& ex, const std::vector<std::vector<uint64_t>>& req, size_t count)
    {
        Runs runs;
        auto pe = Ptrs(ex);
        auto pr = Ptrs(req);
        Astra::Detail::MatchingRunCursor cursor(pe.data(), pe.size(), pr.data(), pr.size(), count);
        size_t b = 0, e = 0;
        while (cursor.Next(b, e))
            runs.emplace_back(b, e);
        EXPECT_FALSE(cursor.Next(b, e)) << "an exhausted cursor stays exhausted";
        return runs;
    }
}

TEST(MatchingRuns, CursorPullsTheSameRunsAsTheCallbackWalk)
{
    EXPECT_EQ(CollectCursor({}, {}, 130), (Runs{{0, 130}}));
    EXPECT_TRUE(CollectCursor({}, {}, 0).empty());

    std::vector<uint64_t> req(3, 0);
    req[0] = 0xF000000000000001ull;   // bit 0, bits 60..63
    req[1] = ~0ull;                   // a whole word continues the open run
    req[2] = 0x0000000000000007ull;   // bits 128..130
    EXPECT_EQ(CollectCursor({}, {req}, 192), (Runs{{0, 1}, {60, 131}}));

    std::vector<uint64_t> ex(1, 0b0010'0100ull);
    EXPECT_EQ(CollectCursor({ex}, {std::vector<uint64_t>(1, 0xFFull)}, 8), (Runs{{0, 2}, {3, 5}, {6, 8}}));
    EXPECT_EQ(CollectCursor({}, {std::vector<uint64_t>(1, ~0ull)}, 64), (Runs{{0, 64}}));   // open run closed at count
}

TEST(MatchingRuns, NoSetsYieldsOneWholeRun)
{
    EXPECT_EQ(Collect({}, {}, 130), (Runs{{0, 130}}));
    EXPECT_TRUE(Collect({}, {}, 0).empty());
}

TEST(MatchingRuns, RequireOnlyStitchesAcrossTheWordBoundary)
{
    std::vector<uint64_t> req(2, 0);
    req[0] = 0xF000000000000000ull;   // bits 60..63
    req[1] = 0x0000000000000007ull;   // bits 64..66
    EXPECT_EQ(Collect({}, {req}, 128), (Runs{{60, 67}}));
}

TEST(MatchingRuns, RequireAndExcludeCombine)
{
    std::vector<uint64_t> req(1, 0b1111'1111ull);
    std::vector<uint64_t> ex(1, 0b0010'0100ull);
    EXPECT_EQ(Collect({ex}, {req}, 8), (Runs{{0, 2}, {3, 5}, {6, 8}}));
}

TEST(MatchingRuns, GarbageTailBitsAreMasked)
{
    std::vector<uint64_t> req(1, ~0ull);   // every bit set, including beyond count
    EXPECT_EQ(Collect({}, {req}, 10), (Runs{{0, 10}}));
    EXPECT_EQ(Astra::Detail::CountMatching(nullptr, 0, Ptrs({req}).data(), 1, 10), 10u);
}

TEST(MatchingRuns, RandomSetsAgreeWithTheReferenceAndTheHelpers)
{
    std::mt19937_64 rng(0xA57A);
    for (int round = 0; round < 300; ++round)
    {
        const size_t count = 1 + rng() % 400;
        const size_t words = (count + 63) / 64;
        auto randomSet = [&] { std::vector<uint64_t> s(words); for (auto& w : s) w = rng() | rng(); return s; };
        std::vector<std::vector<uint64_t>> ex(rng() % 3), req(rng() % 3);
        for (auto& s : ex)  s = randomSet();
        for (auto& s : req) s = randomSet();

        const Runs expected = Reference(ex, req, count);
        ASSERT_EQ(Collect(ex, req, count), expected) << "round " << round;
        ASSERT_EQ(CollectCursor(ex, req, count), expected) << "round " << round;

        size_t total = 0;
        for (auto [b, e] : expected) total += e - b;
        auto pe = Ptrs(ex);
        auto pr = Ptrs(req);
        ASSERT_EQ(Astra::Detail::CountMatching(pe.data(), pe.size(), pr.data(), pr.size(), count), total);

        std::vector<uint64_t> out(words, 0xDEADull);
        Astra::Detail::WriteMatchWords(pe.data(), pe.size(), pr.data(), pr.size(), count, out.data());
        ASSERT_EQ(Collect({}, {out}, count), expected) << "round " << round;
        if (count % 64) ASSERT_EQ(out.back() >> (count % 64), 0ull);   // tail cleared

        // The kept wrapper is exclude-only and agrees with ForEachMatchingRun.
        Runs viaWrapper;
        Astra::Detail::ForEachEnabledRun(pe.data(), pe.size(), count, [&](size_t b, size_t e) { viaWrapper.emplace_back(b, e); });
        ASSERT_EQ(viaWrapper, Reference(ex, {}, count));
    }
}
