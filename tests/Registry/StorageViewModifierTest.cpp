#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <random>
#include <set>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    template<typename V>
    std::set<Astra::Entity> Visited(V&& view)
    {
        std::set<Astra::Entity> s;
        view.ForEach([&](Astra::Entity e, auto&&...) { s.insert(e); });
        return s;
    }

    struct Fixture
    {
        Astra::Registry reg;
        Astra::Entity A, B, C, D, E, F;
        Fixture()
        {
            A = reg.CreateEntity<Position>();                 // nothing
            B = reg.CreateEntity<Position>();                 reg.AddComponent(B, PosS{});
            C = reg.CreateEntity<Position>();                 reg.AddComponent(C, SpB{});
            D = reg.CreateEntity<Position, Velocity>();       // Table member only
            E = reg.CreateEntity<Position>();                 reg.AddComponent(E, PosS{}); reg.AddComponent(E, SpB{});
            F = reg.CreateEntity<Position, Velocity>();       reg.AddComponent(F, PosS{});
        }
    };
}

TEST(StorageViewModifier, AnyWithSparseMembers)
{
    Fixture fx;
    using S = std::set<Astra::Entity>;
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::Any<PosS, SpB>>()), (S{fx.B, fx.C, fx.E, fx.F}));
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::Any<Velocity, PosS>>()), (S{fx.B, fx.D, fx.E, fx.F}));
    EXPECT_EQ((fx.reg.CreateView<Position, Astra::Any<PosS, SpB>>().Size()), 4u);
    EXPECT_EQ((fx.reg.CreateView<Astra::Any<SpB, SpTag>>().Size()), 2u);
}

TEST(StorageViewModifier, OneOfWithSparseMembers)
{
    Fixture fx;
    using S = std::set<Astra::Entity>;
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::OneOf<PosS, SpB>>()), (S{fx.B, fx.C, fx.F}));
    EXPECT_EQ(Visited(fx.reg.CreateView<Position, Astra::OneOf<Velocity, PosS>>()), (S{fx.B, fx.D, fx.E}));
    EXPECT_EQ((fx.reg.CreateView<Position, Astra::OneOf<Velocity, PosS>>().Size()), 3u);
}

// Universal members are held by every entity: Any is always satisfied, OneOf requires
// every other member to be absent.
TEST(StorageViewModifier, UniversalGroupMembers)
{
    auto reg = MakeRegistry<Health>();
    Astra::Entity plain = reg->CreateEntity<Position>();
    Astra::Entity withS = reg->CreateEntity<Position>();
    reg->AddComponent(withS, PosS{});
    Astra::Entity withV = reg->CreateEntity<Position, Velocity>();
    using S = std::set<Astra::Entity>;
    EXPECT_EQ(Visited(reg->CreateView<Position, Astra::Any<Health, PosS>>()), (S{plain, withS, withV}));
    EXPECT_EQ(Visited(reg->CreateView<Position, Astra::OneOf<Health, PosS>>()), (S{plain, withV}));
    EXPECT_EQ(Visited(reg->CreateView<Position, Astra::OneOf<Health, Velocity>>()), (S{plain, withS}));
}

// Word-level correctness over many chunks against a brute-force HasComponent model.
TEST(StorageViewModifier, GroupsAgreeWithHasComponentAcrossChunks)
{
    Astra::Registry reg;
    std::mt19937 rng(0x5EED);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 3000; ++i)
    {
        Astra::Entity e = (rng() % 3 == 0) ? reg.CreateEntity<Position, Velocity>() : reg.CreateEntity<Position>();
        if (rng() % 2) reg.AddComponent(e, PosS{});
        if (rng() % 3 == 0) reg.AddComponent(e, SpB{});
        es.push_back(e);
    }
    std::set<Astra::Entity> any, one, anyV;
    for (Astra::Entity e : es)
    {
        const int s = int(reg.HasComponent<PosS>(e)) + int(reg.HasComponent<SpB>(e));
        if (s > 0) any.insert(e);
        if (s == 1) one.insert(e);
        if (reg.HasComponent<Velocity>(e) || reg.HasComponent<SpB>(e)) anyV.insert(e);
    }
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::Any<PosS, SpB>>()), any);
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::OneOf<PosS, SpB>>()), one);
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::Any<Velocity, SpB>>()), anyV);
    EXPECT_EQ((reg.CreateView<Position, Astra::OneOf<PosS, SpB>>().Size()), one.size());
}

// Several OneOf groups sharing Sparse members, each with a Table member held by the
// archetype: the known==1 path appends one exclude per Sparse member per group (the
// MaxWordSets bound; the capacity ASTRA_ASSERT must not fire).
TEST(StorageViewModifier, ManyOneOfGroupsSharingSparseMembersStayWithinWordSetCapacity)
{
    Astra::Registry reg;
    std::set<Astra::Entity> expect;
    for (int i = 0; i < 200; ++i)
    {
        Astra::Entity e = reg.CreateEntity<Position, Velocity>();
        const bool s = (i % 2) == 0, b = (i % 3) == 0;
        if (s) reg.AddComponent(e, PosS{});
        if (b) reg.AddComponent(e, SpB{});
        if (!s && !b) expect.insert(e);   // Position held (known==1) => both Sparse absent
    }
    auto view = reg.CreateView<Position, Astra::OneOf<Position, PosS, SpB>, Astra::OneOf<Velocity, PosS, SpB>,
                               Astra::OneOf<Position, SpB, PosS>>();
    EXPECT_EQ(Visited(view), expect);
}
