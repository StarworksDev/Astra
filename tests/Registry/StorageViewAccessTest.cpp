#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    using S = std::set<Astra::Entity>;

    // range-for and ForEach visit the same entity set (s6.5, D2).
    template<typename V>
    void ExpectRangeForMatchesForEach(V view)
    {
        S viaForEach, viaRange;
        view.ForEach([&](Astra::Entity e, auto&&...) { viaForEach.insert(e); });
        for (auto t : view) viaRange.insert(std::get<0>(t));
        EXPECT_EQ(viaRange, viaForEach);
        EXPECT_EQ(viaRange.size(), view.Size());
    }
}

TEST(StorageViewAccess, ContainsGetAndSingleAreStorageAware)
{
    Astra::Registry reg;
    Astra::Entity a = reg.CreateEntity<Position>();
    reg.AddComponent(a, PosS{1.0f, 2.0f, 3.0f});
    Astra::Entity b = reg.CreateEntity<Position>();

    auto view = reg.CreateView<Position, PosS>();
    EXPECT_TRUE(view.Contains(a));
    EXPECT_FALSE(view.Contains(b));
    auto ra = view.Get(a);
    ASSERT_TRUE(ra.IsOk());
    auto [pa, sa] = *ra.GetValue();
    ASSERT_NE(sa, nullptr);
    EXPECT_FLOAT_EQ(sa->y, 2.0f);
    EXPECT_EQ(static_cast<const PosS*>(sa), std::as_const(reg).GetComponent<PosS>(a));
    EXPECT_TRUE(view.Get(b).IsErr());

    ASSERT_TRUE(view.Single().IsOk());
    reg.AddComponent(b, PosS{});
    EXPECT_EQ(*view.Single().GetError(), Astra::QueryError::MultipleMatched);

    // Optional: a present Sparse tag yields its static instance on Get(); an absent one null.
    reg.AddComponent(a, SpTag{});
    auto ov = reg.CreateView<Position, Astra::Optional<SpTag>, Astra::Optional<SpB>>();
    auto oa = ov.Get(a);
    ASSERT_TRUE(oa.IsOk());
    EXPECT_NE(std::get<1>(*oa.GetValue()), nullptr);
    EXPECT_EQ(std::get<2>(*oa.GetValue()), nullptr);
    EXPECT_EQ(std::get<1>(*ov.Get(b).GetValue()), nullptr);

    // Enabled-only filtering per entity.
    reg.AddComponent(a, SpE{});
    ASSERT_TRUE(reg.SetEnabled<SpE>(a, false));
    EXPECT_FALSE((reg.CreateView<Position, const SpE>().Contains(a)));
    EXPECT_TRUE((reg.CreateView<Position, Astra::IncludeDisabled<SpE>>().Contains(a)));
}

TEST(StorageViewAccess, GetOnUniversalAndTrackedMut)
{
    auto reg = MakeRegistry<Health>();
    Astra::Entity e = reg->CreateEntity<Position>();
    auto r = reg->CreateView<Position, Health>().Get(e);
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(static_cast<const Health*>(std::get<1>(*r.GetValue())), std::as_const(*reg).GetComponent<Health>(e));

    Astra::Registry treg;
    Astra::Entity t = treg.CreateEntity<Position>();
    treg.AddComponent(t, SpE{});
    const Astra::Tick since = treg.CurrentTick();
    treg.AdvanceTick();
    auto rt = treg.CreateView<SpE>().Get(t);
    ASSERT_TRUE(rt.IsOk());
    std::get<0>(*rt.GetValue()).Write().v = 3.0f;          // Mut<SpE>: marks on use
    EXPECT_TRUE(treg.IsChanged<SpE>(t, since));
    EXPECT_FLOAT_EQ(std::as_const(treg).GetComponent<SpE>(t)->v, 3.0f);
}

// RULINGS Task 29 (from Task 22 review): an ENABLEABLE Universal REQUIRED argument.
TEST(StorageViewAccess, EnableableUniversalRequiredGetContainsSingle)
{
    auto reg = MakeRegistry<Timer>();
    Astra::Entity on = reg->CreateEntity<Position>();
    Astra::Entity off = reg->CreateEntity<Position>();
    ASSERT_TRUE(reg->SetEnabled<Timer>(off, false));

    auto view = reg->CreateView<Position, Timer>();
    EXPECT_TRUE(view.Contains(on));
    EXPECT_FALSE(view.Contains(off));
    EXPECT_TRUE(view.Get(on).IsOk());
    EXPECT_TRUE(view.Get(off).IsErr());
    ASSERT_TRUE(view.Single().IsOk());
    EXPECT_EQ(static_cast<const Timer*>(std::get<1>(*view.Single().GetValue())), std::as_const(*reg).GetComponent<Timer>(on));

    ASSERT_TRUE(reg->SetEnabled<Timer>(off, true));
    EXPECT_TRUE(view.Contains(off));
    EXPECT_EQ(*view.Single().GetError(), Astra::QueryError::MultipleMatched);
}

// RULINGS Task 29 (from Task 26 review): Any / OneOf groups with Sparse members.
TEST(StorageViewAccess, ContainsAndGetHonourAnyAndOneOfWithSparseMembers)
{
    Astra::Registry reg;
    Astra::Entity none = reg.CreateEntity<Position>();
    Astra::Entity onlyS = reg.CreateEntity<Position>();
    reg.AddComponent(onlyS, PosS{});
    Astra::Entity onlyT = reg.CreateEntity<Position>();
    reg.AddComponent(onlyT, SpTag{});
    Astra::Entity both = reg.CreateEntity<Position>();
    reg.AddComponent(both, PosS{});
    reg.AddComponent(both, SpTag{});

    auto any = reg.CreateView<Position, Astra::Any<PosS, SpTag>>();
    EXPECT_FALSE(any.Contains(none));
    EXPECT_TRUE(any.Contains(onlyS));
    EXPECT_TRUE(any.Contains(onlyT));
    EXPECT_TRUE(any.Contains(both));
    EXPECT_TRUE(any.Get(onlyT).IsOk());
    EXPECT_TRUE(any.Get(none).IsErr());

    auto one = reg.CreateView<Position, Astra::OneOf<PosS, SpTag>>();
    EXPECT_FALSE(one.Contains(none));
    EXPECT_TRUE(one.Contains(onlyS));
    EXPECT_TRUE(one.Contains(onlyT));
    EXPECT_FALSE(one.Contains(both));
    EXPECT_TRUE(one.Get(onlyS).IsOk());
    EXPECT_TRUE(one.Get(both).IsErr());

    // Every entity holds the Table member Position, so OneOf<Position, SpTag> admits exactly
    // the entities WITHOUT the Sparse one.
    auto mixed = reg.CreateView<Position, Astra::OneOf<Position, SpTag>>();
    EXPECT_FALSE(mixed.Contains(onlyT));
    EXPECT_FALSE(mixed.Contains(both));
    EXPECT_TRUE(mixed.Contains(none));
    EXPECT_TRUE(mixed.Contains(onlyS));
}

TEST(StorageViewAccess, RangeForMatchesForEachOnStorageViews)
{
    Astra::Registry reg;
    for (int i = 0; i < 2000; ++i)
    {
        Astra::Entity e = (i % 3 == 0) ? reg.CreateEntity<Position, Velocity>() : reg.CreateEntity<Position>();
        if (i % 4 == 0) reg.AddComponent(e, PosS{float(i), 0.0f, 0.0f});
        if (i % 5 == 0) reg.AddComponent(e, SpTag{});
    }
    ExpectRangeForMatchesForEach(reg.CreateView<Position, PosS>());                     // per-entity stepping
    ExpectRangeForMatchesForEach(reg.CreateView<Position, Astra::With<SpTag>>());       // run stepping
    ExpectRangeForMatchesForEach(reg.CreateView<Position, Astra::Not<PosS>>());
    ExpectRangeForMatchesForEach(reg.CreateView<PosS>());
    ExpectRangeForMatchesForEach(reg.CreateView<Position, Astra::Any<PosS, SpTag>>());
    ExpectRangeForMatchesForEach(reg.CreateView<Position, SpTag>());

    auto ureg = MakeRegistry<Health>();
    for (int i = 0; i < 1500; ++i) (void)ureg->CreateEntity<Position>();
    ExpectRangeForMatchesForEach(ureg->CreateView<Position, Health>());
}

TEST(StorageViewAccess, RangeForWritesPersistAndStamp)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 300; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        if (i % 2 == 0) reg.AddComponent(es.back(), PosS{});
    }
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    for (auto [e, pos, s] : reg.CreateView<Position, PosS>())
        s.y = 5.0f;
    for (int i = 0; i < 300; i += 2)
    {
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(es[i])->y, 5.0f);
        EXPECT_TRUE(reg.IsChanged<PosS>(es[i], since));   // untracked: coarse stamp through the iterator
    }
}

TEST(StorageViewAccess, RangeForSurvivesDestroyingBystandersOfTheCurrentChunk)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> bystanders;
    size_t withS = 0;
    for (int i = 0; i < 300; ++i)
    {
        Astra::Entity e = reg.CreateEntity<Position>();
        if (i % 2 == 0) { reg.AddComponent(e, PosS{}); ++withS; }
        else bystanders.push_back(e);   // same chunk, no Sparse component: trips no guard
    }
    size_t visited = 0;
    bool destroyed = false;
    for (auto [e, pos, s] : reg.CreateView<Position, PosS>())
    {
        ++visited;
        if (!destroyed)
        {
            destroyed = true;
            for (auto b : bystanders) reg.DestroyEntity(b);   // swap-removes and shrinks the chunk mid-walk
        }
    }
    EXPECT_GE(visited, 1u);
    EXPECT_LE(visited, withS);
    EXPECT_TRUE(reg.ValidateStorageInvariants().empty());
}

TEST(StorageViewAccess, RangeForSurvivesDestroyingBystandersOnUniversalYield)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> all;
    for (int i = 0; i < 300; ++i) all.push_back(reg->CreateEntity<Position>());
    size_t visited = 0;
    bool destroyed = false;
    for (auto [e, pos, h] : reg->CreateView<Position, Health>())
    {
        ++visited;
        if (!destroyed)
        {
            destroyed = true;
            for (size_t i = all.size() - 1; i > 100; --i) reg->DestroyEntity(all[i]);
        }
    }
    EXPECT_GE(visited, 1u);
    EXPECT_TRUE(reg->ValidateStorageInvariants().empty());
}
