#include <gtest/gtest.h>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

using Astra::Entity;
using Astra::Registry;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace
{
    struct InitLog { int calls = 0; };

    // A registry init (s4.2 step 2): stamps a recognisable per-entity value so a test can
    // tell "init ran" from "default-constructed" from "moved in".
    void InitPosition(Entity e, void* component, void* user)
    {
        static_cast<Position*>(component)->x = 100.0f + static_cast<float>(e.GetID());
        ++static_cast<InitLog*>(user)->calls;
    }

    Registry::Config UniversalPositionConfig(InitLog* log)
    {
        Registry::Config cfg;
        if (log) cfg.AddUniversal<Position>(&InitPosition, log);
        else     cfg.AddUniversal<Position>();
        return cfg;
    }

    bool MaskHas(const Registry& reg, Entity e, Astra::ComponentID id)
    {
        const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        return rec && rec->archetype->GetMask().Test(id);
    }
}

TEST(UniversalStorage, CreateEntityConstructsEveryUniversalThenRunsInit)
{
    InitLog log;
    Registry reg(UniversalPositionConfig(&log));
    const Entity e = reg.CreateEntity();

    ASSERT_TRUE(reg.HasComponent<Position>(e));
    const Position* p = std::as_const(reg).GetComponent<Position>(e);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 100.0f + static_cast<float>(e.GetID()));   // init ran...
    EXPECT_FLOAT_EQ(p->y, 0.0f);                                      // ...over a value-initialised slot
    EXPECT_EQ(log.calls, 1);
    EXPECT_FALSE(MaskHas(reg, e, Astra::TypeID<Position>::Value())); // never an archetype column
}

TEST(UniversalStorage, EveryCreatePathConstructsUniversals)
{
    InitLog log;
    Registry reg(UniversalPositionConfig(&log));
    std::vector<Entity> made;
    made.push_back(reg.CreateEntity<Velocity>());
    made.push_back(reg.CreateEntityWith(Velocity{1.0f, 2.0f, 3.0f}));

    std::vector<Entity> batch(4);
    ASSERT_EQ(reg.CreateEntities<Velocity>(4, std::span{batch}), 4u);
    made.insert(made.end(), batch.begin(), batch.end());
    ASSERT_EQ((reg.CreateEntitiesWith<Velocity>(4, std::span{batch},
        [](size_t i) { return std::tuple{Velocity{static_cast<float>(i), 0.0f, 0.0f}}; })), 4u);
    made.insert(made.end(), batch.begin(), batch.end());

    {
        Astra::CommandBuffer cb(&reg);   // declared after the Registry (CommandBlockArena order)
        const Entity viaCmd = cb.CreateEntity();
        Entity cmdBatch[3];
        cb.CreateEntities(3, cmdBatch);
        EXPECT_FALSE(cb.Execute().IsErr());
        made.push_back(viaCmd);
        made.insert(made.end(), std::begin(cmdBatch), std::end(cmdBatch));
    }

    ASSERT_EQ(made.size(), 14u);
    for (Entity e : made)
    {
        ASSERT_TRUE(reg.IsValid(e));
        const Position* p = std::as_const(reg).GetComponent<Position>(e);
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, 100.0f + static_cast<float>(e.GetID()));
    }
    EXPECT_EQ(log.calls, 14);
}

TEST(UniversalStorage, ComponentAddedFiresOncePerUniversalAfterEntityCreated)
{
    Registry reg(UniversalPositionConfig(nullptr));
    reg.EnableSignals(Astra::Signal::EntityCreated | Astra::Signal::ComponentAdded);
    std::vector<std::string> order;
    auto* signals = reg.GetSignalManager();
    auto created = signals->On<Astra::Events::EntityCreated>().Register(
        [&](const Astra::Events::EntityCreated&) { order.push_back("created"); });
    auto added = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev)
        {
            if (ev.componentId == Astra::TypeID<Position>::Value())
            {
                EXPECT_EQ(static_cast<const void*>(ev.component),
                          static_cast<const void*>(std::as_const(reg).GetComponent<Position>(ev.entity)));
                order.push_back("position");
            }
            else
            {
                order.push_back("other");
            }
        });

    (void)reg.CreateEntityWith(Velocity{});
    (void)reg.CreateEntity();
    EXPECT_EQ(order, (std::vector<std::string>{"created", "other", "position", "created", "position"}));

    signals->On<Astra::Events::EntityCreated>().Unregister(created);
    signals->On<Astra::Events::ComponentAdded>().Unregister(added);
}

TEST(UniversalStorage, BatchCreatorsFireUniversalComponentAddedOncePerEntity)
{
    Registry reg(UniversalPositionConfig(nullptr));
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    size_t positionAdds = 0;
    auto* signals = reg.GetSignalManager();
    auto added = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev)
        {
            if (ev.componentId == Astra::TypeID<Position>::Value())
                ++positionAdds;
        });

    std::vector<Entity> batch(5);
    ASSERT_EQ(reg.CreateEntities<Velocity>(5, std::span{batch}), 5u);
    EXPECT_EQ(positionAdds, 5u);

    positionAdds = 0;
    ASSERT_EQ((reg.CreateEntitiesWith<Velocity>(5, std::span{batch},
        [](size_t i) { return std::tuple{Velocity{static_cast<float>(i), 0.0f, 0.0f}}; })), 5u);
    EXPECT_EQ(positionAdds, 5u);

    signals->On<Astra::Events::ComponentAdded>().Unregister(added);
}

TEST(UniversalStorage, DestroyClearAndRegistryDestructionDestroyUniversalValues)
{
    using Astra::Test::Tracked;   // move-only, lifetime-counted
    const int base = Tracked::s_live;
    {
        Registry::Config cfg;
        cfg.AddUniversal<Tracked>();
        Registry reg(cfg);
        std::vector<Entity> ents(4);
        ASSERT_EQ(reg.CreateEntities<Velocity>(4, std::span{ents}), 4u);
        EXPECT_EQ(Tracked::s_live, base + 4);

        reg.DestroyEntity(ents[0]);
        EXPECT_EQ(Tracked::s_live, base + 3);
        reg.DestroyEntities(std::span{ents}.subspan(1, 2));
        EXPECT_EQ(Tracked::s_live, base + 1);
        reg.Clear();
        EXPECT_EQ(Tracked::s_live, base);

        (void)reg.CreateEntity();
        (void)reg.CreateEntity();
        EXPECT_EQ(Tracked::s_live, base + 2);
    }   // ~Registry destroys the rest
    EXPECT_EQ(Tracked::s_live, base);
}

// ---- Review Focus 1 (Universal): stale handles after destroy-recycle and Clear ----
// T1-C2: ids come back at version+1. A handle to the previous occupant must miss on
// every surface and must never read or write the new occupant's slot. Tasks 12/13
// extend this suite with Add, Remove and SetEnabled.

TEST(UniversalStorageStaleHandle, DestroyRecycleMissesOnGetAndHas)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity stale = reg.CreateEntity();
    Position* before = reg.GetComponent<Position>(stale);
    ASSERT_NE(before, nullptr);
    before->x = 7.0f;
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<Position>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<Position>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<Position>(stale));

    const Position* now = std::as_const(reg).GetComponent<Position>(fresh);
    ASSERT_NE(now, nullptr);
    EXPECT_FLOAT_EQ(now->x, 0.0f);             // reconstructed, not the stale value
}

TEST(UniversalStorageStaleHandle, BatchDestroyRecycleMisses)
{
    Registry reg(UniversalPositionConfig(nullptr));
    std::vector<Entity> ents(3);
    ASSERT_EQ(reg.CreateEntities<>(3, std::span{ents}), 3u);
    Position* before = reg.GetComponent<Position>(ents[1]);
    ASSERT_NE(before, nullptr);
    before->x = 5.0f;
    reg.DestroyEntities(std::span{ents});
    std::vector<Entity> again(3);
    ASSERT_EQ(reg.CreateEntities<>(3, std::span{again}), 3u);
    for (Entity stale : ents)
    {
        EXPECT_EQ(reg.GetComponent<Position>(stale), nullptr);
        EXPECT_FALSE(reg.HasComponent<Position>(stale));
    }
    for (Entity e : again)
    {
        const Position* p = std::as_const(reg).GetComponent<Position>(e);
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, 0.0f);
    }
}

TEST(UniversalStorageStaleHandle, ClearRecycleMissesOnGetAndHas)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity stale = reg.CreateEntity();
    Position* before = reg.GetComponent<Position>(stale);
    ASSERT_NE(before, nullptr);
    before->x = 7.0f;
    reg.Clear();
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<Position>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<Position>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<Position>(stale));
    const Position* now = std::as_const(reg).GetComponent<Position>(fresh);
    ASSERT_NE(now, nullptr);
    EXPECT_FLOAT_EQ(now->x, 0.0f);
}

namespace
{
    struct AssertCount { int n = 0; };

    Astra::AssertAction CountAndContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++static_cast<AssertCount*>(user)->n;
        return Astra::AssertAction::Continue;
    }

#if MOSAIC_ASSERTS_ACTIVE
    constexpr int kAssertsPerRefusal = 1;   // Debug (or a checked build): the refusal asserts
#else
    constexpr int kAssertsPerRefusal = 0;   // Release: compiled out, returns false
#endif
}

TEST(UniversalStorage, AddOnAPresentUniversalIsANoOpReturningTrue)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity e = reg.CreateEntity();
    reg.GetComponent<Position>(e)->x = 4.0f;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int added = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded&) { ++added; });

    EXPECT_TRUE(reg.AddComponent<Position>(e, Position{9.0f, 9.0f, 9.0f}));
    EXPECT_TRUE(reg.EmplaceComponent<Position>(e, 8.0f, 8.0f, 8.0f));
    std::vector<Entity> one{e};
    reg.AddComponents<Position>(std::span{one}, Position{7.0f, 7.0f, 7.0f});
    reg.EmplaceComponents<Position>(std::span{one}, 6.0f, 6.0f, 6.0f);
    const Position value(5.0f, 5.0f, 5.0f);
    EXPECT_FALSE(reg.AddComponentByID(e, Astra::TypeID<Position>::Value(), &value, sizeof(Position)));   // by-id Table rule for "present"
    EXPECT_EQ(reg.AddComponentsByID(std::span{one}, Astra::TypeID<Position>::Value(), &value, sizeof(Position)), 0u);

    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 4.0f);   // untouched
    EXPECT_EQ(added, 0);                                                     // no signal
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(UniversalStorage, RemoveIsRefusedOnEveryFormWithoutASignal)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity e = reg.CreateEntity();
    reg.GetComponent<Position>(e)->x = 4.0f;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);
    int removed = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved&) { ++removed; });

    AssertCount count;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        std::vector<Entity> one{e};
        EXPECT_FALSE(reg.RemoveComponent<Position>(e));
        EXPECT_EQ(reg.RemoveComponents<Position>(std::span{one}), 0u);
        EXPECT_FALSE(reg.RemoveComponentByID(e, Astra::TypeID<Position>::Value()));
        EXPECT_EQ(reg.RemoveComponentsByID(std::span{one}, Astra::TypeID<Position>::Value()), 0u);
    }
    EXPECT_EQ(count.n, 4 * kAssertsPerRefusal);   // one refusal per call
    EXPECT_EQ(removed, 0);
    ASSERT_TRUE(reg.HasComponent<Position>(e));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 4.0f);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(h);
}

TEST(UniversalStorage, CommandBufferRemoveOfAUniversalIsRefusedAtExecute)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity e = reg.CreateEntity();
    AssertCount count;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        Astra::CommandBuffer cb(&reg);
        cb.RemoveComponent<Position>(e);
        (void)cb.Execute();   // the executor reports the refusal as a failed command
    }
    EXPECT_EQ(count.n, kAssertsPerRefusal);
    EXPECT_TRUE(reg.HasComponent<Position>(e));
}

// ---- Review Focus 1 (Universal), Add/Remove arms ----
TEST(UniversalStorageStaleHandle, AddAndRemoveMissQuietlyAndNeverTouchTheOccupant)
{
    Registry reg(UniversalPositionConfig(nullptr));
    const Entity stale = reg.CreateEntity();
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    reg.GetComponent<Position>(fresh)->x = 3.0f;

    AssertCount count;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        std::vector<Entity> staleOnly{stale};
        const Position value(9.0f, 9.0f, 9.0f);
        EXPECT_FALSE(reg.AddComponent<Position>(stale, value));
        EXPECT_FALSE(reg.EmplaceComponent<Position>(stale, 9.0f, 9.0f, 9.0f));
        EXPECT_FALSE(reg.AddComponentByID(stale, Astra::TypeID<Position>::Value(), &value, sizeof(Position)));
        EXPECT_FALSE(reg.RemoveComponent<Position>(stale));
        EXPECT_EQ(reg.RemoveComponents<Position>(std::span{staleOnly}), 0u);
        EXPECT_FALSE(reg.RemoveComponentByID(stale, Astra::TypeID<Position>::Value()));
        EXPECT_EQ(reg.RemoveComponentsByID(std::span{staleOnly}, Astra::TypeID<Position>::Value()), 0u);
    }
    EXPECT_EQ(count.n, 0);                     // a stale handle is a miss, not a refusal
    ASSERT_TRUE(reg.HasComponent<Position>(fresh));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(fresh)->x, 3.0f);
}

namespace
{
    Registry::Config UniversalTimerConfig()
    {
        Registry::Config cfg;
        cfg.AddUniversal<Astra::Test::Timer>();   // enableable
        cfg.AddUniversal<Position>();             // not enableable
        return cfg;
    }
}

TEST(UniversalStorage, SetEnabledTogglesTheColumnFlagAndSignalsOnChangeOnly)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity e = reg.CreateEntity();
    const Entity other = reg.CreateEntity();
    reg.EnableSignals(Astra::Signal::ComponentEnabled | Astra::Signal::ComponentDisabled);
    int enabledSignals = 0, disabledSignals = 0;
    const void* payload = nullptr;
    auto* signals = reg.GetSignalManager();
    auto hOn = signals->On<Astra::Events::ComponentEnabled>().Register(
        [&](const Astra::Events::ComponentEnabled& ev) { ++enabledSignals; payload = ev.component; });
    auto hOff = signals->On<Astra::Events::ComponentDisabled>().Register(
        [&](const Astra::Events::ComponentDisabled& ev) { ++disabledSignals; payload = ev.component; });

    EXPECT_TRUE(reg.IsEnabled<Timer>(e));                  // born enabled
    EXPECT_TRUE(reg.SetEnabled<Timer>(e, false));
    EXPECT_FALSE(reg.IsEnabled<Timer>(e));
    EXPECT_TRUE(reg.IsEnabled<Timer>(other));              // per entity
    EXPECT_EQ(disabledSignals, 1);
    EXPECT_EQ(payload, static_cast<const void*>(std::as_const(reg).GetComponent<Timer>(e)));
    EXPECT_TRUE(reg.SetEnabled<Timer>(e, false));          // idempotent: still true...
    EXPECT_EQ(disabledSignals, 1);                         // ...but silent
    EXPECT_TRUE(reg.HasComponent<Timer>(e));               // existence never lies
    EXPECT_NE(std::as_const(reg).GetComponent<Timer>(e), nullptr);
    EXPECT_TRUE(reg.SetEnabled<Timer>(e, true));
    EXPECT_TRUE(reg.IsEnabled<Timer>(e));
    EXPECT_EQ(enabledSignals, 1);

    // A non-enableable universal: present, always enabled, nothing to toggle.
    EXPECT_FALSE(reg.SetEnabledByID(e, Astra::TypeID<Position>::Value(), false));
    EXPECT_TRUE(reg.IsEnabledByID(e, Astra::TypeID<Position>::Value()));

    signals->On<Astra::Events::ComponentEnabled>().Unregister(hOn);
    signals->On<Astra::Events::ComponentDisabled>().Unregister(hOff);
}

TEST(UniversalStorage, CommandBufferSetEnabledReachesTheColumn)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity e = reg.CreateEntity();
    {
        Astra::CommandBuffer cb(&reg);
        cb.SetEnabled<Timer>(e, false);
        EXPECT_FALSE(cb.Execute().IsErr());
    }
    EXPECT_FALSE(reg.IsEnabled<Timer>(e));
}

TEST(UniversalStorage, DestroyRecycleBringsTheNextOccupantBackEnabled)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.SetEnabled<Timer>(e, false));
    reg.DestroyEntity(e);
    const Entity next = reg.CreateEntity();
    ASSERT_EQ(next.GetID(), e.GetID());
    EXPECT_TRUE(reg.IsEnabled<Timer>(next));               // born enabled, like any component
}

// ---- Review Focus 1 (Universal), SetEnabled arm ----
TEST(UniversalStorageStaleHandle, SetEnabledMissesAndNeverTogglesTheOccupant)
{
    using Astra::Test::Timer;
    Registry reg(UniversalTimerConfig());
    const Entity stale = reg.CreateEntity();
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());

    EXPECT_FALSE(reg.SetEnabled<Timer>(stale, false));
    EXPECT_FALSE(reg.IsEnabled<Timer>(stale));             // stale: "not enabled" (Table answer)
    EXPECT_TRUE(reg.IsEnabled<Timer>(fresh));              // the occupant was never disabled

    reg.Clear();
    const Entity afterClear = reg.CreateEntity();
    ASSERT_EQ(afterClear.GetID(), fresh.GetID());
    EXPECT_FALSE(reg.SetEnabled<Timer>(fresh, false));
    EXPECT_TRUE(reg.IsEnabled<Timer>(afterClear));
}

namespace
{
    void AdvanceTo(Registry& reg, Astra::Tick t)
    {
        while (reg.CurrentTick() < t)
            reg.AdvanceTick();
    }

    Registry::Config TickConfig()
    {
        Registry::Config cfg;
        cfg.AddUniversal<Astra::Test::TrackedPos>();   // change-tracked: exact
        cfg.AddUniversal<Position>();                  // untracked: page-coarse
        cfg.AddUniversal<Astra::Test::Player>();       // tag: no value
        return cfg;
    }
}

TEST(UniversalStorageTicks, TrackedIsExactPerEntity)
{
    using Astra::Test::TrackedPos;
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity a = reg.CreateEntity();
    const Entity b = reg.CreateEntity();
    EXPECT_TRUE(reg.IsAdded<TrackedPos>(a, 1));           // added at 2
    EXPECT_FALSE(reg.IsAdded<TrackedPos>(a, 2));
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(a, 2));

    AdvanceTo(reg, 3);
    reg.GetComponent<TrackedPos>(a)->x = 1.0f;            // a mutable Get is a write
    EXPECT_TRUE(reg.IsChanged<TrackedPos>(a, 2));
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(b, 2));        // exact: b untouched
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(a, 3));        // not newer than its own tick
    EXPECT_FALSE(reg.IsAdded<TrackedPos>(a, 2));          // a write is not a re-add

    AdvanceTo(reg, 4);
    (void)std::as_const(reg).GetComponent<TrackedPos>(b); // a const Get never stamps
    (void)reg.IsChanged<TrackedPos>(b, 0);                // nor does the query
    EXPECT_FALSE(reg.IsChanged<TrackedPos>(b, 3));
    EXPECT_TRUE(reg.Modified<TrackedPos>(b));
    EXPECT_TRUE(reg.IsChanged<TrackedPos>(b, 3));
    EXPECT_TRUE(reg.Modified(a, Astra::TypeID<TrackedPos>::Value()));
    EXPECT_TRUE(reg.IsChanged<TrackedPos>(a, 3));
}

TEST(UniversalStorageTicks, UntrackedIsPageCoarse)
{
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity a = reg.CreateEntity();
    const Entity b = reg.CreateEntity();
    AdvanceTo(reg, 3);
    EXPECT_FALSE(reg.IsChanged<Position>(a, 2));
    reg.GetComponent<Position>(a)->x = 1.0f;
    EXPECT_TRUE(reg.IsChanged<Position>(a, 2));
    EXPECT_TRUE(reg.IsChanged<Position>(b, 2));           // same page: coarse, like a chunk column version
    EXPECT_TRUE(reg.IsAdded<Position>(b, 2));             // untracked IsAdded reads the same version (README caveat)
}

TEST(UniversalStorageTicks, SetIfNeqStampsOnlyOnInequality)
{
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity e = reg.CreateEntity();
    AdvanceTo(reg, 3);
    EXPECT_FALSE(reg.SetIfNeq<Position>(e, Position{}));               // equal: no store, no stamp
    EXPECT_FALSE(reg.IsChanged<Position>(e, 2));
    EXPECT_TRUE(reg.SetIfNeq<Position>(e, Position{9.0f, 0.0f, 0.0f})); // different: stored + stamped
    EXPECT_TRUE(reg.IsChanged<Position>(e, 2));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 9.0f);
    EXPECT_TRUE(reg.SetIfNeq<Astra::Test::TrackedPos>(e, Astra::Test::TrackedPos{1.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(reg.IsChanged<Astra::Test::TrackedPos>(e, 2));
}

TEST(UniversalStorageTicks, TagStaleAndInvalidAnswerFalseAndNeverStampTheOccupant)
{
    using Astra::Test::Player;
    Registry reg(TickConfig());
    AdvanceTo(reg, 2);
    const Entity e = reg.CreateEntity();
    EXPECT_FALSE(reg.Modified<Player>(e));                    // a tag has no value (Table: no column)
    EXPECT_FALSE(reg.Modified(e, Astra::TypeID<Player>::Value()));
    EXPECT_FALSE(reg.IsChanged<Player>(e, 0));
    EXPECT_FALSE(reg.IsAdded<Player>(e, 0));
    EXPECT_FALSE(reg.Modified<Position>(Astra::Entity{}));
    EXPECT_FALSE(reg.IsChanged<Position>(Astra::Entity{}, 0));

    // Review Focus 1 (Universal), tick arm: the recycled id's stale handle stamps nothing.
    reg.DestroyEntity(e);
    const Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), e.GetID());
    AdvanceTo(reg, 5);
    EXPECT_FALSE(reg.Modified<Position>(e));
    EXPECT_FALSE(reg.IsChanged<Position>(e, 0));
    EXPECT_FALSE(reg.SetIfNeq<Position>(e, Position{1.0f, 1.0f, 1.0f}));
    EXPECT_FALSE(reg.IsChanged<Position>(fresh, 4));          // the occupant was not stamped
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(fresh)->x, 0.0f);
}
