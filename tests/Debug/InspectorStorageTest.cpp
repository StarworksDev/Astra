// Inspector storage reporting + the "sparse-shaped" advisory (spec 2026-10-01 s9.4).
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Debug/Inspector.hpp>
#include "../Storage/StorageParity.hpp"
#include "../Support/StorageOverride.hpp"

namespace
{
    using Astra::Test::Health;
    using Astra::Test::Name;
    using Astra::Test::Physics;
    using Astra::Test::Position;
    using Astra::Test::Timer;
    using Astra::Test::Transform;
    using Astra::Test::Velocity;
    using Astra::Test::Parity::ParityHierS;
    using Astra::Test::Parity::ParityPosS;
    using Astra::Test::Parity::ParityTagS;

    const Astra::Debug::StoreInfo* FindStore(const Astra::Debug::InspectorSnapshot& snap, Astra::ComponentID id)
    {
        for (const auto& s : snap.components)
            if (s.id == id) return &s;
        return nullptr;
    }

    // Name (non-enableable) and Timer (enableable) are Universal in this registry.
    std::unique_ptr<Astra::Registry> MakeRegistry()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position, Velocity, Health, Physics, Transform, Name, Timer,
                                 ParityPosS, ParityTagS, ParityHierS>();
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>();
        cfg.AddUniversal<Timer>();
        return std::make_unique<Astra::Registry>(creg, cfg);
    }
}

TEST(InspectorStorage, ReportsKindPopulationPagesAndPresenceBlocks)
{
    auto reg = MakeRegistry();
    std::vector<Astra::Entity> es(2000);
    ASSERT_EQ(reg->CreateEntities<Position>(2000, std::span{es}), 2000u);
    for (size_t i = 0; i < 2000; i += 4) ASSERT_TRUE(reg->AddComponent(es[i], ParityPosS{}));   // 500 holders
    for (size_t i = 0; i < 2000; i += 8) ASSERT_TRUE(reg->SetEnabled<Timer>(es[i], false));     // 250 disabled

    const auto snap = Astra::Debug::Capture(*reg);
    EXPECT_EQ(snap.registry.sparseSlotsUsed, 2u);   // ParityPosS + Timer's disabled mirror

    const auto* pos = FindStore(snap, Astra::TypeID<Position>::Value());
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->kind, Astra::StorageKind::Table);
    EXPECT_EQ(pos->population, 2000u);
    EXPECT_EQ(pos->archetypeCount, 1u);          // Sparse membership never fragments
    EXPECT_EQ(pos->sparseSlot, -1);
    EXPECT_EQ(pos->pageCount, 0u);

    const auto* ps = FindStore(snap, Astra::TypeID<ParityPosS>::Value());
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->kind, Astra::StorageKind::Sparse);
    EXPECT_GE(ps->sparseSlot, 0);
    EXPECT_EQ(ps->population, 500u);
    EXPECT_GE(ps->pageCount, 1u);
    EXPECT_GE(ps->presenceBlockCount, 1u);
    EXPECT_EQ(ps->presenceBitsSet, 500u);        // P1, as the Inspector sees it

    const auto* name = FindStore(snap, Astra::TypeID<Name>::Value());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->kind, Astra::StorageKind::Universal);
    EXPECT_EQ(name->population, 2000u);
    EXPECT_EQ(name->pageCount, 2u);              // indices < 2048 -> pages 0 and 1 (1024 slots each)
    EXPECT_EQ(name->sparseSlot, -1);             // non-enableable Universal: no slot, no blocks

    const auto* timer = FindStore(snap, Astra::TypeID<Timer>::Value());
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->kind, Astra::StorageKind::Universal);
    EXPECT_GE(timer->sparseSlot, 0);             // enableable Universal: slot for the disabled mirror
    EXPECT_EQ(timer->disabledCount, 250u);       // P3, as the Inspector sees it

    size_t bits = 0, blocks = 0;
    for (const auto& a : snap.archetypes)
        for (const auto& ch : a.chunks)
            for (const auto& b : ch.sparseBlocks)
                if (b.componentId == Astra::TypeID<ParityPosS>::Value())
                {
                    ++blocks;
                    bits += b.presentCount;
                    EXPECT_TRUE(b.hasPresentWords);
                    EXPECT_GT(ch.sparseBlockBytes, 0u);
                }
    EXPECT_EQ(bits, 500u);
    EXPECT_EQ(blocks, ps->presenceBlockCount);
}

TEST(InspectorStorage, CaptureNeverCreatesStoresOrSlots)
{
    // A fresh registry: the Sparse tag, the declared Universals and the (enableable)
    // Sparse components have no store, no column and no slot yet.
    auto reg = MakeRegistry();
    const Astra::ComponentStorage* storage = reg->GetComponentStorage();
    ASSERT_NE(storage, nullptr);
    const Astra::ComponentID tagId = Astra::TypeID<ParityTagS>::Value();
    const Astra::ComponentID nameId = Astra::TypeID<Name>::Value();
    const Astra::ComponentID timerId = Astra::TypeID<Timer>::Value();
    const bool nameColumnBefore = storage->FindUniversal(nameId) != nullptr;
    const bool timerColumnBefore = storage->FindUniversal(timerId) != nullptr;
    const size_t slotsBefore = storage->UsedSparseSlotCount();
    ASSERT_EQ(storage->FindSparse(tagId), nullptr);

    for (int pass = 0; pass < 2; ++pass)
    {
        const auto snap = Astra::Debug::Capture(*reg);
        const auto* tag = FindStore(snap, tagId);
        ASSERT_NE(tag, nullptr);                   // Sparse kinds are listed even when unused...
        EXPECT_EQ(tag->kind, Astra::StorageKind::Sparse);
        EXPECT_EQ(tag->sparseSlot, -1);            // ...but looking acquires no slot
        EXPECT_EQ(tag->population, 0u);

        // A declared-but-empty Universal is listed as Universal with nothing behind it.
        const auto* name = FindStore(snap, nameId);
        ASSERT_NE(name, nullptr);
        EXPECT_EQ(name->kind, Astra::StorageKind::Universal);
        EXPECT_EQ(name->population, 0u);
        EXPECT_EQ(name->pageCount, 0u);
        const auto* timer = FindStore(snap, timerId);
        ASSERT_NE(timer, nullptr);
        EXPECT_EQ(timer->kind, Astra::StorageKind::Universal);
        EXPECT_EQ(timer->population, 0u);

        EXPECT_EQ(storage->FindSparse(tagId), nullptr);
        EXPECT_EQ(storage->FindUniversal(nameId) != nullptr, nameColumnBefore);
        EXPECT_EQ(storage->FindUniversal(timerId) != nullptr, timerColumnBefore);
        EXPECT_EQ(storage->UsedSparseSlotCount(), slotsBefore);
        EXPECT_EQ(snap.registry.sparseSlotsUsed, slotsBefore);
    }
}

TEST(InspectorStorage, ASuspendedUniversalIsReportedTruthfullyWithoutRebuildingItsColumn)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;   // ComponentModule needs an installed type context
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    auto mod = Astra::ComponentModule::Open(creg, "InspectorPlugin");
    ASSERT_TRUE(static_cast<bool>(mod));
    mod.Register<Name>();
    const Astra::ComponentID nameId = Astra::TypeID<Name>::Value();

    Astra::Registry::Config cfg;
    cfg.AddUniversal<Name>();
    Astra::Registry reg(creg, cfg);
    for (int i = 0; i < 5; ++i) (void)reg.CreateEntity<Position>();
    ASSERT_NE(reg.GetComponentStorage()->FindUniversal(nameId), nullptr);

    {
        const auto live = Astra::Debug::Capture(reg);
        const auto* name = FindStore(live, nameId);
        ASSERT_NE(name, nullptr);
        EXPECT_EQ(name->kind, Astra::StorageKind::Universal);
        EXPECT_EQ(name->population, 5u);
    }

    mod.Reset();   // the plugin's Shutdown: the column is dropped, the type is suspended
    ASSERT_EQ(reg.GetComponentStorage()->FindUniversal(nameId), nullptr);

    for (int pass = 0; pass < 2; ++pass)
    {
        const auto snap = Astra::Debug::Capture(reg);
        // The descriptor is gone, so the id is not listed (nothing to name it by); the
        // snapshot must still be coherent and Capture must not have rebuilt the column.
        const auto* name = FindStore(snap, nameId);
        if (name)
        {
            EXPECT_EQ(name->kind, Astra::StorageKind::Universal);
            EXPECT_EQ(name->population, 0u);
            EXPECT_EQ(name->pageCount, 0u);
        }
        EXPECT_EQ(snap.registry.entityCount, 5u);
        EXPECT_EQ(reg.GetComponentStorage()->FindUniversal(nameId), nullptr);
    }

    // Registered again (anonymously): the declared Universal is reported with no column
    // until the next create retro-fills it.
    creg->RegisterComponents<Name>();
    const auto again = Astra::Debug::Capture(reg);
    const auto* name = FindStore(again, nameId);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->kind, Astra::StorageKind::Universal);
    EXPECT_EQ(name->population, 0u);
    EXPECT_EQ(name->pageCount, 0u);
    EXPECT_EQ(reg.GetComponentStorage()->FindUniversal(nameId), nullptr) << "Capture never rebuilds a suspended column";
}

TEST(InspectorStorage, AdvisoryFlagsToggledTableComponentsOnly)
{
    auto reg = MakeRegistry();
    Astra::Debug::ChurnTracker churn(*reg);
    std::vector<Astra::Entity> es(50);
    ASSERT_EQ(reg->CreateEntities<Position>(50, std::span{es}), 50u);
    for (int round = 0; round < 4; ++round)
        for (Astra::Entity e : es)
        {
            ASSERT_TRUE(reg->AddComponent(e, Health{}));        // Table, toggled: flagged
            ASSERT_TRUE(reg->RemoveComponent<Health>(e));
            ASSERT_TRUE(reg->AddComponent(e, ParityPosS{}));    // already Sparse: never flagged
            ASSERT_TRUE(reg->RemoveComponent<ParityPosS>(e));
        }
    for (Astra::Entity e : es) ASSERT_TRUE(reg->AddComponent(e, Velocity{}));   // added once, kept

    const auto snap = Astra::Debug::Capture(*reg, &churn);
    const auto* health = FindStore(snap, Astra::TypeID<Health>::Value());
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health->adds, 200u);
    EXPECT_EQ(health->removes, 200u);
    EXPECT_EQ(health->population, 0u);
    EXPECT_TRUE(health->looksSparseShaped);
    EXPECT_NE(health->advisory.find("toggled"), std::string::npos) << health->advisory;

    const auto* ps = FindStore(snap, Astra::TypeID<ParityPosS>::Value());
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->removes, 200u);
    EXPECT_FALSE(ps->looksSparseShaped);

    const auto* vel = FindStore(snap, Astra::TypeID<Velocity>::Value());
    ASSERT_NE(vel, nullptr);
    EXPECT_EQ(vel->adds, 50u);
    EXPECT_EQ(vel->removes, 0u);
    EXPECT_FALSE(vel->looksSparseShaped);
}

// 1000 entities over 5 archetypes; Physics on ONE entity of each -> 5 archetypes that
// differ from an existing archetype only by Physics: rare AND multiplying.
TEST(InspectorStorage, AdvisoryFlagsRareTableComponentsThatMultiplyArchetypes)
{
    auto reg = MakeRegistry();
    std::vector<Astra::Entity> holders;
    for (int i = 0; i < 200; ++i)
    {
        Astra::Entity a = reg->CreateEntity<Position>();
        Astra::Entity b = reg->CreateEntity<Position, Velocity>();
        Astra::Entity c = reg->CreateEntity<Position, Health>();
        Astra::Entity d = reg->CreateEntity<Position, Velocity, Health>();
        Astra::Entity e = reg->CreateEntity<Position, Transform>();
        if (i == 0) holders = {a, b, c, d, e};
    }
    for (Astra::Entity h : holders) ASSERT_TRUE(reg->AddComponent(h, Physics{}));

    const auto snap = Astra::Debug::Capture(*reg);
    const auto* physics = FindStore(snap, Astra::TypeID<Physics>::Value());
    ASSERT_NE(physics, nullptr);
    EXPECT_EQ(physics->population, 5u);
    EXPECT_EQ(physics->archetypeCount, 5u);
    EXPECT_EQ(physics->pairedArchetypeCount, 5u);
    EXPECT_TRUE(physics->looksSparseShaped);
    EXPECT_NE(physics->advisory.find("rare"), std::string::npos) << physics->advisory;

    const auto* pos = FindStore(snap, Astra::TypeID<Position>::Value());
    ASSERT_NE(pos, nullptr);
    EXPECT_FALSE(pos->looksSparseShaped);        // on every entity
    const auto* vel = FindStore(snap, Astra::TypeID<Velocity>::Value());
    ASSERT_NE(vel, nullptr);
    EXPECT_FALSE(vel->looksSparseShaped);        // 400 of 1000: not rare
}

TEST(InspectorStorage, ChurnTrackerRestoresTheSignalMask)
{
    Astra::Registry reg;
    const Astra::Signal before = reg.GetEnabledSignals();
    {
        Astra::Debug::ChurnTracker churn(reg);
        EXPECT_NE(reg.GetEnabledSignals(), before);
    }
    EXPECT_EQ(reg.GetEnabledSignals(), before);
}

// Final review M5: the tracker turns off only the bits IT turned on. Signals the host
// enabled while it was attached stay on, and one the host had on before stays on.
TEST(InspectorStorage, ChurnTrackerDisablesOnlyTheSignalsItEnabled)
{
    using Astra::Signal;
    const auto on = [](Signal mask, Signal s) { return (mask & s) != Signal::None; };
    Astra::Registry reg;
    reg.EnableSignals(Signal::ComponentRemoved);                // the host's, before the tracker
    {
        Astra::Debug::ChurnTracker churn(reg);
        reg.EnableSignals(Signal::EntityCreated | Signal::ComponentEnabled);   // the host's, while attached
    }
    const Signal after = reg.GetEnabledSignals();
    EXPECT_TRUE(on(after, Signal::EntityCreated)) << "enabled by the host while the tracker was attached";
    EXPECT_TRUE(on(after, Signal::ComponentEnabled));
    EXPECT_TRUE(on(after, Signal::ComponentRemoved)) << "the host had it on before the tracker";
    EXPECT_FALSE(on(after, Signal::ComponentAdded)) << "the tracker's own bit is turned off again";
}
