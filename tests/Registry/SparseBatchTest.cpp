#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <string>
#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

TEST(SparseBatch, AddComponentByIDRoutesToTheSet)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpA>();
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int fired = 0;
    void* addedPtr = nullptr;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { ++fired; addedPtr = ev.component; });

    const Astra::Entity e = reg.CreateEntityWith(Position{});
    const Astra::Archetype* arch = reg.GetArchetypeManager()->GetEntityRecord(e)->archetype;
    const SpA value{7.0f};
    const Astra::ComponentID id = Astra::TypeID<SpA>::Value();
    fired = 0;   // CreateEntityWith(Position) already signalled once
    EXPECT_TRUE(reg.AddComponentByID(e, id, &value, sizeof(SpA)));
    EXPECT_FALSE(reg.AddComponentByID(e, id, &value, sizeof(SpA)));   // by-id: present -> false (Table parity)
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, arch);
    ASSERT_TRUE(reg.HasComponent<SpA>(e));
    ASSERT_NE(reg.GetComponent<SpA>(e), nullptr);
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 7.0f);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(addedPtr, static_cast<void*>(reg.GetComponent<SpA>(e)));
    EXPECT_FALSE(reg.AddComponentByID(reg.CreateEntity(), id, &value, 3));   // size mismatch refused
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseBatch, ByIdCopiesANonTrivialValue)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpStr>();
    const Astra::Entity e = reg.CreateEntity();
    const SpStr source{std::string("by-id")};
    ASSERT_TRUE(reg.AddComponentByID(e, Astra::TypeID<SpStr>::Value(), &source, sizeof(SpStr)));
    ASSERT_NE(reg.GetComponent<SpStr>(e), nullptr);
    EXPECT_EQ(reg.GetComponent<SpStr>(e)->s, "by-id");
    EXPECT_EQ(source.s, "by-id");   // copied, not moved from
}

TEST(SparseBatch, RemoveComponentByIDSignalsBeforeRemoval)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);
    bool presentInHandler = false;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev) { presentInHandler = reg.HasComponent<SpA>(ev.entity); });
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{});
    EXPECT_TRUE(reg.RemoveComponentByID(e, Astra::TypeID<SpA>::Value()));
    EXPECT_TRUE(presentInHandler);
    EXPECT_FALSE(reg.HasComponent<SpA>(e));
    EXPECT_FALSE(reg.RemoveComponentByID(e, Astra::TypeID<SpA>::Value()));
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(h);
}

TEST(SparseBatch, RemoveMissPathsCreateNoStore)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpB>();
    const Astra::Entity e = reg.CreateEntity();
    const Astra::ComponentID id = Astra::TypeID<SpB>::Value();
    EXPECT_FALSE(reg.RemoveComponentByID(e, id));
    std::vector<Astra::Entity> es{e};
    EXPECT_EQ(reg.RemoveComponentsByID(std::span<Astra::Entity>(es.data(), es.size()), id), 0u);
    EXPECT_EQ(reg.RemoveComponents<SpB>(std::span<Astra::Entity>(es.data(), es.size())), 0u);
    EXPECT_EQ(reg.GetComponentStorage()->FindSparse(id), nullptr);   // G2: a miss never creates a set
}

TEST(SparseBatch, ByIdTagUsesTheEmptySentinel)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpTag>();
    reg.EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    void* addedPtr = nullptr;
    void* removedPtr = nullptr;
    auto ha = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    auto hr = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev) { removedPtr = ev.component; });
    const Astra::Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.AddComponentByID(e, Astra::TypeID<SpTag>::Value(), nullptr, 0));
    EXPECT_EQ(addedPtr, Astra::EmptyComponentSentinel());
    EXPECT_TRUE(PresentBit<SpTag>(reg, e));   // routed to the set, not into an archetype mask
    ASSERT_TRUE(reg.RemoveComponentByID(e, Astra::TypeID<SpTag>::Value()));
    EXPECT_EQ(removedPtr, Astra::EmptyComponentSentinel());
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(ha);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(hr);
}

TEST(SparseBatch, ByIdBatchesCountSuccesses)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpB>();
    std::vector<Astra::Entity> es(10);
    reg.CreateEntities<Position>(10, es);
    for (size_t i = 0; i < 3; ++i) reg.AddComponent<SpB>(es[i], SpB{});
    const SpB v{4};
    const Astra::ComponentID id = Astra::TypeID<SpB>::Value();
    EXPECT_EQ(reg.AddComponentsByID(std::span<Astra::Entity>(es.data(), es.size()), id, &v, sizeof(SpB)), 7u);
    EXPECT_EQ(reg.RemoveComponentsByID(std::span<Astra::Entity>(es.data(), es.size()), id), 10u);
    for (Astra::Entity e : es) EXPECT_FALSE(reg.HasComponent<SpB>(e));
    ExpectInvariants(reg);
}

TEST(SparseBatch, TypedBatchAddFollowsTheTableSignalRule)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int fired = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded&) { ++fired; });
    std::vector<Astra::Entity> es(4);
    reg.CreateEntities<Position>(4, es);
    reg.AddComponent<SpA>(es[0], SpA{1.0f});
    fired = 0;

    std::vector<Astra::Entity> batch{es[0], es[1], es[2], es[3], es[1]};   // a duplicate
    reg.AddComponents<SpA>(std::span<Astra::Entity>(batch.data(), batch.size()), SpA{2.0f});

    // Table parity (Registry::AddComponents): after the pass, ComponentAdded fires
    // once per de-duplicated entity that HOLDS T -- the pre-existing holder included.
    EXPECT_EQ(fired, 4);
    ASSERT_NE(reg.GetComponent<SpA>(es[0]), nullptr);
    ASSERT_NE(reg.GetComponent<SpA>(es[3]), nullptr);
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(es[0])->v, 1.0f);   // present: untouched
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(es[3])->v, 2.0f);
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseBatch, TypedEmplaceAndRemoveBatches)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(64);
    reg.CreateEntities<Position>(64, es);
    reg.EmplaceComponents<SpStr>(std::span<Astra::Entity>(es.data(), es.size()), std::string("x"));
    for (Astra::Entity e : es)
    {
        ASSERT_NE(reg.GetComponent<SpStr>(e), nullptr);
        EXPECT_EQ(reg.GetComponent<SpStr>(e)->s, "x");
    }

    std::vector<Astra::Entity> evens;
    for (size_t i = 0; i < es.size(); i += 2) evens.push_back(es[i]);
    EXPECT_EQ(reg.RemoveComponents<SpStr>(std::span<Astra::Entity>(evens.data(), evens.size())), evens.size());
    for (size_t i = 0; i < es.size(); ++i) EXPECT_EQ(reg.HasComponent<SpStr>(es[i]), i % 2 == 1) << i;
    ExpectInvariants(reg);
}

TEST(SparseBatch, CommandBufferMixedTableAndSparseMovesOnlyForTheTablePart)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<SpA>();
    reg.GetComponentRegistry()->RegisterComponent<Velocity>();
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    const size_t archetypes = reg.GetArchetypeCount();

    Astra::CommandBuffer cmd(&reg);
    cmd.AddComponent(e, SpA{3.0f});
    cmd.AddComponent(e, Velocity{1, 1, 1});
    cmd.Execute();

    EXPECT_EQ(reg.GetArchetypeCount(), archetypes + 1);   // only {Position, Velocity} appeared
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, (reg.FindArchetype<Position, Velocity>()));
    ASSERT_NE(reg.GetComponent<SpA>(e), nullptr);
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 3.0f);
    ExpectInvariants(reg);

    Astra::CommandBuffer cmd2(&reg);
    cmd2.RemoveComponent<SpA>(e);
    cmd2.Execute();
    EXPECT_FALSE(reg.HasComponent<SpA>(e));
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, (reg.FindArchetype<Position, Velocity>()));
    ExpectInvariants(reg);
}

TEST(SparseBatch, MixedCreationPlacesTheTableSubsetOnce)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{}, SpA{1.0f}, Velocity{}, SpB{2});
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, (reg.FindArchetype<Position, Velocity>()));
    EXPECT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_TRUE(reg.HasComponent<SpB>(e));
    ExpectInvariants(reg);
}
