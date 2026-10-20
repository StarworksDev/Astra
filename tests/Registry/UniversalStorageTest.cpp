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
