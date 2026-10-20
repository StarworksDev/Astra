#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <set>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    using S = std::set<Astra::Entity>;
    struct FakeCtx { Astra::Tick last; Astra::Tick LastRun() const noexcept { return last; } };
}

TEST(StorageViewChange, TrackedSparseChangedAndAddedArePerEntity)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 6; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), SpE{float(i)});
    }
    const Astra::Tick t0 = reg.CurrentTick();
    reg.AdvanceTick();
    ASSERT_TRUE(reg.Modified<SpE>(es[1]));
    Astra::Entity late = reg.CreateEntity<Position>();
    reg.AddComponent(late, SpE{});

    S changed, added, changedWithTable;
    reg.CreateView<const SpE, Astra::Changed<SpE>>().Since(t0).ForEach([&](Astra::Entity e, const SpE&) { changed.insert(e); });
    reg.CreateView<const SpE, Astra::Added<SpE>>().ForEach(FakeCtx{t0}, [&](Astra::Entity e, const SpE&) { added.insert(e); });
    reg.CreateView<const Position, const SpE, Astra::Changed<SpE>>().Since(t0).ForEach(
        [&](Astra::Entity e, const Position&, const SpE&) { changedWithTable.insert(e); });
    EXPECT_EQ(changed, (S{es[1], late}));
    EXPECT_EQ(added, (S{late}));
    EXPECT_EQ(changedWithTable, (S{es[1], late}));

    // Agreement with Registry::IsChanged / IsAdded for every holder.
    for (Astra::Entity e : es)
    {
        EXPECT_EQ(changed.count(e) != 0, reg.IsChanged<SpE>(e, t0));
        EXPECT_EQ(added.count(e) != 0, reg.IsAdded<SpE>(e, t0));
    }

    // A disabled member is still filtered out under Changed<T> (enabled filter first).
    ASSERT_TRUE(reg.SetEnabled<SpE>(es[1], false));
    S again;
    reg.CreateView<const SpE, Astra::Changed<SpE>>().Since(t0).ForEach([&](Astra::Entity e, const SpE&) { again.insert(e); });
    EXPECT_EQ(again, (S{late}));
}

// Untracked Sparse: the per-component coarse version -- all members or none.
TEST(StorageViewChange, UntrackedSparseIsCoarse)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 4; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), PosS{});
    }
    const Astra::Tick t0 = reg.CurrentTick();
    reg.AdvanceTick();
    size_t n = 0;
    reg.CreateView<const Position, const PosS, Astra::Changed<PosS>>().Since(t0).ForEach([&](Astra::Entity, const Position&, const PosS&) { ++n; });
    EXPECT_EQ(n, 0u);
    ASSERT_TRUE(reg.Modified<PosS>(es[2]));
    reg.CreateView<const Position, const PosS, Astra::Changed<PosS>>().Since(t0).ForEach([&](Astra::Entity, const Position&, const PosS&) { ++n; });
    EXPECT_EQ(n, 4u);
}

TEST(StorageViewChange, TrackedUniversalIsPerEntity)
{
    auto reg = MakeRegistry<TrackedPos>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 5; ++i) es.push_back(reg->CreateEntity<Position>());
    const Astra::Tick t0 = reg->CurrentTick();
    reg->AdvanceTick();
    ASSERT_TRUE(reg->Modified<TrackedPos>(es[0]));
    Astra::Entity late = reg->CreateEntity<Position>();

    S changed, added;
    reg->CreateView<const Position, const TrackedPos, Astra::Changed<TrackedPos>>().Since(t0).ForEach(
        [&](Astra::Entity e, const Position&, const TrackedPos&) { changed.insert(e); });
    reg->CreateView<const TrackedPos, Astra::Added<TrackedPos>>().Since(t0).ForEach(
        [&](Astra::Entity e, const TrackedPos&) { added.insert(e); });
    EXPECT_EQ(changed, (S{es[0], late}));
    EXPECT_EQ(added, (S{late}));
    for (Astra::Entity e : es)
    {
        EXPECT_EQ(changed.count(e) != 0, reg->IsChanged<TrackedPos>(e, t0));
        EXPECT_EQ(added.count(e) != 0, reg->IsAdded<TrackedPos>(e, t0));
    }
}

// Untracked Universal: page-coarse -- every live slot of a written page answers.
TEST(StorageViewChange, UntrackedUniversalIsPageCoarse)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 3; ++i) es.push_back(reg->CreateEntity<Position>());
    const Astra::Tick t0 = reg->CurrentTick();
    reg->AdvanceTick();
    size_t n = 0;
    auto count = [&] {
        n = 0;
        reg->CreateView<const Health, Astra::Changed<Health>>().Since(t0).ForEach([&](Astra::Entity, const Health&) { ++n; });
        return n;
    };
    EXPECT_EQ(count(), 0u);
    ASSERT_TRUE(reg->Modified<Health>(es[0]));
    EXPECT_EQ(count(), 3u);    // ids 0..2 share one 1024-slot page
}
