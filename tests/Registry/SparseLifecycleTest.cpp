#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

namespace
{
    template<typename T>
    size_t SetSize(Astra::Registry& reg)
    {
        Astra::SparseSet* set = reg.GetComponentStorage()->Sparse(Astra::TypeID<T>::Value());
        return set ? set->Size() : 0u;
    }
}

TEST(SparseLifecycle, DestroyErasesEveryMembership)
{
    Astra::Registry reg;
    const Astra::Entity keep = reg.CreateEntityWith(Position{});
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(keep, SpA{1.0f});
    reg.AddComponent<SpA>(e, SpA{2.0f});
    reg.AddComponent<SpB>(e, SpB{3});
    const uint32_t counter = reg.GetArchetypeManager()->GetSparseStructuralCounter();

    reg.DestroyEntity(e);

    EXPECT_EQ(SetSize<SpA>(reg), 1u);
    EXPECT_EQ(SetSize<SpB>(reg), 0u);
    EXPECT_NE(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);
    ASSERT_NE(reg.GetComponent<SpA>(keep), nullptr);
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(keep)->v, 1.0f);
    ExpectInvariants(reg);

    const Astra::Entity reused = reg.CreateEntityWith(Position{});   // may recycle e's id at version + 1
    EXPECT_FALSE(reg.HasComponent<SpA>(reused));
    EXPECT_FALSE(reg.HasComponent<SpB>(reused));
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DestroySwapRemoveKeepsTheTailsMembership)
{
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntityWith(Position{});
    const Astra::Entity e1 = reg.CreateEntityWith(Position{});
    const Astra::Entity e2 = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e0, SpA{});
    reg.AddComponent<SpB>(e2, SpB{9});
    reg.DestroyEntity(e0);                      // e2 swaps into slot 0
    ASSERT_EQ(Locate(reg, e2).second, 0u);
    EXPECT_TRUE(PresentBit<SpB>(reg, e2));
    EXPECT_FALSE(PresentBit<SpA>(reg, e2));
    EXPECT_FALSE(reg.HasComponent<SpA>(e1));
    ASSERT_NE(reg.GetComponent<SpB>(e2), nullptr);
    EXPECT_EQ(reg.GetComponent<SpB>(e2)->v, 9);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, EntityDestroyedHandlersStillSeeSparseComponents)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::EntityDestroyed);
    bool had = false;
    float value = 0.0f;
    auto h = reg.GetSignalManager()->On<Astra::Events::EntityDestroyed>().Register(
        [&](const Astra::Events::EntityDestroyed& ev)
        {
            had = reg.HasComponent<SpA>(ev.entity);
            if (had) value = std::as_const(reg).GetComponent<SpA>(ev.entity)->v;
        });
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{6.0f});
    reg.DestroyEntity(e);
    EXPECT_TRUE(had);
    EXPECT_FLOAT_EQ(value, 6.0f);
    reg.GetSignalManager()->On<Astra::Events::EntityDestroyed>().Unregister(h);
}

TEST(SparseLifecycle, DestroyEntitiesBatchWithDuplicates)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(200);
    reg.CreateEntities<Position>(200, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 2 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 3 == 0) reg.AddComponent<SpE>(es[i], SpE{});
        if (i % 6 == 0) reg.SetEnabled<SpE>(es[i], false);
    }
    std::vector<Astra::Entity> doomed;
    for (size_t i = 1; i < es.size(); i += 4) doomed.push_back(es[i]);
    for (size_t i = 0; i < es.size(); i += 8) doomed.push_back(es[i]);
    doomed.push_back(es[0]);                                   // a duplicate
    reg.DestroyEntities(std::span<Astra::Entity>(doomed.data(), doomed.size()));

    size_t expectA = 0;
    for (size_t i = 0; i < es.size(); ++i)
    {
        const bool dead = (i % 4 == 1) || (i % 8 == 0);
        if (dead) { EXPECT_FALSE(reg.IsValid(es[i])); continue; }
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 2 == 0) << i;
        expectA += (i % 2 == 0) ? 1u : 0u;
        if (i % 3 == 0) EXPECT_EQ(reg.IsEnabled<SpE>(es[i]), i % 6 != 0) << i;
    }
    EXPECT_EQ(SetSize<SpA>(reg), expectA);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, CommandBufferDestroyRoutes)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{});
    Astra::CommandBuffer cmd(&reg);
    cmd.DestroyEntity(e);
    cmd.Execute();
    EXPECT_EQ(SetSize<SpA>(reg), 0u);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, ClearDestroysElementsAndKeepsSlots)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(10);
    reg.CreateEntities<Position>(10, es);
    for (Astra::Entity e : es) reg.AddComponent<SpStr>(e, SpStr{std::string("v")});
    const int slot = SlotOf<SpStr>(reg);
    ASSERT_GE(slot, 0);

    reg.Clear();

    EXPECT_EQ(SetSize<SpStr>(reg), 0u);
    EXPECT_EQ(SlotOf<SpStr>(reg), slot);                     // slot assignment survives Clear
    for (Astra::Entity e : es) EXPECT_FALSE(reg.HasComponent<SpStr>(e));   // stale handles miss
    const Astra::Entity fresh = reg.CreateEntityWith(Position{});
    EXPECT_FALSE(reg.HasComponent<SpStr>(fresh));            // recycled id, next version
    reg.AddComponent<SpStr>(fresh, SpStr{std::string("w")});
    ASSERT_NE(reg.GetComponent<SpStr>(fresh), nullptr);
    EXPECT_EQ(reg.GetComponent<SpStr>(fresh)->s, "w");
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, UniversalDestroyAndRecycleIsBornEnabled)
{
    Astra::Registry::Config cfg;
    cfg.AddUniversal<UnE>();
    Astra::Registry reg(cfg);
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    reg.SetEnabled<UnE>(a, false);
    reg.SetEnabled<UnE>(b, false);
    reg.DestroyEntity(a);                       // b swaps into a's slot, disabled mirror travels
    EXPECT_FALSE(reg.IsEnabled<UnE>(b));
    ExpectInvariants(reg);
    const Astra::Entity c = reg.CreateEntityWith(Position{});   // may reuse a's id
    EXPECT_TRUE(reg.IsEnabled<UnE>(c));
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DefragmentAfterDestroysKeepsPresenceAndDisabledState)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(3000);
    reg.CreateEntities<Position>(3000, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 3 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 2 == 0) reg.AddComponent<SpE>(es[i], SpE{});
        if (i % 4 == 0) reg.SetEnabled<SpE>(es[i], false);
    }
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < es.size(); ++i)
        if (i % 5 != 0) doomed.push_back(es[i]);
    reg.DestroyEntities(std::span<Astra::Entity>(doomed.data(), doomed.size()));

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;
    reg.Defragment(opts);

    size_t expectA = 0;
    for (size_t i = 0; i < es.size(); i += 5)
    {
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 3 == 0) << i;
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), i % 3 == 0) << i;
        if (i % 3 == 0)
        {
            ASSERT_NE(reg.GetComponent<SpA>(es[i]), nullptr) << i;
            EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(es[i])->v, float(i)) << i;
        }
        if (i % 2 == 0) EXPECT_EQ(reg.IsEnabled<SpE>(es[i]), i % 4 != 0) << i;
        expectA += (i % 3 == 0) ? 1u : 0u;
    }
    EXPECT_EQ(SetSize<SpA>(reg), expectA);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DestroyingABystanderDoesNotBumpTheSparseCounter)
{
    Astra::Registry reg;
    const Astra::Entity holder = reg.CreateEntityWith(Position{});
    const Astra::Entity bystander = reg.CreateEntityWith(Position{});
    const Astra::Entity bystander2 = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(holder, SpA{1.0f});            // the shared chunk now has a presence block
    ASSERT_TRUE(Locate(reg, holder).first != nullptr);
    ASSERT_EQ(Locate(reg, holder).first, Locate(reg, bystander).first);
    ASSERT_TRUE(Locate(reg, bystander).first->HasSparseBlocks());  
    const uint32_t counter = reg.GetArchetypeManager()->GetSparseStructuralCounter();

    reg.DestroyEntity(bystander);
    EXPECT_EQ(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);

    const Astra::Entity batch[] = { bystander2 };
    reg.DestroyEntities(std::span<Astra::Entity>(const_cast<Astra::Entity*>(batch), 1));
    EXPECT_EQ(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);

    reg.DestroyEntity(holder);                           // a holder DOES bump
    EXPECT_NE(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DestroyingABystanderBesideAUniversalMirrorBlockDoesNotBump)
{
    Astra::Registry::Config cfg;
    cfg.AddUniversal<UnE>();
    Astra::Registry reg(cfg);
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    reg.SetEnabled<UnE>(a, false);                       // mirror-only block (present == nullptr)
    ASSERT_TRUE(Locate(reg, b).first != nullptr);
    ASSERT_TRUE(Locate(reg, b).first->HasSparseBlocks());
    const uint32_t counter = reg.GetArchetypeManager()->GetSparseStructuralCounter();
    reg.DestroyEntity(b);
    EXPECT_EQ(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);
    reg.DestroyEntity(a);
    EXPECT_EQ(reg.GetArchetypeManager()->GetSparseStructuralCounter(), counter);
    ExpectInvariants(reg);
}

TEST(SparseLifecycle, DefragmentReleasesBlocksCompactionEmptied)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(3000);
    reg.CreateEntities<Position>(3000, es);
    for (size_t i = 0; i < es.size(); i += 2) reg.AddComponent<SpA>(es[i], SpA{});
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < es.size(); i += 2) doomed.push_back(es[i]);   // every SpA holder dies
    reg.DestroyEntities(std::span<Astra::Entity>(doomed.data(), doomed.size()));

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;
    reg.Defragment(opts);

    for (Astra::Archetype* arch : reg.GetArchetypeManager()->GetArchetypes())
        for (const auto& chunk : arch->GetChunks())
            for (const Astra::ArchetypeChunk::SparseBlock& blk : chunk->GetSparseBlocks())
                EXPECT_TRUE(blk.presentCount != 0 || blk.disabledCount != 0)
                    << "an empty block survived compaction (slot " << blk.slot << ")";
    ExpectInvariants(reg);
}
