#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

// Review Focus 3 (pin): the swap-remove moves the TAIL entity into the vacated
// slot. The removed slot and the tail carry DIFFERENT sparse memberships, so a
// missing or partial carry leaves the wrong bits at the vacated slot.
TEST(SparseCarry, SwapRemoveCarriesTheTailsMembershipIntoTheVacatedSlot)
{
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntityWith(Position{});
    const Astra::Entity e1 = reg.CreateEntityWith(Position{});
    const Astra::Entity e2 = reg.CreateEntityWith(Position{});
    ASSERT_EQ(Locate(reg, e0).second, 0u);
    ASSERT_EQ(Locate(reg, e2).second, 2u);
    reg.AddComponent<SpA>(e0, SpA{1.0f});   // removed slot: {SpA}
    reg.AddComponent<SpB>(e2, SpB{2});      // tail:         {SpB}

    reg.AddComponent<Velocity>(e0, Velocity{});   // e0 leaves; e2 swaps into slot 0

    auto [oldChunk, idx2] = Locate(reg, e2);
    ASSERT_EQ(idx2, 0u);
    EXPECT_TRUE(oldChunk->IsSparsePresent(static_cast<uint16_t>(SlotOf<SpB>(reg)), 0));
    EXPECT_FALSE(oldChunk->IsSparsePresent(static_cast<uint16_t>(SlotOf<SpA>(reg)), 0));
    EXPECT_FALSE(oldChunk->IsSparsePresent(static_cast<uint16_t>(SlotOf<SpB>(reg)), 2));   // tail cleared
    EXPECT_TRUE(PresentBit<SpA>(reg, e0));      // e0's membership travelled with it
    EXPECT_TRUE(reg.HasComponent<SpA>(e0));
    EXPECT_TRUE(reg.HasComponent<SpB>(e2));
    EXPECT_FALSE(reg.HasComponent<SpA>(e1));
    ExpectInvariants(reg);
}

// Review Focus 3 (pin): the removed slot IS the tail -- only the tail clear runs,
// dropping exactly the removed entity's bits and nobody else's.
TEST(SparseCarry, SwapRemoveOfTheTailDropsOnlyItsOwnBits)
{
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntityWith(Position{});
    const Astra::Entity e1 = reg.CreateEntityWith(Position{});
    const Astra::Entity e2 = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpB>(e1, SpB{1});
    reg.AddComponent<SpA>(e2, SpA{});
    reg.AddComponent<SpB>(e2, SpB{2});
    auto [chunk, idx] = Locate(reg, e2);
    ASSERT_EQ(idx, 2u);

    reg.AddComponent<Velocity>(e2, Velocity{});   // the tail leaves; nothing swaps

    EXPECT_EQ(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<SpA>(reg)))->presentCount, 0u);
    EXPECT_EQ(chunk->FindSparseBlock(static_cast<uint16_t>(SlotOf<SpB>(reg)))->presentCount, 1u);
    EXPECT_TRUE(PresentBit<SpB>(reg, e1));
    EXPECT_FALSE(PresentBit<SpA>(reg, e0));
    EXPECT_TRUE(PresentBit<SpA>(reg, e2));
    EXPECT_TRUE(PresentBit<SpB>(reg, e2));
    ExpectInvariants(reg);
}

TEST(SparseCarry, TableRemoveCarriesThroughMoveEntityFrom)
{
    Astra::Registry reg;
    const Astra::Entity a = reg.CreateEntityWith(Position{}, Velocity{});
    const Astra::Entity b = reg.CreateEntityWith(Position{}, Velocity{});
    reg.AddComponent<SpA>(a, SpA{5.0f});
    reg.AddComponent<SpB>(b, SpB{6});
    EXPECT_TRUE(reg.RemoveComponent<Velocity>(a));
    EXPECT_TRUE(PresentBit<SpA>(reg, a));
    const SpA* sa = reg.GetComponent<SpA>(a);
    ASSERT_NE(sa, nullptr);
    EXPECT_FLOAT_EQ(sa->v, 5.0f);
    EXPECT_TRUE(PresentBit<SpB>(reg, b));
    ExpectInvariants(reg);
}

TEST(SparseCarry, ByIdAddCarriesThroughMoveAndAddByID)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<Velocity>();
    const Astra::Entity a = reg.CreateEntityWith(Position{});
    const Astra::Entity b = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(a, SpA{});
    reg.AddComponent<SpB>(b, SpB{});
    const Velocity v{1, 2, 3};
    ASSERT_TRUE(reg.AddComponentByID(a, Astra::TypeID<Velocity>::Value(), &v, sizeof(Velocity)));
    EXPECT_TRUE(PresentBit<SpA>(reg, a));
    EXPECT_TRUE(PresentBit<SpB>(reg, b));   // b was swapped into a's old slot
    ExpectInvariants(reg);
}

// Batch move OUT OF THE ROOT archetype: the source and destination share no
// column (componentsToMove is empty), and presence must still follow the entity.
TEST(SparseCarry, BatchAddFromRootCarriesWithNoSharedColumn)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(300);
    reg.CreateEntities<>(300, es);
    for (size_t i = 0; i < es.size(); i += 2)
        reg.AddComponent<SpA>(es[i], SpA{float(i)});

    reg.AddComponents<Position>(std::span<Astra::Entity>(es.data(), es.size()), Position{});

    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), i % 2 == 0) << i;
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 2 == 0) << i;
    }
    ExpectInvariants(reg);
}

TEST(SparseCarry, BatchRemoveWithSharedColumnsCarries)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(500);
    reg.CreateEntities<Position, Velocity>(500, es);
    for (size_t i = 0; i < es.size(); i += 3)
        reg.AddComponent<SpB>(es[i], SpB{int(i)});

    // Remove Velocity from every second entity: batch moves interleaved with swap-removes.
    std::vector<Astra::Entity> half;
    for (size_t i = 0; i < es.size(); i += 2) half.push_back(es[i]);
    EXPECT_EQ(reg.RemoveComponents<Velocity>(std::span<Astra::Entity>(half.data(), half.size())), half.size());

    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(PresentBit<SpB>(reg, es[i]), i % 3 == 0) << i;
        if (i % 3 == 0)
        {
            const SpB* sb = reg.GetComponent<SpB>(es[i]);
            ASSERT_NE(sb, nullptr) << i;
            EXPECT_EQ(sb->v, int(i)) << i;
        }
    }
    ExpectInvariants(reg);
}

TEST(SparseCarry, DefragmentCompactionCarries)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(3000);
    reg.CreateEntities<Position>(3000, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 3 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 5 == 0) reg.AddComponent<SpB>(es[i], SpB{int(i)});
    }
    ASSERT_GT(reg.FindArchetype<Position>()->GetChunkCount(), 1u);

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;   // compact every multi-chunk archetype
    const auto result = reg.Defragment(opts);
    EXPECT_GT(result.entitiesMoved, 0u);

    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), i % 3 == 0) << i;
        EXPECT_EQ(PresentBit<SpB>(reg, es[i]), i % 5 == 0) << i;
    }
    ExpectInvariants(reg);
}
