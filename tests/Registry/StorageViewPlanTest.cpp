#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

// s6.1: only Table-TRAIT members reach the archetype masks; Sparse members never do.
TEST(StorageViewPlan, TableTraitMasksLeaveSparseMembersOut)
{
    using QB = Astra::QueryBuilder<Position, PosS, Astra::With<SpTag>, Astra::Not<SpB>, Astra::Not<Velocity>>;
    EXPECT_EQ(QB::GetTableRequiredMask(), Astra::MakeComponentMask<Position>());
    EXPECT_EQ(QB::GetTableExcludedMask(), Astra::MakeComponentMask<Velocity>());

    static_assert(Astra::View<Position, PosS>::HasSparseArg);
    static_assert(Astra::View<Position, Astra::Optional<PosS>>::HasSparseArg);
    static_assert(Astra::View<Position, Astra::Any<Velocity, SpB>>::HasSparseArg);
    static_assert(!Astra::View<Position, Velocity, Astra::Not<Health>>::HasSparseArg);
    static_assert(Astra::Detail::IsSparseArgV<const PosS> && !Astra::Detail::IsSparseArgV<Position>);
}

// The scheduler's access sets stay id-level for every kind (ViewAccess is untouched).
TEST(StorageViewPlan, ViewAccessStaysIdLevel)
{
    using VA = Astra::ViewAccess<Astra::View<PosS, const Position, Astra::Optional<const SpB>>>;
    EXPECT_TRUE(VA::WriteMask().Test(Astra::TypeID<PosS>::Value()));
    EXPECT_TRUE(VA::ReadMask().Test(Astra::TypeID<Position>::Value()));
    EXPECT_TRUE(VA::ReadMask().Test(Astra::TypeID<SpB>::Value()));
}

// A Universal component is on every live entity and in no archetype mask: a view
// requiring it matches every archetype, and Not<U> matches nothing.
TEST(StorageViewPlan, UniversalArgumentsLeaveTheMasksAndMatchEveryArchetype)
{
    auto reg = MakeRegistry<Health>();
    for (int i = 0; i < 3; ++i) (void)reg->CreateEntity<Position>();
    for (int i = 0; i < 2; ++i) (void)reg->CreateEntity<Velocity>();
    (void)reg->CreateEntity();

    EXPECT_EQ(reg->CreateView<Health>().Size(), 6u);
    EXPECT_EQ((reg->CreateView<Position, Health>().Size()), 3u);
    EXPECT_EQ((reg->CreateView<Position, Astra::With<Health>>().Size()), 3u);
    EXPECT_EQ((reg->CreateView<Position, Astra::Not<Health>>().Size()), 0u);
    EXPECT_EQ(reg->CreateView<Astra::Not<Health>>().Size(), 0u);
}

// Archetypes created AFTER the view was built go through the same runtime matcher.
TEST(StorageViewPlan, UniversalViewCacheRefreshUsesThePlan)
{
    auto reg = MakeRegistry<Health>();
    (void)reg->CreateEntity<Position>();
    auto view = reg->CreateView<Health>();
    EXPECT_EQ(view.Size(), 1u);
    (void)reg->CreateEntity<Position, Velocity>();   // new archetype
    (void)reg->CreateEntity<Transform>();            // new archetype
    EXPECT_EQ(view.Size(), 3u);
}

// G4: a registry without universals keeps today's matching for a pure-Table view.
TEST(StorageViewPlan, PureTableViewMatchingIsUnchanged)
{
    Astra::Registry reg;
    (void)reg.CreateEntity<Position>();
    (void)reg.CreateEntity<Position, Velocity>();
    (void)reg.CreateEntity<Position, Health>();
    EXPECT_EQ(reg.CreateView<Position>().Size(), 3u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<Velocity>>().Size()), 2u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Any<Velocity, Health>>().Size()), 2u);
    EXPECT_EQ((reg.CreateView<Astra::OneOf<Velocity, Health>>().Size()), 2u);
}

// The manager carries the registry's ComponentStorage for View/Relations. Pointer
// EQUALITY (not just non-null): a stale pointer to a destroyed store is a use-after-free,
// and LoadInternal rebuilds the store after constructing the manager.
TEST(StorageViewPlan, ArchetypeManagerCarriesTheComponentStorage)
{
    Astra::Registry reg;
    EXPECT_NE(reg.GetComponentStorage(), nullptr);
    EXPECT_EQ(reg.GetArchetypeManager()->GetComponentStorage(), reg.GetComponentStorage());
    reg.Clear();
    EXPECT_EQ(reg.GetArchetypeManager()->GetComponentStorage(), reg.GetComponentStorage());

    (void)reg.CreateEntity<Position>();
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position>();
    auto loaded = Astra::Registry::Load(*saved.GetValue(), componentRegistry);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg2 = **loaded.GetValue();
    EXPECT_NE(reg2.GetComponentStorage(), nullptr);
    EXPECT_EQ(reg2.GetArchetypeManager()->GetComponentStorage(), reg2.GetComponentStorage());
}
