// Storage-parity suite, query surface (spec 2026-10-01 s9.1, D2).
#include <atomic>
#include <set>
#include <span>
#include <tuple>
#include <vector>

#include "StorageParity.hpp"
#include "../Support/TestWorkerPool.hpp"

using namespace Astra::Test::Parity;
using Astra::Test::Health;
using Astra::Test::Velocity;

namespace
{
    // n entities, all with Velocity{dx = i}; Health on even i; C "given" on i % 3 == 0
    // with MakePos<C>(i). Universal: every entity holds C; non-givens hold C{}.
    template<class V>
    struct Population
    {
        using C = typename V::Comp;
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> es;
        size_t n = 0;

        bool Given(size_t i) const { return i % 3 == 0; }
        bool HasC(size_t i) const { return V::kUniversal || Given(i); }
        static bool HasHealth(size_t i) { return i % 2 == 0; }
        C ValueOf(size_t i) const { return Given(i) ? MakePos<C>(float(i)) : C{}; }
        size_t Holders() const { size_t h = 0; for (size_t i = 0; i < n; ++i) h += HasC(i); return h; }
        std::set<Astra::Entity> Where(auto pred) const
        {
            std::set<Astra::Entity> s;
            for (size_t i = 0; i < n; ++i) if (pred(i)) s.insert(es[i]);
            return s;
        }
    };

    template<class V>
    Population<V> Populate(size_t n, Astra::Registry::Config cfg = {})
    {
        Population<V> p;
        p.reg = V::Make(std::move(cfg));
        p.n = n;
        p.es.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            p.es[i] = p.reg->CreateEntityWith(Velocity{float(i), 0.0f, 0.0f});
            if (Population<V>::HasHealth(i)) EXPECT_TRUE(p.reg->AddComponent(p.es[i], Health{}));
            if (p.Given(i)) Give<V>(*p.reg, p.es[i], MakePos<typename V::Comp>(float(i)));
        }
        return p;
    }

    void AdvanceTo(Astra::Registry& reg, Astra::Tick t)
    {
        while (reg.CurrentTick() < t) reg.AdvanceTick();
    }
}

template<class V> class StorageParityQuery : public ::testing::Test {};
using PosVariants = ::testing::Types<TableV<Astra::Test::Position>,
                                     SparseV<ParityPosS>,
                                     UniversalV<Astra::Test::Position>>;
TYPED_TEST_SUITE(StorageParityQuery, PosVariants, VariantNames);

// Archetype-driven (Velocity is Table): C becomes a chunk filter + per-entity fetch.
TYPED_TEST(StorageParityQuery, RequiredArgumentVisitsExactlyTheHolders)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(600);
    std::set<Astra::Entity> seen;
    p.reg->template CreateView<Velocity, C>().ForEach([&](Astra::Entity e, Velocity& v, C& c) {
        seen.insert(e);
        const size_t i = size_t(v.dx);
        EXPECT_EQ(e, p.es[i]);
        EXPECT_EQ(c, p.ValueOf(i));
    });
    const auto expected = p.Where([&](size_t i) { return p.HasC(i); });
    EXPECT_EQ(seen, expected);
    auto view = p.reg->template CreateView<Velocity, C>();
    EXPECT_EQ(view.Size(), expected.size());
    for (size_t i = 0; i < p.n; ++i) EXPECT_EQ(view.Contains(p.es[i]), p.HasC(i)) << i;
}

// No Table argument: sparse-driven for Sparse, every archetype for Universal (s6.2).
TYPED_TEST(StorageParityQuery, ViewOfOnlyTheComponentVisitsExactlyTheHolders)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(600);
    std::set<Astra::Entity> seen;
    p.reg->template CreateView<const C>().ForEach([&](Astra::Entity e, const C& c) {
        seen.insert(e);
        EXPECT_EQ(c, *p.reg->template GetComponent<C>(e));
    });
    EXPECT_EQ(seen, p.Where([&](size_t i) { return p.HasC(i); }));
    EXPECT_EQ(p.reg->template CreateView<C>().Size(), p.Holders());
}

TYPED_TEST(StorageParityQuery, NotWithOptionalAnyOneOf)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(300);
    auto count = [&](auto view) { size_t k = 0; view.ForEach([&](Astra::Entity, Velocity&) { ++k; }); return k; };
    auto expectCount = [&](auto pred) { return p.Where(pred).size(); };

    EXPECT_EQ(count(p.reg->template CreateView<Velocity, Astra::Not<C>>()), expectCount([&](size_t i) { return !p.HasC(i); }));
    EXPECT_EQ(count(p.reg->template CreateView<Velocity, Astra::With<C>>()), expectCount([&](size_t i) { return p.HasC(i); }));
    EXPECT_EQ(count(p.reg->template CreateView<Velocity, Astra::Any<C, Health>>()),
              expectCount([&](size_t i) { return p.HasC(i) || p.HasHealth(i); }));
    EXPECT_EQ(count(p.reg->template CreateView<Velocity, Astra::OneOf<C, Health>>()),
              expectCount([&](size_t i) { return p.HasC(i) != p.HasHealth(i); }));

    size_t visited = 0;
    p.reg->template CreateView<Velocity, Astra::Optional<C>>().ForEach([&](Astra::Entity, Velocity& v, C* c) {
        ++visited;
        const size_t i = size_t(v.dx);
        EXPECT_EQ(c != nullptr, p.HasC(i)) << i;
        if (c) EXPECT_EQ(*c, p.ValueOf(i)) << i;
    });
    EXPECT_EQ(visited, p.n);
}

TYPED_TEST(StorageParityQuery, GetSingleContainsAgreeWithForEach)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto p = Populate<V>(30);
    auto v = p.reg->template CreateView<Velocity, C>();
    for (size_t i = 0; i < p.n; ++i)
    {
        auto r = v.Get(p.es[i]);
        ASSERT_EQ(r.IsOk(), p.HasC(i)) << i;
        if (r.IsOk())
        {
            auto [vel, c] = *r.GetValue();
            EXPECT_EQ(*c, p.ValueOf(i));
            EXPECT_EQ(c, p.reg->template GetComponent<C>(p.es[i]));   // pointer identity with GetComponent
            (void)vel;
        }
        else
        {
            EXPECT_EQ(*r.GetError(), Astra::QueryError::NotMatched);
        }
    }

    // Single: exactly one Velocity+C holder (the others carry Health only).
    auto reg = V::Make();
    Astra::Entity only = reg->CreateEntityWith(Velocity{});
    Give<V>(*reg, only, MakePos<C>(7.0f));
    for (int i = 0; i < 4; ++i) (void)reg->CreateEntityWith(Health{});
    auto sv = reg->template CreateView<Velocity, C>();
    {
        auto r = sv.Single();
        ASSERT_TRUE(r.IsOk());
        auto [vel, c] = *r.GetValue();
        (void)vel;
        EXPECT_EQ(*c, MakePos<C>(7.0f));
    }
    Astra::Entity second = reg->CreateEntityWith(Velocity{});
    Give<V>(*reg, second, MakePos<C>(8.0f));
    auto r2 = sv.Single();
    ASSERT_TRUE(r2.IsErr());
    EXPECT_EQ(*r2.GetError(), Astra::QueryError::MultipleMatched);
    reg->DestroyEntity(second);
    EXPECT_FALSE(sv.Contains(second));                       // stale handle
    EXPECT_TRUE(sv.Get(second).IsErr());
}

// s6.5: a required Sparse argument must NOT trigger the range-for refusal (a Table one never does).
TYPED_TEST(StorageParityQuery, RangeForVisitsTheSameSetAsForEach)
{
    using C = typename TypeParam::Comp;
    auto p = Populate<TypeParam>(400);
    auto view = p.reg->template CreateView<Velocity, C>();
    std::set<Astra::Entity> seen;
    for (auto [e, vel, c] : view)
    {
        seen.insert(e);
        EXPECT_EQ(c, p.ValueOf(size_t(vel.dx)));
    }
    EXPECT_EQ(seen, p.Where([&](size_t i) { return p.HasC(i); }));
}

TYPED_TEST(StorageParityQuery, ParallelForEachOnARealSchedulerVisitsEveryHolderOnce)
{
    using C = typename TypeParam::Comp;
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    auto p = Populate<TypeParam>(30'000, cfg);

    std::atomic<size_t> visits{0};
    p.reg->template CreateView<Velocity, C>().ParallelForEach([&](Astra::Entity, Velocity&, C& c) {
        c.z += 1.0f;                                         // each entity owns its slot in every store
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visits.load(), p.Holders());
    for (size_t i = 0; i < p.n; ++i)
        if (p.HasC(i)) ASSERT_FLOAT_EQ(p.reg->template GetComponent<C>(p.es[i])->z, p.ValueOf(i).z + 1.0f) << i;

    std::atomic<size_t> onlyC{0};                            // sparse-driven partitioning (s6.5)
    p.reg->template CreateView<const C>().ParallelForEach([&](Astra::Entity, const C&) { onlyC.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(onlyC.load(), p.Holders());
}

// Parity of the Debug structural-mutation guard (View.hpp:829-856, extended by Task 25).
// Integration ruling (D vs F): parity means "same outcome as Table today", NOT new guard
// semantics. Today's guard trips on an archetype-SET change (a never-seen archetype is
// created); Task 25 adds the sparse membership counter. So, for every kind:
//   - an archetype-set change inside ForEach trips it identically;
//   - a Sparse add/remove trips it through the sparse counter;
//   - a deferred (CommandBuffer) change never trips it.
// Destroying a bystander, and a move into an EXISTING archetype, do not trip it for Table
// today, so they are deliberately asserted for no kind.
TYPED_TEST(StorageParityQuery, StructuralChangeDuringForEachTripsTheGuard)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the structural-mutation guard is a Debug-only ASTRA_ASSERT";
#else
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity iterated = reg->CreateEntityWith(Velocity{});      // the view's ONLY match
    Give<V>(*reg, iterated, MakePos<C>(1.0f));
    Astra::Entity bystander = reg->CreateEntityWith(Health{});       // outside the iterated archetype
    Astra::Entity holder = reg->CreateEntityWith(Health{});
    Give<V>(*reg, holder, MakePos<C>(2.0f));

    auto runAndCount = [&](auto&& mutate) {
        AssertCount cap;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            reg->template CreateView<Velocity, C>().ForEach([&](Astra::Entity, Velocity&, C&) { mutate(); });
        }
        return cap.count;
    };

    // {Health, Velocity} has never existed in this registry, for any kind: an archetype-set change.
    EXPECT_GE(runAndCount([&] { (void)reg->AddComponent(bystander, Velocity{}); }), 1);   // every kind
    if constexpr (V::kSparse)
    {
        // Sparse membership changes never touch the archetype set; the sparse counter catches them.
        Astra::Entity lacking = reg->CreateEntityWith(Health{});
        EXPECT_GE(runAndCount([&] { (void)reg->AddComponent(lacking, MakePos<C>(3.0f)); }), 1);
        EXPECT_GE(runAndCount([&] { (void)reg->template RemoveComponent<C>(holder); }), 1);
    }
    Astra::Entity later = reg->CreateEntityWith(Health{});
    Astra::CommandBuffer cmd(reg.get());
    EXPECT_EQ(runAndCount([&] { cmd.DestroyEntity(later); }), 0);                    // deferred: never trips
    ASSERT_TRUE(cmd.Execute().IsOk());
    EXPECT_FALSE(reg->IsValid(later));
#endif
}

// Save -> Load into a registry with the SAME storage assignment (raw and LZ4). Cross-storage
// migration (Table<->Sparse/Universal of one type) is pinned by Tasks 33-34.
TYPED_TEST(StorageParityQuery, SaveLoadRoundTripKeepsValuesAndMembership)
{
    using V = TypeParam;
    using C = typename V::Comp;
    for (Astra::CompressionMode mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        SCOPED_TRACE(mode == Astra::CompressionMode::LZ4 ? "LZ4" : "raw");
        auto p = Populate<V>(3000);
        Astra::Registry::SaveConfig sc;
        sc.compressionMode = mode;
        auto saved = p.reg->Save(sc);
        ASSERT_TRUE(saved.IsOk());
        auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()),
                                            V::MakeComponentRegistry(), V::MakeConfig());
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& r = **loaded.GetValue();
        ASSERT_EQ(r.Size(), p.n);
        for (size_t i = 0; i < p.n; ++i)
        {
            ASSERT_EQ(r.template HasComponent<C>(p.es[i]), p.HasC(i)) << i;
            if (p.HasC(i)) ASSERT_EQ(*r.template GetComponent<C>(p.es[i]), p.ValueOf(i)) << i;
            ASSERT_EQ(r.template HasComponent<Health>(p.es[i]), p.HasHealth(i)) << i;
        }
        EXPECT_EQ((r.template CreateView<Velocity, C>().Size()), p.Holders());
        EXPECT_EQ((r.template CreateView<Velocity, Astra::Not<C>>().Size()), p.n - p.Holders());
    }
}

TYPED_TEST(StorageParityQuery, RelationsFiltersSeeTheComponent)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity parent = reg->template CreateEntity<Velocity>();
    for (int i = 0; i < 6; ++i)
    {
        Astra::Entity kid = reg->template CreateEntity<Velocity>();
        if (i % 2 == 0) Give<V>(*reg, kid, MakePos<C>(float(i)));
        reg->SetParent(kid, parent);
    }
    const size_t holders = V::kUniversal ? 6u : 3u;
    EXPECT_EQ(reg->template GetRelations<C>(parent).GetChildren().size(), holders);
    EXPECT_EQ(reg->template GetRelations<Astra::Not<C>>(parent).GetChildren().size(), 6u - holders);
    size_t visited = 0;
    float sumX = 0.0f;
    reg->template GetRelations<C>(parent).ForEachChild([&](Astra::Entity, C& c) { ++visited; sumX += c.x; });
    EXPECT_EQ(visited, holders);
    EXPECT_FLOAT_EQ(sumX, 0.0f + 2.0f + 4.0f);   // Universal non-givens hold C{} (x == 0)
}

// ======================= Change-tracked variants (ChangeDetectionTest.cpp:750-1010) =======================
// Untracked components are chunk-coarse for tables BY DESIGN (a superset), so exact
// parity is defined for change-TRACKED components only: per-entity ticks in every store.

template<class V> class StorageParityTracked : public ::testing::Test {};
using TrackedVariants = ::testing::Types<TableV<Astra::Test::TrackedPos>,
                                         SparseV<ParityTrackedS>,
                                         UniversalV<Astra::Test::TrackedPos>>;
TYPED_TEST_SUITE(StorageParityTracked, TrackedVariants, VariantNames);

TYPED_TEST(StorageParityTracked, ChangedAndAddedArePerEntity)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    AdvanceTo(*reg, 2);
    std::vector<Astra::Entity> ents(500);
    ASSERT_EQ((reg->template CreateEntitiesWith<C, Velocity>(500, std::span{ents}, [](size_t) {
        return std::tuple{C{}, Velocity{}};
    })), 500u);
    AdvanceTo(*reg, 3);
    const std::vector<size_t> marked = {0, 3, 64, 65, 127, 300, 499};
    std::set<Astra::Entity> expected;
    for (size_t i : marked) { reg->template GetComponent<C>(ents[i])->x = 1.0f; expected.insert(ents[i]); }
    Astra::Entity fresh = reg->CreateEntityWith(C{}, Velocity{});          // added == 3

    auto changed = reg->template CreateView<const C, Astra::Changed<C>>();          // sparse-driven for Sparse
    std::set<Astra::Entity> seen;
    changed.Since(2).ForEach([&](Astra::Entity e, const C&) { seen.insert(e); });
    std::set<Astra::Entity> expectedChanged = expected;
    expectedChanged.insert(fresh);
    EXPECT_EQ(seen, expectedChanged);
    size_t n3 = 0, n0 = 0;
    changed.Since(3).ForEach([&](Astra::Entity, const C&) { ++n3; });
    changed.Since(0).ForEach([&](Astra::Entity, const C&) { ++n0; });
    EXPECT_EQ(n3, 0u);
    EXPECT_EQ(n0, 501u);

    auto hot = reg->template CreateView<const Velocity, const C, Astra::Changed<C>>();   // archetype-driven
    seen.clear();
    hot.Since(2).ForEach([&](Astra::Entity e, const Velocity&, const C&) { seen.insert(e); });
    EXPECT_EQ(seen, expectedChanged);

    auto added = reg->template CreateView<const C, Astra::Added<C>>();
    seen.clear();
    added.Since(2).ForEach([&](Astra::Entity e, const C&) { seen.insert(e); });
    EXPECT_EQ(seen, std::set<Astra::Entity>{fresh});
}

TYPED_TEST(StorageParityTracked, MutModifiedAndSetIfNeqMarkExactly)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    AdvanceTo(*reg, 2);
    Astra::Entity e = reg->CreateEntityWith(C{1.0f, 2.0f, 3.0f}, Velocity{});
    AdvanceTo(*reg, 3);
    reg->template CreateView<C>().ForEach([](Astra::Mut<C> c) { (void)c.Read(); });   // Read never marks
    EXPECT_FALSE(reg->template IsChanged<C>(e, 2));
    AdvanceTo(*reg, 4);
    reg->template CreateView<C>().ForEach([](Astra::Mut<C> c) { c->y = 9.0f; });      // operator-> marks
    EXPECT_TRUE(reg->template IsChanged<C>(e, 3));
    AdvanceTo(*reg, 5);
    EXPECT_FALSE(reg->SetIfNeq(e, C{1.0f, 9.0f, 3.0f}));                      // equal: no mark
    EXPECT_FALSE(reg->template IsChanged<C>(e, 4));
    EXPECT_TRUE(reg->SetIfNeq(e, C{7.0f, 9.0f, 3.0f}));
    EXPECT_TRUE(reg->template IsChanged<C>(e, 4));
    AdvanceTo(*reg, 6);
    EXPECT_TRUE(reg->template Modified<C>(e));
    EXPECT_TRUE(reg->template IsChanged<C>(e, 5));
    EXPECT_TRUE(reg->template IsAdded<C>(e, 1));
    EXPECT_FALSE(reg->template IsAdded<C>(e, 2));
}

// ======================= Enableable filtering through queries + round trip =======================

template<class V> class StorageParityEnableableQuery : public ::testing::Test {};
using EnVariants = ::testing::Types<TableV<Astra::Test::Hierarchy>,
                                    SparseV<ParityHierS>,
                                    UniversalV<Astra::Test::Hierarchy>>;
TYPED_TEST_SUITE(StorageParityEnableableQuery, EnVariants, VariantNames);

TYPED_TEST(StorageParityEnableableQuery, DefaultViewsSkipDisabledAndBitsSurviveSaveLoad)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 1000;   // 16 B x 1000 clears the 1024-byte LZ4 threshold
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg->template CreateEntitiesWith<C, Velocity>(N, std::span{es}, [](size_t i) {
        return std::tuple{MakeHier<C>(uint32_t(i)), Velocity{float(i), 0.0f, 0.0f}};
    })), N);
    auto disabled = [](size_t i) { return i % 3 == 0 || i == 63 || i == 64 || i == 65 || i == 127 || i == 128; };
    for (size_t i = 0; i < N; ++i) if (disabled(i)) ASSERT_TRUE(reg->template SetEnabled<C>(es[i], false));
    size_t enabled = 0;
    for (size_t i = 0; i < N; ++i) enabled += disabled(i) ? 0 : 1;

    auto check = [&](Astra::Registry& r) {
        std::set<Astra::Entity> seen;
        r.CreateView<Velocity, C>().ForEach([&](Astra::Entity e, Velocity& v, C& c) {
            seen.insert(e);
            EXPECT_FALSE(disabled(size_t(v.dx)));
            EXPECT_EQ(c.parent, uint32_t(v.dx));
        });
        EXPECT_EQ(seen.size(), enabled);
        EXPECT_EQ(r.CreateView<C>().Size(), enabled);
        EXPECT_EQ(r.CreateView<Astra::IncludeDisabled<C>>().Size(), N);
        EXPECT_EQ((r.CreateView<Velocity, Astra::IncludeDisabled<C>>().Size()), N);
        for (size_t i = 0; i < N; ++i) ASSERT_EQ(r.IsEnabled<C>(es[i]), !disabled(i)) << i;
        auto v = r.CreateView<Velocity, C>();
        EXPECT_FALSE(v.Contains(es[0]));                     // disabled -> invisible to random access
        EXPECT_TRUE(v.Contains(es[1]));
        EXPECT_TRUE(v.Get(es[1]).IsOk());
        EXPECT_TRUE(v.Get(es[0]).IsErr());
    };
    check(*reg);

    Astra::Registry::SaveConfig sc;
    sc.compressionMode = Astra::CompressionMode::LZ4;
    auto saved = reg->Save(sc);
    ASSERT_TRUE(saved.IsOk());
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()),
                                        V::MakeComponentRegistry(), V::MakeConfig());
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    check(**loaded.GetValue());
}
