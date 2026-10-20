#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <utility>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

TEST(SparseTicks, AddStampsAddedAndChanged)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    const Astra::Tick before = reg.CurrentTick();
    reg.AdvanceTick();
    reg.AddComponent<SpE>(e, SpE{1.0f});
    EXPECT_TRUE(reg.IsAdded<SpE>(e, before));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, before));
    const Astra::Tick at = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_FALSE(reg.IsAdded<SpE>(e, at));
    EXPECT_FALSE(reg.IsChanged<SpE>(e, at));
}

TEST(SparseTicks, MutableGetIsAWriteConstGetIsNot)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    const Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    (void)std::as_const(reg).GetComponent<SpE>(e);
    EXPECT_FALSE(reg.IsChanged<SpE>(e, mark));
    SpE* p = reg.GetComponent<SpE>(e);
    ASSERT_NE(p, nullptr);
    p->v = 2.0f;
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    EXPECT_FALSE(reg.IsAdded<SpE>(e, mark));
}

TEST(SparseTicks, ModifiedTypedAndById)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    reg.AddComponent<SpTag>(e, SpTag{});
    Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_TRUE(reg.Modified<SpE>(e));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_TRUE(reg.Modified(e, Astra::TypeID<SpE>::Value()));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    EXPECT_FALSE(reg.Modified<SpTag>(e));                   // tag: no data, Table parity
    EXPECT_FALSE(reg.Modified<SpA>(e));                     // absent
    EXPECT_FALSE(reg.Modified<SpE>(Astra::Entity::Invalid()));
}

TEST(SparseTicks, SetIfNeqStoresAndStampsOnlyOnDifference)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{1.0f});
    const Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_FALSE(reg.SetIfNeq<SpE>(e, SpE{1.0f}));
    EXPECT_FALSE(reg.IsChanged<SpE>(e, mark));
    EXPECT_TRUE(reg.SetIfNeq<SpE>(e, SpE{3.0f}));
    EXPECT_TRUE(reg.IsChanged<SpE>(e, mark));
    const SpE* p = std::as_const(reg).GetComponent<SpE>(e);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->v, 3.0f);
}

TEST(SparseTicks, UntrackedSparseIsComponentCoarse)
{
    Astra::Registry reg;
    const Astra::Entity a = reg.CreateEntity();
    const Astra::Entity b = reg.CreateEntity();
    reg.AddComponent<SpA>(a, SpA{});
    reg.AddComponent<SpA>(b, SpA{});
    const Astra::Tick mark = reg.CurrentTick();
    reg.AdvanceTick();
    EXPECT_FALSE(reg.IsChanged<SpA>(a, mark));
    EXPECT_TRUE(reg.Modified<SpA>(b));
    EXPECT_TRUE(reg.IsChanged<SpA>(b, mark));
    EXPECT_TRUE(reg.IsChanged<SpA>(a, mark));     // coarse: any member's write (documented)
    EXPECT_TRUE(reg.IsAdded<SpA>(a, mark));       // untracked: IsAdded == IsChanged (README caveat)
}

TEST(SparseTicks, AbsentOrInvalidAnswersFalse)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.GetComponentRegistry()->RegisterComponent<SpE>();
    EXPECT_FALSE(reg.IsChanged<SpE>(e, Astra::Tick{0}));
    EXPECT_FALSE(reg.IsAdded<SpE>(Astra::Entity::Invalid(), Astra::Tick{0}));
}
