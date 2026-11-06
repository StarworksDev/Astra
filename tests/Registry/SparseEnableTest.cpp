#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <utility>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

namespace
{
    Astra::Registry::Config UniversalUnE()
    {
        Astra::Registry::Config cfg;
        cfg.AddUniversal<UnE>();
        return cfg;
    }
}

TEST(SparseEnable, SparseToggleUpdatesTheStoreAndTheMirror)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpE>(e, SpE{1.0f});
    EXPECT_TRUE(reg.IsEnabled<SpE>(e));
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, false));
    EXPECT_FALSE(reg.IsEnabled<SpE>(e));
    EXPECT_TRUE(reg.HasComponent<SpE>(e));       // existence never lies
    EXPECT_TRUE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, true));
    EXPECT_TRUE(reg.IsEnabled<SpE>(e));
    EXPECT_FALSE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, SignalsFireOnGenuineChangeOnly)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentEnabled | Astra::Signal::ComponentDisabled);
    int disabled = 0, enabled = 0;
    void* payload = nullptr;
    auto hd = reg.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const Astra::Events::ComponentDisabled& ev) { ++disabled; payload = ev.component; });
    auto he = reg.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const Astra::Events::ComponentEnabled&) { ++enabled; });
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, false));
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, false));    // idempotent: true, silent
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, true));
    EXPECT_TRUE(reg.SetEnabled<SpE>(e, true));
    EXPECT_EQ(disabled, 1);
    EXPECT_EQ(enabled, 1);
    EXPECT_EQ(payload, static_cast<void*>(reg.GetComponent<SpE>(e)));
    reg.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Unregister(hd);
    reg.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Unregister(he);
}

TEST(SparseEnable, AbsentInvalidOrNonEnableableIsRefused)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.GetComponentRegistry()->RegisterComponent<SpE>();
    EXPECT_FALSE(reg.SetEnabled<SpE>(e, false));                 // absent
    EXPECT_FALSE(reg.IsEnabled<SpE>(e));
    EXPECT_FALSE(reg.SetEnabled<SpE>(Astra::Entity::Invalid(), false));
    // G2: the refusals created no set and took no slot.
    EXPECT_EQ(reg.GetComponentStorage()->FindSparse(Astra::TypeID<SpE>::Value()), nullptr);
    EXPECT_EQ(reg.GetComponentStorage()->UsedSparseSlotCount(), 0u);
    reg.AddComponent<SpA>(e, SpA{});
    const Astra::ComponentID a = Astra::TypeID<SpA>::Value();
    EXPECT_FALSE(reg.SetEnabledByID(e, a, false));               // present, not enableable
    EXPECT_TRUE(reg.IsEnabledByID(e, a));                        // present non-enableable: enabled
    ExpectInvariants(reg);
}

TEST(SparseEnable, SparseTagIsEnableable)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpTag>(e, SpTag{});
    EXPECT_TRUE(reg.SetEnabled<SpTag>(e, false));
    EXPECT_FALSE(reg.IsEnabled<SpTag>(e));
    EXPECT_TRUE(DisabledBit<SpTag>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, UniversalToggleUsesAMirrorBlockWithoutPresentWords)
{
    Astra::Registry reg(UniversalUnE());
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    ASSERT_TRUE(reg.HasComponent<UnE>(e));
    EXPECT_TRUE(reg.IsEnabled<UnE>(e));
    EXPECT_TRUE(reg.SetEnabled<UnE>(e, false));
    EXPECT_FALSE(reg.IsEnabled<UnE>(e));
    ASSERT_GE(SlotOf<UnE>(reg), 0);
    EXPECT_TRUE(DisabledBit<UnE>(reg, e));
    Astra::ArchetypeChunk* chunk = Locate(reg, e).first;
    ASSERT_NE(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<UnE>(reg))), nullptr);
    EXPECT_EQ(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<UnE>(reg)))->present, nullptr);
    ExpectInvariants(reg);
    EXPECT_TRUE(reg.SetEnabled<UnE>(e, true));
    EXPECT_FALSE(DisabledBit<UnE>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, DisabledStateSurvivesTableMovesAndSwapRemoves)
{
    Astra::Registry reg(UniversalUnE());
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    const Astra::Entity c = reg.CreateEntityWith(Position{});
    for (Astra::Entity e : {a, b, c}) reg.AddComponent<SpE>(e, SpE{});
    reg.SetEnabled<SpE>(a, false);
    reg.SetEnabled<SpE>(c, false);           // the tail
    reg.SetEnabled<UnE>(c, false);

    reg.AddComponent<Velocity>(a, Velocity{});   // a leaves; c swaps into slot 0
    EXPECT_FALSE(reg.IsEnabled<SpE>(a));
    EXPECT_TRUE(reg.IsEnabled<SpE>(b));
    EXPECT_FALSE(reg.IsEnabled<SpE>(c));
    EXPECT_FALSE(reg.IsEnabled<UnE>(c));
    EXPECT_TRUE(DisabledBit<UnE>(reg, c));
    ExpectInvariants(reg);

    reg.RemoveComponent<Velocity>(a);
    EXPECT_FALSE(reg.IsEnabled<SpE>(a));
    ExpectInvariants(reg);
}

TEST(SparseEnable, CommandBufferSetEnabledRoutes)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    Astra::CommandBuffer cmd(&reg);
    cmd.SetEnabled<SpE>(e, false);
    cmd.Execute();
    EXPECT_FALSE(reg.IsEnabled<SpE>(e));
    EXPECT_TRUE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseEnable, RemovingADisabledMemberClearsTheMirrorAndReAddIsBornEnabled)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpE>(e, SpE{});
    reg.SetEnabled<SpE>(e, false);
    reg.RemoveComponent<SpE>(e);
    EXPECT_FALSE(DisabledBit<SpE>(reg, e));
    ExpectInvariants(reg);
    reg.AddComponent<SpE>(e, SpE{});
    EXPECT_TRUE(reg.IsEnabled<SpE>(e));
    ExpectInvariants(reg);
}

// ---- Review Focus 1 (Sparse), registry level: Add / Remove / SetEnabled ----
// A handle whose id was recycled at version+1 is refused and never touches the
// new occupant's element or its disabled mirror.
TEST(SparseEnable, StaleHandleIsRefusedOnAddRemoveAndSetEnabled)
{
    Astra::Registry reg;
    const Astra::Entity stale = reg.CreateEntity();
    reg.AddComponent<SpE>(stale, SpE{1.0f});
    reg.DestroyEntity(stale);
    const Astra::Entity fresh = reg.CreateEntity();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());
    reg.AddComponent<SpE>(fresh, SpE{2.0f});

    EXPECT_FALSE(reg.SetEnabled<SpE>(stale, false));
    EXPECT_FALSE(reg.IsEnabled<SpE>(stale));
    EXPECT_FALSE(reg.RemoveComponent<SpE>(stale));
    EXPECT_FALSE(reg.AddComponent<SpE>(stale, SpE{9.0f}));
    ASSERT_TRUE(reg.HasComponent<SpE>(fresh));
    EXPECT_TRUE(reg.IsEnabled<SpE>(fresh));
    EXPECT_FALSE(DisabledBit<SpE>(reg, fresh));
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<SpE>(fresh)->v, 2.0f);
    ExpectInvariants(reg);
}

// RULINGS "Task 19 (from Task 17 review)": a disabled Sparse member and a disabled
// enableable-Universal mirror survive (a) a swap-remove of another entity in the same
// chunk, (b) an archetype move of the holder, (c) Defragment.
TEST(SparseEnable, DisabledMirrorsSurviveSwapRemoveArchetypeMoveAndDefragment)
{
    Astra::Registry reg(UniversalUnE());
    // Filler first: the holders land near the tail of a LATER chunk, so the Defragment
    // step below really compacts them into an earlier one (the CompactChunks carry).
    std::vector<Astra::Entity> filler(3000);
    reg.CreateEntities<Position>(3000, filler);
    constexpr int N = 8;
    Astra::Entity es[N];
    for (int i = 0; i < N; ++i)
    {
        es[i] = reg.CreateEntityWith(Position{});
        reg.AddComponent<SpE>(es[i], SpE{static_cast<float>(i)});
    }
    const Astra::Entity target = es[N - 1];                 // the tail
    ASSERT_TRUE(reg.SetEnabled<SpE>(target, false));
    ASSERT_TRUE(reg.SetEnabled<UnE>(target, false));
    ExpectInvariants(reg);

    auto expectTargetDisabled = [&]
    {
        EXPECT_FALSE(reg.IsEnabled<SpE>(target));
        EXPECT_FALSE(reg.IsEnabled<UnE>(target));
        EXPECT_TRUE(DisabledBit<SpE>(reg, target));
        EXPECT_TRUE(DisabledBit<UnE>(reg, target));
        ExpectInvariants(reg);
    };

    // (a) swap-remove of another entity in the same chunk (the tail moves into slot 1)
    reg.DestroyEntity(es[1]);
    expectTargetDisabled();
    EXPECT_TRUE(reg.IsEnabled<SpE>(es[0]));

    // (b) archetype move of the holder, both directions
    reg.AddComponent<Velocity>(target, Velocity{});
    expectTargetDisabled();
    reg.RemoveComponent<Velocity>(target);
    expectTargetDisabled();

    // (c) Defragment, after thinning the chunk
    reg.DestroyEntity(es[2]);
    reg.DestroyEntity(es[3]);
    reg.DestroyEntity(es[4]);
    for (size_t i = 0; i < filler.size(); i += 2)
        reg.DestroyEntity(filler[i]);                       // under-fill every chunk
    ASSERT_GT(reg.FindArchetype<Position>()->GetChunkCount(), 1u);
    Astra::Registry::DefragmentationOptions opts;
    opts.minArchetypesToKeep = 0;
    opts.chunkUtilizationThreshold = 1.0f;                  // compact every multi-chunk archetype
    const auto result = reg.Defragment(opts);
    EXPECT_GT(result.entitiesMoved, 0u);
    expectTargetDisabled();
    EXPECT_TRUE(reg.IsEnabled<SpE>(es[0]));
    EXPECT_FALSE(DisabledBit<SpE>(reg, es[0]));
}
