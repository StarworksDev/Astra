#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <utility>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

// Mut<T> over a tracked Sparse element marks ONLY the entity written through it.
TEST(StorageViewMut, SparseMutMarksOnlyTheWrittenEntity)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), SpE{float(i)});
    }
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<Position, SpE>().ForEach([&](Astra::Entity e, Position&, Astra::Mut<SpE> s)
    {
        if (e == es[3]) s.Write().v = 99.0f;
    });
    EXPECT_TRUE(reg.IsChanged<SpE>(es[3], since));
    EXPECT_FALSE(reg.IsChanged<SpE>(es[4], since));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<SpE>(es[3])->v, 99.0f);
}

TEST(StorageViewMut, UniversalMutMarksOnlyTheWrittenEntity)
{
    auto reg = MakeRegistry<TrackedPos>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i) es.push_back(reg->CreateEntity<Position>());
    const Astra::Tick since = reg->CurrentTick();
    reg->AdvanceTick();
    reg->CreateView<Position, TrackedPos>().ForEach([&](Astra::Entity e, Position&, Astra::Mut<TrackedPos> t)
    {
        if (e == es[5]) t->x = 4.0f;
    });
    EXPECT_TRUE(reg->IsChanged<TrackedPos>(es[5], since));
    EXPECT_FALSE(reg->IsChanged<TrackedPos>(es[6], since));
}

// Plan deviation 5 holds for every kind: a PRESENT non-const tracked Optional marks.
TEST(StorageViewMut, PresentTrackedSparseOptionalMarks)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 6; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        if (i % 2 == 0) reg.AddComponent(es.back(), SpE{});
    }
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<Position, Astra::Optional<SpE>>().ForEach([](Astra::Entity, Position&, SpE*) {});
    for (int i = 0; i < 6; i += 2) EXPECT_TRUE(reg.IsChanged<SpE>(es[i], since)) << i;
}

// Untracked: a non-const yield stamps the coarse source IsChanged reads; a const one does not.
TEST(StorageViewMut, UntrackedYieldsStampOnlyWhenNonConst)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.AddComponent(e, PosS{});
    Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<const Position, const PosS>().ForEach([](Astra::Entity, const Position&, const PosS&) {});
    EXPECT_FALSE(reg.IsChanged<PosS>(e, since));
    reg.CreateView<const Position, PosS>().ForEach([](Astra::Entity, const Position&, PosS&) {});
    EXPECT_TRUE(reg.IsChanged<PosS>(e, since));

    auto ureg = MakeRegistry<Health>();
    Astra::Entity u = ureg->CreateEntity<Position>();
    since = ureg->CurrentTick();
    ureg->AdvanceTick();
    ureg->CreateView<const Position, const Health>().ForEach([](Astra::Entity, const Position&, const Health&) {});
    EXPECT_FALSE(ureg->IsChanged<Health>(u, since));
    ureg->CreateView<const Position, Health>().ForEach([](Astra::Entity, const Position&, Health&) {});
    EXPECT_TRUE(ureg->IsChanged<Health>(u, since));
}

// Table parity (D2): an Optional the visited chunk does not hold stamps nothing. The
// only PosS holder lives in an archetype the view never visits.
TEST(StorageViewMut, AbsentUntrackedSparseOptionalDoesNotStamp)
{
    Astra::Registry reg;
    Astra::Entity holder = reg.CreateEntity<Health>();
    reg.AddComponent(holder, PosS{});
    reg.CreateEntity<Position>();
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<Position, Astra::Optional<PosS>>().ForEach([](Astra::Entity, Position&, PosS*) {});
    EXPECT_FALSE(reg.IsChanged<PosS>(holder, since));
}

// Positive twin: a visited chunk that DOES hold the Optional stamps it.
TEST(StorageViewMut, PresentUntrackedSparseOptionalStamps)
{
    Astra::Registry reg;
    Astra::Entity holder = reg.CreateEntity<Position>();
    reg.AddComponent(holder, PosS{});
    reg.CreateEntity<Position>();
    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    reg.CreateView<Position, Astra::Optional<PosS>>().ForEach([](Astra::Entity, Position&, PosS*) {});
    EXPECT_TRUE(reg.IsChanged<PosS>(holder, since));
}
