// Shared hand-rolled timing harness for bench-compare (CSV: lib,op,N,ns_per_op,items).
//
// Authored on this branch with exactly the API bench_storage.cpp uses; it includes no
// third-party ECS headers. Timing protocol: wall clock (steady_clock), the MEDIAN of
// `rounds` measurements. report() divides a measured total by `items` to print ns/op.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

struct Position { float x, y, z; };
struct Velocity { float x, y, z; };

// Keeps measured loops observable; printed once at the end of main().
inline double g_sink = 0.0;

// Library label for the CSV's first column. Defined by the including executable.
extern const char* kLib;

namespace bench_detail
{
    using Clock = std::chrono::steady_clock;

    inline double Nanos(Clock::time_point a, Clock::time_point b)
    {
        return std::chrono::duration<double, std::nano>(b - a).count();
    }

    inline double Median(std::vector<double>& v)
    {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
}

// One CSV row. `total_ns` is one measured pass; `items` is how many operations it performed.
inline void report(const char* op, size_t n, double total_ns, size_t items)
{
    std::printf("%s,%s,%zu,%.3f,%zu\n", kLib, op, n, total_ns / double(items), items);
    std::fflush(stdout);
}

// Median total nanoseconds of `rounds` calls of fn() on state that survives between calls
// (one untimed warm-up first). For read-only / idempotent passes.
template<class Fn>
double median_persistent(int rounds, Fn&& fn)
{
    fn();
    std::vector<double> samples;
    samples.reserve(size_t(rounds));
    for (int i = 0; i < rounds; ++i)
    {
        const auto t0 = bench_detail::Clock::now();
        fn();
        samples.push_back(bench_detail::Nanos(t0, bench_detail::Clock::now()));
    }
    return bench_detail::Median(samples);
}

// Median total nanoseconds of run(state) over `rounds` rounds, each on a FRESH state from
// build() (construction is untimed; one untimed warm-up round first). For structural passes.
template<class Build, class Run>
double median_structural(int rounds, Build&& build, Run&& run)
{
    {
        auto warm = build();
        run(warm);
    }
    std::vector<double> samples;
    samples.reserve(size_t(rounds));
    for (int i = 0; i < rounds; ++i)
    {
        auto state = build();
        const auto t0 = bench_detail::Clock::now();
        run(state);
        samples.push_back(bench_detail::Nanos(t0, bench_detail::Clock::now()));
    }
    return bench_detail::Median(samples);
}

// A fixed (seeded) shuffle of 0..n-1: the cache-hostile access order.
inline std::vector<uint32_t> shuffled_indices(size_t n)
{
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), uint32_t(0));
    std::mt19937 rng(0xC0FFEEu);
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}
