#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <vector>

#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::SparseC;

TEST(SparseStructure, AddSparseNeverMovesTheEntity)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{1, 2, 3});
    const Astra::Archetype* before = reg.GetArchetypeManager()->GetEntityRecord(e)->archetype;
    const size_t archetypes = reg.GetArchetypeCount();
    const uint32_t structural = reg.GetArchetypeManager()->GetStructuralChangeCounter();

    EXPECT_TRUE(reg.AddComponent<SpA>(e, SpA{2.5f}));

    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, before);
    EXPECT_EQ(reg.GetArchetypeCount(), archetypes);
    EXPECT_EQ(reg.GetArchetypeManager()->GetStructuralChangeCounter(), structural);
    ASSERT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 2.5f);
    EXPECT_TRUE(PresentBit<SpA>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseStructure, RemoveSparseClearsMembershipAndBit)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e, SpA{1.0f});
    EXPECT_TRUE(reg.RemoveComponent<SpA>(e));
    EXPECT_FALSE(reg.HasComponent<SpA>(e));
    EXPECT_FALSE(PresentBit<SpA>(reg, e));
    EXPECT_FALSE(reg.RemoveComponent<SpA>(e));   // absent -> false (Table parity)
    EXPECT_FALSE(reg.RemoveComponent<SpB>(e));   // never held -> false
    // G2: a miss never creates a store or consumes a sparse slot.
    EXPECT_EQ(reg.GetComponentStorage()->FindSparse(Astra::TypeID<SpB>::Value()), nullptr);
    EXPECT_LT(SlotOf<SpB>(reg), 0);
    EXPECT_EQ(reg.GetComponentStorage()->UsedSparseSlotCount(), size_t(1));   // SpA only
    ExpectInvariants(reg);
}

TEST(SparseStructure, AddWhenPresentIsANoOpReturningTrue)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    int added = 0;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded&) { ++added; });

    const Astra::Entity e = reg.CreateEntity();
    EXPECT_TRUE(reg.AddComponent<SpA>(e, SpA{1.0f}));
    EXPECT_TRUE(reg.AddComponent<SpA>(e, SpA{9.0f}));     // present: no-op
    EXPECT_FLOAT_EQ(reg.GetComponent<SpA>(e)->v, 1.0f);   // value untouched
    EXPECT_EQ(added, 1);                                   // no second signal
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseStructure, SignalsCarryTheElementAndRemovedFiresBeforeRemoval)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    void* addedPtr = nullptr;
    bool presentInHandler = false;
    float valueInHandler = 0.0f;
    auto ha = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    auto hr = reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev)
        {
            presentInHandler = reg.HasComponent<SpA>(ev.entity);
            valueInHandler = static_cast<SpA*>(ev.component)->v;
        });

    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<SpA>(e, SpA{4.0f});
    EXPECT_EQ(addedPtr, static_cast<void*>(reg.GetComponent<SpA>(e)));
    reg.RemoveComponent<SpA>(e);
    EXPECT_TRUE(presentInHandler);           // emitted BEFORE the erase, as for Table
    EXPECT_FLOAT_EQ(valueInHandler, 4.0f);

    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(ha);
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Unregister(hr);
}

TEST(SparseStructure, EmplaceAndNonTrivialValues)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    EXPECT_TRUE(reg.EmplaceComponent<SpStr>(e, std::string("hello")));
    ASSERT_NE(reg.GetComponent<SpStr>(e), nullptr);
    EXPECT_EQ(reg.GetComponent<SpStr>(e)->s, "hello");
    EXPECT_TRUE(reg.RemoveComponent<SpStr>(e));
    ExpectInvariants(reg);
}

TEST(SparseStructure, SparseTagAddRemove)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    void* addedPtr = nullptr;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    const Astra::Entity e = reg.CreateEntity();
    EXPECT_TRUE(reg.AddComponent<SpTag>(e, SpTag{}));
    EXPECT_NE(addedPtr, nullptr);            // the per-type tag instance
    EXPECT_EQ(addedPtr, static_cast<void*>(reg.GetComponent<SpTag>(e)));   // one shared static (Table parity)
    EXPECT_TRUE(reg.HasComponent<SpTag>(e));
    EXPECT_TRUE(PresentBit<SpTag>(reg, e));
    EXPECT_TRUE(reg.RemoveComponent<SpTag>(e));
    EXPECT_FALSE(reg.HasComponent<SpTag>(e));
    ExpectInvariants(reg);
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseStructure, SparseAddAndRemoveBumpTheSparseCounterOnly)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity();
    const uint32_t s0 = reg.GetArchetypeManager()->GetSparseStructuralCounter();
    const uint32_t structural = reg.GetArchetypeManager()->GetStructuralChangeCounter();
    reg.AddComponent<SpA>(e, SpA{});
    const uint32_t s1 = reg.GetArchetypeManager()->GetSparseStructuralCounter();
    EXPECT_NE(s1, s0);
    reg.RemoveComponent<SpA>(e);
    EXPECT_NE(reg.GetArchetypeManager()->GetSparseStructuralCounter(), s1);
    EXPECT_EQ(reg.GetArchetypeManager()->GetStructuralChangeCounter(), structural);   // the "only"
}

TEST(SparseStructure, ManyEntitiesInOneChunkKeepP1)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es(64);
    reg.CreateEntities<Position>(64, es);
    for (size_t i = 0; i < es.size(); ++i)
    {
        if (i % 2 == 0) reg.AddComponent<SpA>(es[i], SpA{float(i)});
        if (i % 3 == 0) reg.AddComponent<SpB>(es[i], SpB{int(i)});
    }
    for (size_t i = 0; i < es.size(); i += 4) reg.RemoveComponent<SpA>(es[i]);
    ExpectInvariants(reg);
    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(reg.HasComponent<SpA>(es[i]), i % 2 == 0 && i % 4 != 0) << i;
        EXPECT_EQ(PresentBit<SpA>(reg, es[i]), reg.HasComponent<SpA>(es[i])) << i;
    }
}

TEST(SparseStructure, ValidatorReportsACorruptedPresenceBit)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e, SpA{});
    ASSERT_TRUE(reg.ValidateStorageInvariants().empty());
    auto [chunk, idx] = Locate(reg, e);
    chunk->SetSparsePresent(static_cast<uint16_t>(SlotOf<SpA>(reg)), idx, false);   // corrupt P1
    const std::string report = reg.ValidateStorageInvariants();
    EXPECT_NE(report.find("P1"), std::string::npos) << report;
}

TEST(SparseStructure, CreateWithASparseMemberSetsItsBit)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{}, SpA{3.0f});
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(e)->archetype, reg.FindArchetype<Position>());
    EXPECT_TRUE(reg.HasComponent<SpA>(e));
    EXPECT_TRUE(PresentBit<SpA>(reg, e));
    ExpectInvariants(reg);
}

TEST(SparseStructure, CreationPathTagPayloadEqualsGetComponent)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    void* addedPtr = nullptr;
    auto h = reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) { addedPtr = ev.component; });
    const Astra::Entity e = reg.CreateEntityWith(SpTag{});
    EXPECT_NE(addedPtr, nullptr);
    EXPECT_EQ(addedPtr, static_cast<void*>(reg.GetComponent<SpTag>(e)));
    reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Unregister(h);
}

TEST(SparseStructure, ValidatorIsSideEffectFree)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(e, SpA{});
    const size_t slots = reg.GetComponentStorage()->UsedSparseSlotCount();
    EXPECT_TRUE(reg.ValidateStorageInvariants().empty());
    EXPECT_EQ(reg.GetComponentStorage()->UsedSparseSlotCount(), slots);
}

TEST(SparseStructure, UniversalStorageValidatorCatchesADisabledFlagWithNoMirror)
{
    Astra::Registry::Config cfg;
    cfg.AddUniversal<UnE>();
    Astra::Registry reg(cfg);
    const Astra::Entity e = reg.CreateEntityWith(Position{});
    ASSERT_TRUE(reg.ValidateStorageInvariants().empty());
    Astra::UniversalColumn* col = reg.GetComponentStorage()->FindUniversal(Astra::TypeID<UnE>::Value());
    ASSERT_NE(col, nullptr);
    col->SetDisabled(e, true);   // corrupt P3: flag set, no slot and no mirror bit
    const std::string report = reg.ValidateStorageInvariants();
    EXPECT_NE(report.find("P3"), std::string::npos) << report;
}

TEST(SparseStructure, ValidatorReportsAStaleSparseMember)
{
    Astra::Registry reg;
    const Astra::Entity keep = reg.CreateEntityWith(Position{});
    reg.AddComponent<SpA>(keep, SpA{});
    const Astra::Entity dead = reg.CreateEntityWith(Position{});
    reg.DestroyEntity(dead);
    ASSERT_TRUE(reg.ValidateStorageInvariants().empty());
    Astra::SparseSet* set = reg.GetComponentStorage()->FindSparse(Astra::TypeID<SpA>::Value());
    ASSERT_NE(set, nullptr);
    set->Insert(dead);   // corrupt P1: a member whose entity is gone
    const std::string report = reg.ValidateStorageInvariants();
    EXPECT_NE(report.find("P1"), std::string::npos) << report;
}
