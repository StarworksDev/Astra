#include <gtest/gtest.h>
#include <vector>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/SparseSet.hpp>
#include "../TestComponents.hpp"

using Astra::Entity;
using Astra::SparseSet;
using Astra::Tick;
using Astra::Test::Position;

namespace
{
    // See UniversalColumnTest.cpp: MakeDescriptor mints no ComponentID.
    template<typename T>
    Astra::ComponentDescriptor Desc()
    {
        return Astra::ComponentRegistry::MakeDescriptor<T>(0, nullptr);
    }

    Position* PosOf(SparseSet& set, Entity e) { return static_cast<Position*>(set.Get(e)); }
}

TEST(SparseSet, InsertGetContainsErase)
{
    SparseSet set(Desc<Position>());
    const Entity e(12, 1);
    EXPECT_FALSE(set.Contains(e));
    EXPECT_EQ(set.Get(e), nullptr);

    auto* p = static_cast<Position*>(set.Insert(e));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, Position{});                 // DefaultConstruct: value-initialised
    p->y = 4.0f;
    EXPECT_TRUE(set.Contains(e));
    EXPECT_EQ(set.Get(e), p);
    EXPECT_EQ(set.Size(), 1u);
    EXPECT_EQ(set.Entities()[0], e);
    EXPECT_EQ(set.DataAt(0), p);

    EXPECT_TRUE(set.Erase(e));
    EXPECT_FALSE(set.Erase(e));                // already gone
    EXPECT_FALSE(set.Contains(e));
    EXPECT_EQ(set.Size(), 0u);
}

TEST(SparseSet, SwapAndPopMovesTheTailIntoTheHole)
{
    SparseSet set(Desc<Position>());
    for (uint32_t id = 1; id <= 5; ++id)
    {
        Position src(static_cast<float>(id), 0.0f, 0.0f);
        (void)set.InsertMove(Entity(id, 1), &src);
    }
    ASSERT_TRUE(set.Erase(Entity(2, 1)));      // the hole at dense 1 takes the tail (id 5)

    const std::vector<Entity> dense(set.Entities(), set.Entities() + set.Size());
    EXPECT_EQ(dense, (std::vector<Entity>{Entity(1, 1), Entity(5, 1), Entity(3, 1), Entity(4, 1)}));
    for (uint32_t id : {1u, 3u, 4u, 5u})
    {
        Position* p = PosOf(set, Entity(id, 1));
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, static_cast<float>(id));   // the moved element's sparse entry was rewritten
    }
    ASSERT_TRUE(set.Erase(Entity(4, 1)));      // erasing the tail itself: a plain pop
    EXPECT_EQ(set.Size(), 3u);
    EXPECT_FLOAT_EQ(PosOf(set, Entity(3, 1))->x, 3.0f);
}

TEST(SparseSet, PagesAllocateOnFirstTouchAndReleaseWhenEmpty)
{
    SparseSet set(Desc<Position>());
    (void)set.Insert(Entity(3, 1));
    (void)set.Insert(Entity(5000, 1));
    (void)set.Insert(Entity(9000, 1));
    EXPECT_EQ(set.PageCount(), 3u);            // 4096-entry pages 0, 1 and 2
    ASSERT_TRUE(set.Erase(Entity(5000, 1)));
    EXPECT_EQ(set.PageCount(), 2u);
    EXPECT_TRUE(set.Contains(Entity(9000, 1)));
    set.Clear();
    EXPECT_EQ(set.PageCount(), 0u);
    EXPECT_EQ(set.Size(), 0u);
}

TEST(SparseSet, GrowthAndEraseRunEveryLifetimeHookExactlyOnce)
{
    using Astra::Test::Tracked;   // move-only, lifetime-counted: a skipped move ctor or dtor shows
    const int base = Tracked::s_live;
    {
        SparseSet set(Desc<Tracked>());
        for (int i = 0; i < 100; ++i)
        {
            Tracked src(i);
            (void)set.InsertMove(Entity(static_cast<uint32_t>(i), 1), &src);
        }
        EXPECT_GE(set.Capacity(), 100u);
        EXPECT_EQ(Tracked::s_live, base + 100);
        for (int i = 0; i < 100; ++i)
            EXPECT_EQ(static_cast<Tracked*>(set.Get(Entity(static_cast<uint32_t>(i), 1)))->value, i);
        for (int i = 0; i < 100; i += 2)
            ASSERT_TRUE(set.Erase(Entity(static_cast<uint32_t>(i), 1)));
        EXPECT_EQ(Tracked::s_live, base + 50);
        for (int i = 1; i < 100; i += 2)
            EXPECT_EQ(static_cast<Tracked*>(set.Get(Entity(static_cast<uint32_t>(i), 1)))->value, i);
        set.Clear();
        EXPECT_EQ(Tracked::s_live, base);
        (void)set.Insert(Entity(7, 1));
        EXPECT_EQ(Tracked::s_live, base + 1);
    }   // ~SparseSet
    EXPECT_EQ(Tracked::s_live, base);
}

TEST(SparseSet, TagStoresPresenceOnly)
{
    SparseSet set(Desc<Astra::Test::Player>());
    const Entity e(8, 1);
    EXPECT_EQ(set.Insert(e), Astra::EmptyComponentSentinel());
    EXPECT_TRUE(set.Contains(e));
    EXPECT_EQ(set.Get(e), Astra::EmptyComponentSentinel());
    EXPECT_EQ(set.DataAt(0), nullptr);         // no data buffer for an empty type
    EXPECT_EQ(set.Capacity(), 0u);
    EXPECT_FALSE(set.MarkWritten(e, 3));       // a tag has no value to write
    EXPECT_TRUE(set.Erase(e));
}

TEST(SparseSet, TicksArePerElementForTrackedAndAbsentOtherwise)
{
    Tick now = 3;
    SparseSet tracked(Desc<Astra::Test::TrackedPos>());
    tracked.SetTickSource(&now);
    const Entity a(1, 1), b(2, 1), c(3, 1);
    (void)tracked.Insert(a);
    now = 5;
    (void)tracked.Insert(b);
    (void)tracked.Insert(c);
    ASSERT_NE(tracked.TicksAt(0), nullptr);
    EXPECT_EQ(tracked.TicksAt(0)->added, 3u);
    now = 8;
    EXPECT_TRUE(tracked.MarkWritten(c, now));
    EXPECT_EQ(tracked.ChangedTick(c), 8u);
    EXPECT_EQ(tracked.ChangedTick(b), 5u);     // exact: b untouched
    ASSERT_TRUE(tracked.Erase(a));             // c (the tail) moves to dense 0 with its ticks
    EXPECT_EQ(tracked.Entities()[0], c);
    EXPECT_EQ(tracked.TicksAt(0)->changed, 8u);
    EXPECT_EQ(tracked.AddedTick(c), 5u);

    SparseSet plain(Desc<Position>());
    plain.SetTickSource(&now);
    now = 2;
    (void)plain.Insert(a);
    (void)plain.Insert(b);
    EXPECT_EQ(plain.TicksAt(0), nullptr);      // untracked: no per-element ticks
    now = 6;
    EXPECT_TRUE(plain.MarkWritten(a, now));    // present, non-tag: accepted; nothing per-element to stamp
    EXPECT_EQ(plain.ChangedTick(a), 0u);       // untracked: no store-level tier (ComponentStorage::CoarseVersion, Task 20)
    EXPECT_EQ(plain.AddedTick(b), 0u);
    EXPECT_EQ(plain.ChangedTick(Entity(1, 2)), 0u);   // stale handle: never
}

TEST(SparseSet, DisabledFlagsFollowSwapAndPop)
{
    using Astra::Test::TrackedVel;   // enableable
    SparseSet set(Desc<TrackedVel>());
    const Entity a(1, 1), b(2, 1), c(3, 1);
    (void)set.Insert(a);
    (void)set.Insert(b);
    (void)set.Insert(c);
    EXPECT_FALSE(set.IsDisabled(c));           // born enabled
    EXPECT_TRUE(set.SetDisabled(c, true));
    EXPECT_FALSE(set.SetDisabled(c, true));    // idempotent: no change
    ASSERT_TRUE(set.Erase(a));                 // c moves into dense 0...
    EXPECT_TRUE(set.IsDisabled(c));            // ...and keeps its flag
    EXPECT_FALSE(set.IsDisabled(b));
    EXPECT_TRUE(set.SetDisabled(c, false));

    SparseSet plain(Desc<Position>());
    (void)plain.Insert(a);
    EXPECT_FALSE(plain.SetDisabled(a, true));  // not enableable: nothing to store
    EXPECT_FALSE(plain.IsDisabled(a));
}

// ---- Review Focus 1 (Sparse), store level ----
// T1-C2 recycles ids at version+1. A handle to an erased or cleared occupant must
// miss on every accessor, and must never reach the element of the id's next owner.
TEST(SparseSetStaleHandle, EraseThenReinsertAtNextVersionMisses)
{
    using Astra::Test::TrackedVel;   // tracked + enableable: every per-element array exists
    SparseSet set(Desc<TrackedVel>());
    const Entity old(5, 1), fresh(5, 2);
    static_cast<TrackedVel*>(set.Insert(old))->dx = 1.0f;
    ASSERT_TRUE(set.Erase(old));
    static_cast<TrackedVel*>(set.Insert(fresh))->dx = 2.0f;

    EXPECT_FALSE(set.Contains(old));
    EXPECT_EQ(set.Get(old), nullptr);
    EXPECT_FALSE(set.Erase(old));              // must not erase the occupant
    EXPECT_FALSE(set.SetDisabled(old, true));  // must not disable the occupant
    EXPECT_FALSE(set.IsDisabled(old));
    EXPECT_FALSE(set.MarkWritten(old, 9));
    EXPECT_EQ(set.ChangedTick(old), 0u);

    ASSERT_TRUE(set.Contains(fresh));
    EXPECT_FALSE(set.IsDisabled(fresh));
    EXPECT_FLOAT_EQ(static_cast<TrackedVel*>(set.Get(fresh))->dx, 2.0f);
    EXPECT_EQ(set.Size(), 1u);
}

TEST(SparseSetStaleHandle, ClearThenReinsertAtNextVersionMisses)
{
    SparseSet set(Desc<Position>());
    const Entity old(5, 1), fresh(5, 2);
    static_cast<Position*>(set.Insert(old))->x = 1.0f;
    set.Clear();
    EXPECT_EQ(set.Get(old), nullptr);          // Clear released every page
    static_cast<Position*>(set.Insert(fresh))->x = 2.0f;
    EXPECT_EQ(set.Get(old), nullptr);
    EXPECT_FALSE(set.Contains(old));
    EXPECT_FALSE(set.Erase(old));
    EXPECT_FLOAT_EQ(PosOf(set, fresh)->x, 2.0f);
}

TEST(SparseSet, TicksByHandleMirrorTheDenseTicks)
{
    Tick now = 4;
    SparseSet tracked(Desc<Astra::Test::TrackedPos>());
    tracked.SetTickSource(&now);
    const Entity a(1, 1), b(2, 1);
    (void)tracked.Insert(a);
    now = 7;
    (void)tracked.Insert(b);
    ASSERT_NE(tracked.Ticks(b), nullptr);
    EXPECT_EQ(tracked.Ticks(b), tracked.TicksAt(1));
    EXPECT_EQ(tracked.Ticks(b)->added, 7u);
    const SparseSet& ctracked = tracked;
    ASSERT_NE(ctracked.Ticks(a), nullptr);
    EXPECT_EQ(ctracked.Ticks(a)->added, 4u);
    EXPECT_EQ(tracked.Ticks(Entity(2, 2)), nullptr);   // stale
    EXPECT_EQ(tracked.Ticks(Entity(9, 1)), nullptr);   // absent
    ASSERT_TRUE(tracked.Erase(b));
    EXPECT_EQ(tracked.Ticks(b), nullptr);

    SparseSet plain(Desc<Position>());
    (void)plain.Insert(a);
    EXPECT_EQ(plain.Ticks(a), nullptr);   // untracked
    const SparseSet& cplain = plain;
    EXPECT_EQ(cplain.Ticks(a), nullptr);
}
