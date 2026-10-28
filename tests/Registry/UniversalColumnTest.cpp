#include <gtest/gtest.h>
#include <vector>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/UniversalColumn.hpp>
#include "../TestComponents.hpp"

using Astra::Entity;
using Astra::Tick;
using Astra::UniversalColumn;

namespace
{
    // The registry's own descriptor factory, without registering anything: MakeDescriptor
    // reads TypeID<T>::Hash() (compile-time), never TypeID<T>::Value(), so these unit
    // tests mint no ComponentID. The id argument is irrelevant to the store.
    template<typename T>
    Astra::ComponentDescriptor Desc()
    {
        return Astra::ComponentRegistry::MakeDescriptor<T>(0, nullptr);
    }
}

TEST(UniversalColumn, ConstructGetDestroyRoundTrip)
{
    using Astra::Test::Position;
    UniversalColumn col(Desc<Position>());
    const Entity e(5, 1);
    EXPECT_EQ(col.Get(e), nullptr);
    EXPECT_FALSE(col.IsLive(e));

    auto* p = static_cast<Position*>(col.Construct(e));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, Position{});                 // DefaultConstruct: value-initialised
    p->x = 3.0f;
    EXPECT_EQ(col.Get(e), p);
    EXPECT_TRUE(col.IsLive(e));
    EXPECT_EQ(col.LiveCount(), 1u);
    EXPECT_EQ(col.PageCount(), 1u);

    col.Destroy(e);
    EXPECT_EQ(col.Get(e), nullptr);
    EXPECT_EQ(col.LiveCount(), 0u);
    EXPECT_EQ(col.PageCount(), 0u);            // the page's last live slot went: page released
}

TEST(UniversalColumn, ConstructMoveAndClearRunEveryLifetimeHook)
{
    using Astra::Test::Tracked;
    const int base = Tracked::s_live;
    {
        UniversalColumn col(Desc<Tracked>());
        Tracked src(41);
        auto* t = static_cast<Tracked*>(col.ConstructMove(Entity(2, 1), &src));
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->value, 41);
        (void)col.Construct(Entity(3, 1));
        (void)col.Construct(Entity(1500, 1));  // a second page
        EXPECT_EQ(Tracked::s_live, base + 4);  // src + three stored
        EXPECT_EQ(col.PageCount(), 2u);

        col.Clear();
        EXPECT_EQ(Tracked::s_live, base + 1);  // only src is left
        EXPECT_EQ(col.LiveCount(), 0u);
        EXPECT_EQ(col.PageCount(), 0u);

        (void)col.Construct(Entity(9, 1));     // the column is reusable after Clear
        EXPECT_EQ(Tracked::s_live, base + 2);
    }                                          // ~UniversalColumn destroys what is left
    EXPECT_EQ(Tracked::s_live, base);
}

TEST(UniversalColumn, ForEachLiveWalksAscendingIndexAcrossPages)
{
    using Astra::Test::Position;
    UniversalColumn col(Desc<Position>());
    const std::vector<Entity> ents{Entity(2100, 1), Entity(7, 3), Entity(1024, 1),
                                   Entity(0, 1), Entity(63, 2), Entity(64, 1)};
    for (Entity e : ents)
        static_cast<Position*>(col.Construct(e))->x = static_cast<float>(e.GetID());

    std::vector<Entity> seen;
    col.ForEachLive([&](Entity e, const void* p)
    {
        EXPECT_FLOAT_EQ(static_cast<const Position*>(p)->x, static_cast<float>(e.GetID()));
        seen.push_back(e);
    });
    const std::vector<Entity> expected{Entity(0, 1), Entity(7, 3), Entity(63, 2),
                                       Entity(64, 1), Entity(1024, 1), Entity(2100, 1)};
    EXPECT_EQ(seen, expected);
}

TEST(UniversalColumn, TagHasPresenceButNoData)
{
    UniversalColumn col(Desc<Astra::Test::Player>());
    const Entity e(4, 1);
    EXPECT_EQ(col.Construct(e), Astra::EmptyComponentSentinel());
    EXPECT_TRUE(col.IsLive(e));
    EXPECT_EQ(col.Get(e), Astra::EmptyComponentSentinel());
    EXPECT_FALSE(col.MarkWritten(e, 5));       // a tag has no value to write (Table: no column)
    col.Destroy(e);
    EXPECT_FALSE(col.IsLive(e));
}

TEST(UniversalColumn, TrackedTicksArePerSlotUntrackedIsPageCoarse)
{
    Tick now = 4;
    UniversalColumn tracked(Desc<Astra::Test::TrackedPos>());
    tracked.SetTickSource(&now);
    const Entity a(1, 1), b(2, 1);
    (void)tracked.Construct(a);
    now = 6;
    (void)tracked.Construct(b);
    ASSERT_NE(tracked.Ticks(a), nullptr);
    EXPECT_EQ(tracked.Ticks(a)->added, 4u);
    EXPECT_EQ(tracked.Ticks(b)->added, 6u);
    now = 9;
    EXPECT_TRUE(tracked.MarkWritten(a, now));
    EXPECT_EQ(tracked.ChangedTick(a), 9u);
    EXPECT_EQ(tracked.ChangedTick(b), 6u);     // exact: b was not written
    EXPECT_EQ(tracked.AddedTick(a), 4u);

    UniversalColumn plain(Desc<Astra::Test::Position>());
    plain.SetTickSource(&now);
    now = 2;
    (void)plain.Construct(a);
    (void)plain.Construct(b);
    EXPECT_EQ(plain.Ticks(a), nullptr);        // untracked: no per-slot ticks
    EXPECT_EQ(plain.AddedTick(a), 2u);
    now = 5;
    EXPECT_TRUE(plain.MarkWritten(a, now));
    EXPECT_EQ(plain.ChangedTick(b), 5u);       // page-coarse, the chunk-column-version twin
    EXPECT_EQ(plain.ChangedTick(Entity(1, 2)), 0u);   // stale handle: 0 == never
}

TEST(UniversalColumn, DisabledBitsExistOnlyForEnableableTypes)
{
    UniversalColumn en(Desc<Astra::Test::Timer>());
    const Entity e(3, 1);
    (void)en.Construct(e);
    EXPECT_FALSE(en.IsDisabled(e));            // born enabled
    EXPECT_TRUE(en.SetDisabled(e, true));
    EXPECT_FALSE(en.SetDisabled(e, true));     // idempotent: no change
    EXPECT_TRUE(en.IsDisabled(e));
    en.Destroy(e);
    (void)en.Construct(Entity(3, 2));
    EXPECT_FALSE(en.IsDisabled(Entity(3, 2))); // the next occupant is born enabled

    UniversalColumn plain(Desc<Astra::Test::Position>());
    (void)plain.Construct(e);
    EXPECT_FALSE(plain.SetDisabled(e, true));  // not enableable: nothing to store
    EXPECT_FALSE(plain.IsDisabled(e));
}

// Store-level half of Review Focus 1 (the registry-level pin is Task 9): an older
// version of a recycled id must miss on every accessor and never touch the occupant.
TEST(UniversalColumn, StaleHandleMissesAfterRecycleAndClear)
{
    using Astra::Test::TrackedVel;   // change-tracked AND enableable: every per-slot array exists
    UniversalColumn col(Desc<TrackedVel>());
    const Entity old(10, 1), fresh(10, 2);
    static_cast<TrackedVel*>(col.Construct(old))->dx = 7.0f;
    col.Destroy(old);
    auto* v = static_cast<TrackedVel*>(col.Construct(fresh));
    v->dx = 9.0f;

    EXPECT_EQ(col.Get(old), nullptr);
    EXPECT_FALSE(col.IsLive(old));
    EXPECT_EQ(col.Ticks(old), nullptr);
    EXPECT_FALSE(col.MarkWritten(old, 3));
    EXPECT_FALSE(col.SetDisabled(old, true));
    EXPECT_FALSE(col.IsDisabled(fresh));       // the stale write never reached the occupant
    col.Destroy(old);                          // stale destroy: a no-op
    ASSERT_TRUE(col.IsLive(fresh));
    EXPECT_FLOAT_EQ(v->dx, 9.0f);

    col.Clear();
    EXPECT_EQ(col.Get(fresh), nullptr);
    EXPECT_EQ(col.Get(Entity(10, 3)), nullptr);
}
