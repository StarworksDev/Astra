// Storage-policy ops for the hand-rolled harness (spec 2026-10-01 s9.3), same protocol as
// bench_astra.cpp (bench_common.hpp: CSV lib,op,N,ns_per_op,items). Astra-only: the storage
// comparisons are internal (Table vs Sparse vs Universal); EnTT and flecs have no Table/Sparse
// switch for one type and no per-registry universal column.
// The component types below repeat benchmark/Benchmark.cpp's on purpose: independent executables.
#include "bench_common.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <Astra/Astra.hpp>

const char* kLib = "astra-storage";

namespace
{
    struct BenchIdentity   // Arcane::Identity shape: 48 B on MSVC x64 (release STL), non-trivially copyable
    {
        uint64_t guidHi = 0;
        uint64_t guidLo = 0;
        std::string name;

        template<typename Archive>
        void Serialize(Archive& ar) { ar(guidHi)(guidLo)(name); }
    };
#ifdef NDEBUG
    static_assert(sizeof(BenchIdentity) == 48, "BenchIdentity must be the 48 B Arcane::Identity shape (MSVC x64 release STL)");
#endif
    struct ChurnT { uint64_t a, b; };
    struct ChurnS { static constexpr auto AstraStorage = Astra::Storage::Sparse; uint64_t a, b; };

    template<class... Ts>
    std::shared_ptr<Astra::ComponentRegistry> Components()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Ts...>();
        return creg;
    }

    bool Holds(size_t i, int pct) { return ((i * 2654435761ull) >> 7) % 100 < static_cast<size_t>(pct); }

    struct State { std::unique_ptr<Astra::Registry> reg; std::vector<Astra::Entity> ents; };

    template<bool Universal>
    State BuildIdentity(size_t N)
    {
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<BenchIdentity>();
        State s;
        s.reg = std::make_unique<Astra::Registry>(Components<Position, Velocity, BenchIdentity>(), cfg);
        s.ents.reserve(N);
        for (size_t i = 0; i < N; ++i)
            s.ents.push_back(s.reg->CreateEntityWith(Position{float(i), 0.0f, 0.0f},
                                                     BenchIdentity{i, ~uint64_t(i), "Entity " + std::to_string(i)}));
        return s;
    }

    template<class C>
    State BuildPositions(size_t N, int pct)
    {
        State s;
        s.reg = std::make_unique<Astra::Registry>(Components<Position, C>());
        s.ents.reserve(N);
        for (size_t i = 0; i < N; ++i)
        {
            Astra::Entity e = s.reg->CreateEntityWith(Position{float(i), 0.0f, 0.0f});
            if (Holds(i, pct)) s.reg->AddComponent(e, C{i, i});
            s.ents.push_back(e);
        }
        return s;
    }

    // ns_per_op = per entity for one add + one remove (items = 2N).
    template<bool Universal>
    void IdentityChurn(const char* op, size_t N)
    {
        report(op, N, median_structural(5, [N] { return BuildIdentity<Universal>(N); }, [](State& s) {
            for (Astra::Entity e : s.ents) s.reg->AddComponent(e, Velocity{1.0f, 1.0f, 1.0f});
            for (Astra::Entity e : s.ents) s.reg->RemoveComponent<Velocity>(e);
        }), N * 2);
    }

    template<class C>
    void AddRemoveChurn(const char* op, size_t N)
    {
        report(op, N, median_structural(5, [N] { return BuildPositions<C>(N, 0); }, [](State& s) {
            for (Astra::Entity e : s.ents) s.reg->AddComponent(e, C{1, 2});
            for (Astra::Entity e : s.ents) s.reg->RemoveComponent<C>(e);
        }), N * 2);
    }

    template<class C>
    void FilteredIter(const char* prefix, size_t N, int pct)
    {
        State s = BuildPositions<C>(N, pct);
        auto view = s.reg->CreateView<Position, C>();
        char op[64];
        std::snprintf(op, sizeof op, "%s_%d", prefix, pct);
        report(op, N, median_persistent(7, [&]() {
            double acc = 0.0;
            view.ForEach([&](Astra::Entity, Position& p, C& c) { acc += p.x + double(c.a); });
            g_sink += acc;
        }), N);
    }

    template<class C>
    void SoleIter(const char* prefix, size_t N, int pct)
    {
        State s = BuildPositions<C>(N, pct);
        auto view = s.reg->CreateView<C>();
        char op[64];
        std::snprintf(op, sizeof op, "%s_%d", prefix, pct);
        report(op, N, median_persistent(7, [&]() {
            double acc = 0.0;
            view.ForEach([&](Astra::Entity, C& c) { acc += double(c.a); });
            g_sink += acc;
        }), N);
    }

    template<class C, bool Universal>
    void RandomGet(const char* op, size_t N)
    {
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<C>();
        Astra::Registry reg(Components<Position, C>(), cfg);
        std::vector<Astra::Entity> ents(N);
        for (size_t i = 0; i < N; ++i) ents[i] = reg.CreateEntityWith(Position{float(i), 0.0f, 0.0f}, C{i, i});
        const std::vector<uint32_t> order = shuffled_indices(N);
        report(op, N, median_persistent(7, [&]() {
            uint64_t acc = 0;
            for (uint32_t i : order) acc += reg.GetComponent<C>(ents[i])->a;
            g_sink += double(acc);
        }), N);
    }
}

int main()
{
    IdentityChurn<false>("identity_churn_table", 100'000);
    IdentityChurn<true>("identity_churn_universal", 100'000);
    for (size_t n : {size_t(1'000), size_t(10'000), size_t(100'000)})
    {
        AddRemoveChurn<ChurnT>("addremove_table", n);
        AddRemoveChurn<ChurnS>("addremove_sparse", n);
    }
    for (int pct : {1, 50, 99})
    {
        FilteredIter<ChurnT>("filter_table", 1'000'000, pct);
        FilteredIter<ChurnS>("filter_sparse", 1'000'000, pct);
        SoleIter<ChurnT>("sole_table", 1'000'000, pct);
        SoleIter<ChurnS>("sole_sparse", 1'000'000, pct);
    }
    RandomGet<ChurnT, false>("random_get_table", 1'000'000);
    RandomGet<ChurnS, false>("random_get_sparse", 1'000'000);
    RandomGet<ChurnT, true>("random_get_universal", 1'000'000);
    std::fprintf(stderr, "# astra-storage sink=%f\n", (double)g_sink);
    return 0;
}
