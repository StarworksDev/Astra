// Storage-parity suite, Registry surface (spec 2026-10-01 s9.1, D2). One body per
// behaviour, instantiated over Table / Sparse / Universal variants of the same component.
#include <algorithm>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "StorageParity.hpp"

using namespace Astra::Test::Parity;
using Astra::Test::Health;
using Astra::Test::Velocity;

// ======================= Position-shaped variants =======================

template<class V> class StorageParityPos : public ::testing::Test {};
using PosVariants = ::testing::Types<TableV<Astra::Test::Position>,
                                     SparseV<ParityPosS>,
                                     UniversalV<Astra::Test::Position>>;
TYPED_TEST_SUITE(StorageParityPos, PosVariants, VariantNames);

// Sanity: the variant really is in force (otherwise every parity body is vacuous).
TYPED_TEST(StorageParityPos, VariantIsInForce)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    const size_t archetypesBefore = reg->GetArchetypeCount();
    if constexpr (V::kUniversal)
    {
        EXPECT_TRUE(reg->HasComponent<C>(e));                    // always present
        EXPECT_NE(reg->GetComponent<C>(e), nullptr);
        EXPECT_EQ(reg->GetArchetypeCount(), archetypesBefore);   // not in any archetype mask
    }
    else
    {
        EXPECT_FALSE(reg->HasComponent<C>(e));
        ASSERT_TRUE(reg->AddComponent(e, MakePos<C>(1.0f)));
        if constexpr (V::kSparse)
            EXPECT_EQ(reg->GetArchetypeCount(), archetypesBefore);       // never fragments
        else
            EXPECT_EQ(reg->GetArchetypeCount(), archetypesBefore + 1);   // {Velocity, C} is new
    }
}

TYPED_TEST(StorageParityPos, CreateWithValueThenGetAndHas)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{4.0f, 5.0f, 6.0f});
    ASSERT_TRUE(reg->IsValid(e));
    EXPECT_TRUE(reg->HasComponent<C>(e));
    C* c = reg->GetComponent<C>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, MakePos<C>(1.0f));          // Universal: a supplied value is never overwritten
    const Astra::Registry& cref = *reg;
    EXPECT_EQ(cref.GetComponent<C>(e), c);    // const and mutable access agree
    EXPECT_FLOAT_EQ(reg->GetComponent<Velocity>(e)->dx, 4.0f);
}

TYPED_TEST(StorageParityPos, AddAndEmplaceWhenPresentAreNoOpsReturningTrue)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    // Universal: present from creation, so even the first Add is the no-op (s3.3, s12.2).
    const C expected = V::kUniversal ? C{} : MakePos<C>(1.0f);

    EXPECT_TRUE(reg->AddComponent(e, MakePos<C>(1.0f)));
    EXPECT_TRUE(reg->HasComponent<C>(e));
    EXPECT_EQ(*reg->GetComponent<C>(e), expected);
    EXPECT_TRUE(reg->AddComponent(e, MakePos<C>(9.0f)));             // present: no-op, true
    EXPECT_EQ(*reg->GetComponent<C>(e), expected);
    EXPECT_TRUE(reg->EmplaceComponent<C>(e, 7.0f, 7.0f, 7.0f));    // present: no-op, true
    EXPECT_EQ(*reg->GetComponent<C>(e), expected);

    Astra::Entity f = reg->CreateEntity<Velocity>();
    EXPECT_TRUE(reg->EmplaceComponent<C>(f, 3.0f, 4.0f, 5.0f));
    EXPECT_EQ(*reg->GetComponent<C>(f), (V::kUniversal ? C{} : C{3.0f, 4.0f, 5.0f}));
}

TYPED_TEST(StorageParityPos, RemovePresentAbsentAndUniversalRefusal)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{});
    Astra::Entity keep = reg->CreateEntityWith(MakePos<C>(2.0f), Velocity{});
    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            EXPECT_FALSE(reg->RemoveComponent<C>(e));
            EXPECT_FALSE(reg->RemoveComponentByID(e, Astra::TypeID<C>::Value()));
        }
        if (kAssertsActive) EXPECT_GE(cap.count, 2);   // one refusal per call (Debug)
        else                EXPECT_EQ(cap.count, 0);
        EXPECT_TRUE(reg->HasComponent<C>(e));
        EXPECT_EQ(*reg->GetComponent<C>(e), MakePos<C>(1.0f));
    }
    else
    {
        EXPECT_TRUE(reg->RemoveComponent<C>(e));
        EXPECT_FALSE(reg->HasComponent<C>(e));
        EXPECT_EQ(reg->GetComponent<C>(e), nullptr);
        EXPECT_FALSE(reg->RemoveComponent<C>(e));                            // absent: false
        EXPECT_FALSE(reg->RemoveComponentByID(e, Astra::TypeID<C>::Value()));
        EXPECT_TRUE(reg->IsValid(e));
        EXPECT_NE(reg->GetComponent<Velocity>(e), nullptr);                  // other data untouched
    }
    EXPECT_EQ(*reg->GetComponent<C>(keep), MakePos<C>(2.0f));                // neighbour value intact
}

// Review Focus 1: a stale handle (destroy + recycle at version+1) must miss or be
// refused, and must never read or write the new occupant's element.
TYPED_TEST(StorageParityPos, StaleHandleAfterDestroyAndRecycleMisses)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity stale = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{});
    reg->DestroyEntity(stale);
    Astra::Entity occupant = RecycleIndex(*reg, stale, [](Astra::Registry& r) {
        return r.CreateEntityWith(MakePos<C>(5.0f), Velocity{});
    });
    ASSERT_TRUE(occupant.IsValid()) << "the destroyed index was never recycled";
    ASSERT_EQ(occupant.GetID(), stale.GetID());
    ASSERT_NE(occupant.GetVersion(), stale.GetVersion());

    EXPECT_FALSE(reg->HasComponent<C>(stale));
    EXPECT_EQ(reg->GetComponent<C>(stale), nullptr);
    EXPECT_FALSE(reg->HasComponentByHash(stale, Astra::TypeID<C>::Hash()));
    EXPECT_EQ(reg->GetComponentByHash(stale, Astra::TypeID<C>::Hash()), nullptr);
    EXPECT_FALSE(reg->AddComponent(stale, MakePos<C>(9.0f)));
    EXPECT_FALSE(reg->EmplaceComponent<C>(stale, 9.0f, 9.0f, 9.0f));
    {
        AssertCount cap;   // Universal: a remove is refused whatever the handle; never fatal here
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
        EXPECT_FALSE(reg->RemoveComponent<C>(stale));
        EXPECT_FALSE(reg->RemoveComponentByID(stale, Astra::TypeID<C>::Value()));
    }
    EXPECT_TRUE(reg->HasComponent<C>(occupant));
    EXPECT_EQ(*reg->GetComponent<C>(occupant), MakePos<C>(5.0f));   // the occupant is untouched
}

// Review Focus 1 (Clear): every pre-Clear handle is stale afterwards.
TYPED_TEST(StorageParityPos, ClearThenRecycleMisses)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    std::vector<Astra::Entity> before;
    for (int i = 0; i < 64; ++i) before.push_back(reg->CreateEntityWith(MakePos<C>(float(i)), Velocity{}));
    reg->Clear();
    EXPECT_EQ(reg->Size(), 0u);
    std::vector<Astra::Entity> after;
    for (int i = 0; i < 64; ++i) after.push_back(reg->CreateEntityWith(MakePos<C>(100.0f + i), Velocity{}));
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_FALSE(reg->HasComponent<C>(before[i])) << i;
        EXPECT_EQ(reg->GetComponent<C>(before[i]), nullptr) << i;
        EXPECT_FALSE(reg->AddComponent(before[i], MakePos<C>(-1.0f))) << i;
        EXPECT_EQ(*reg->GetComponent<C>(after[i]), MakePos<C>(100.0f + i)) << i;
    }
}

// Batches spanning several chunks (3000 x 12 B + Velocity), lifted from RegistryTest's
// BatchEntityCreation; survivors must keep their values through every swap-remove.
TYPED_TEST(StorageParityPos, BatchCreateAddEmplaceRemove)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 3000;
    std::vector<Astra::Entity> withC(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{withC}, [](size_t i) {
        return std::tuple{MakePos<C>(float(i)), Velocity{}};
    })), N);
    for (size_t i = 0; i < N; ++i) ASSERT_EQ(*reg->GetComponent<C>(withC[i]), MakePos<C>(float(i))) << i;

    std::vector<Astra::Entity> plain(N);
    ASSERT_EQ(reg->CreateEntities<Velocity>(N, std::span{plain}), N);
    reg->AddComponents(std::span{plain}, MakePos<C>(42.0f));
    for (Astra::Entity e : plain) ASSERT_EQ(*reg->GetComponent<C>(e), V::kUniversal ? C{} : MakePos<C>(42.0f));

    std::vector<Astra::Entity> emplaced(64);
    ASSERT_EQ(reg->CreateEntities<Velocity>(64, std::span{emplaced}), 64u);
    reg->EmplaceComponents<C>(std::span{emplaced}, 3.0f, 3.0f, 3.0f);
    for (Astra::Entity e : emplaced) ASSERT_EQ(*reg->GetComponent<C>(e), (V::kUniversal ? C{} : C{3.0f, 3.0f, 3.0f}));

    std::span<Astra::Entity> firstHalf(withC.data(), N / 2);
    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        size_t removed = 0;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            removed = reg->RemoveComponents<C>(firstHalf);
        }
        EXPECT_EQ(removed, 0u);
        if (kAssertsActive) EXPECT_GE(cap.count, 1);
        else                EXPECT_EQ(cap.count, 0);
        for (Astra::Entity e : firstHalf) EXPECT_TRUE(reg->HasComponent<C>(e));
    }
    else
    {
        EXPECT_EQ(reg->RemoveComponents<C>(firstHalf), N / 2);
        for (size_t i = 0; i < N; ++i)
        {
            ASSERT_EQ(reg->HasComponent<C>(withC[i]), i >= N / 2) << i;
            if (i >= N / 2) ASSERT_EQ(*reg->GetComponent<C>(withC[i]), MakePos<C>(float(i))) << i;
        }
        EXPECT_EQ(reg->RemoveComponents<C>(firstHalf), 0u);   // all absent now
    }
}

// The type-erased paths (CommandBuffer's funnel) and the reflection APIs (s3.3 last row):
// store membership must be merged into GetEntityComponents / InspectEntity / ByHash.
// RULINGS Task 37: a by-id add of an already-present component returns false for EVERY
// kind; a Universal component is always present, so its first by-id add is the refusal.
TYPED_TEST(StorageParityPos, ByIdPathsAndReflectionSeeTheComponent)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    const Astra::ComponentID id = Astra::TypeID<C>::Value();
    const uint64_t hash = Astra::TypeID<C>::Hash();
    const C value = MakePos<C>(3.0f);

    EXPECT_EQ(reg->AddComponentByID(e, id, &value, sizeof(C)), !V::kUniversal);   // present => false
    EXPECT_FALSE(reg->AddComponentByID(e, id, &value, sizeof(C)));                // now present in every kind
    const C expected = V::kUniversal ? C{} : value;
    ASSERT_TRUE(reg->HasComponentByHash(e, hash));
    ASSERT_NE(reg->GetComponentByHash(e, hash), nullptr);
    EXPECT_EQ(*static_cast<const C*>(reg->GetComponentByHash(e, hash)), expected);
    EXPECT_EQ(reg->GetComponentByHash(e, hash), static_cast<void*>(reg->GetComponent<C>(e)));

    auto countId = [&](const std::vector<const Astra::ComponentDescriptor*>& ds) {
        return std::count_if(ds.begin(), ds.end(), [&](const Astra::ComponentDescriptor* d) { return d && d->id == id; });
    };
    EXPECT_EQ(countId(reg->GetEntityComponents(e)), 1);
    const auto infos = reg->InspectEntity(e);
    auto it = std::find_if(infos.begin(), infos.end(), [&](const auto& ci) { return ci.descriptor && ci.descriptor->id == id; });
    ASSERT_NE(it, infos.end());
    EXPECT_EQ(it->data, static_cast<void*>(reg->GetComponent<C>(e)));

    if constexpr (!V::kUniversal)
    {
        EXPECT_TRUE(reg->RemoveComponentByID(e, id));
        EXPECT_FALSE(reg->HasComponentByHash(e, hash));
        EXPECT_EQ(reg->GetComponentByHash(e, hash), nullptr);
        EXPECT_EQ(countId(reg->GetEntityComponents(e)), 0);
        EXPECT_EQ(reg->AddComponentsByID(std::span{&e, 1}, id, &value, sizeof(C)), 1u);
        EXPECT_EQ(reg->RemoveComponentsByID(std::span{&e, 1}, id), 1u);
        EXPECT_FALSE(reg->HasComponent<C>(e));
    }
    else
    {
        EXPECT_EQ(reg->AddComponentsByID(std::span{&e, 1}, id, &value, sizeof(C)), 0u);   // present: nothing added
        EXPECT_EQ(*reg->GetComponent<C>(e), C{});
    }
}

// Signal timing and payload (s3.3 row "Signals", s7.4).
TYPED_TEST(StorageParityPos, SignalsFireOnGenuineChangeOnly)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    reg->EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    const Astra::ComponentID id = Astra::TypeID<C>::Value();
    std::vector<std::pair<Astra::Entity, C>> added, removed;
    reg->GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev) {
            if (ev.componentId == id) added.emplace_back(ev.entity, *static_cast<const C*>(ev.component));
        });
    reg->GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev) {   // emitted BEFORE removal: read the live value
            if (ev.componentId == id) removed.emplace_back(ev.entity, *static_cast<const C*>(ev.component));
        });

    Astra::Entity e = reg->CreateEntity<Velocity>();
    if constexpr (V::kUniversal)
    {
        ASSERT_EQ(added.size(), 1u);   // Universal: ComponentAdded at create (s3.3)
        EXPECT_EQ(added[0].first, e);
        added.clear();
    }
    else
    {
        EXPECT_TRUE(added.empty());
    }

    ASSERT_TRUE(reg->AddComponent(e, MakePos<C>(1.0f)));
    if constexpr (V::kUniversal)
    {
        EXPECT_TRUE(added.empty());    // a no-op add is silent
    }
    else
    {
        ASSERT_EQ(added.size(), 1u);
        EXPECT_EQ(added[0].first, e);
        EXPECT_EQ(added[0].second, MakePos<C>(1.0f));
    }
    added.clear();
    ASSERT_TRUE(reg->AddComponent(e, MakePos<C>(2.0f)));   // present: silent no-op
    EXPECT_TRUE(added.empty());

    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        {
            Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
            EXPECT_FALSE(reg->RemoveComponent<C>(e));
        }
        EXPECT_TRUE(removed.empty());  // refused: no signal
    }
    else
    {
        ASSERT_TRUE(reg->RemoveComponent<C>(e));
        ASSERT_EQ(removed.size(), 1u);
        EXPECT_EQ(removed[0].first, e);
        EXPECT_EQ(removed[0].second, MakePos<C>(1.0f));
        EXPECT_FALSE(reg->RemoveComponent<C>(e));
        EXPECT_EQ(removed.size(), 1u);
    }
    const size_t removedBeforeDestroy = removed.size();
    reg->DestroyEntity(e);
    EXPECT_EQ(removed.size(), removedBeforeDestroy);   // destroy emits no ComponentRemoved (today's rule)
}

// CommandBuffer record/execute (CommandBufferTest CreateEntityWithComponents / EmplaceComponent /
// BatchOperations / RemoveComponents / DestroyEntity), incl. the creation funnel (s7.1).
// RULINGS Task 37: the Universal row does not record adds of the always-present component
// (an add of a present component fails Execute for every kind; see the dedicated bodies below).
TYPED_TEST(StorageParityPos, CommandBufferPlaysBackIdentically)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::CommandBuffer cmd(reg.get());
    Astra::Entity t = cmd.CreateEntity();
    cmd.AddComponent(t, Velocity{1.0f, 0.0f, 0.0f});
    Astra::Entity u = cmd.CreateEntity();
    std::vector<Astra::Entity> batch = {cmd.CreateEntity(), cmd.CreateEntity(), cmd.CreateEntity()};
    if constexpr (!V::kUniversal)
    {
        cmd.AddComponent(t, MakePos<C>(1.0f));
        cmd.EmplaceComponent<C>(u, 2.0f, 3.0f, 4.0f);
        cmd.AddComponents<C>(batch, MakePos<C>(10.0f));
    }
    ASSERT_TRUE(cmd.Execute().IsOk());

    ASSERT_TRUE(reg->IsValid(t));
    ASSERT_TRUE(reg->IsValid(u));
    EXPECT_TRUE(reg->HasComponent<C>(t));   // Universal: constructed by the funnel, never skipped
    EXPECT_TRUE(reg->HasComponent<C>(u));
    EXPECT_EQ(*reg->GetComponent<C>(t), V::kUniversal ? C{} : MakePos<C>(1.0f));
    EXPECT_EQ(*reg->GetComponent<C>(u), (V::kUniversal ? C{} : C{2.0f, 3.0f, 4.0f}));
    for (Astra::Entity b : batch)
    {
        ASSERT_TRUE(reg->IsValid(b));
        EXPECT_EQ(*reg->GetComponent<C>(b), V::kUniversal ? C{} : MakePos<C>(10.0f));
    }

    if constexpr (!V::kUniversal)
    {
        Astra::CommandBuffer rm(reg.get());
        rm.RemoveComponent<C>(t);
        ASSERT_TRUE(rm.Execute().IsOk());
        EXPECT_FALSE(reg->HasComponent<C>(t));
        EXPECT_TRUE(reg->HasComponent<Velocity>(t));
    }
    Astra::CommandBuffer d(reg.get());
    d.DestroyEntity(u);
    ASSERT_TRUE(d.Execute().IsOk());
    EXPECT_FALSE(reg->IsValid(u));
    EXPECT_EQ(reg->GetComponent<C>(u), nullptr);
    for (Astra::Entity b : batch) EXPECT_TRUE(reg->HasComponent<C>(b));   // other holders unaffected
}

// Controller carry (c), add side: a deferred add of an already-present component fails
// Execute the same way for every kind (Universal is always present), reports
// ExecutionFailed, and leaves the live value untouched. Batch: every slot counts as failed.
TYPED_TEST(StorageParityPos, CommandBufferAddOfAPresentComponentFailsIdenticallyAndReports)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakePos<C>(1.0f), Velocity{});
    {
        Astra::CommandBuffer cmd(reg.get());
        cmd.AddComponent(e, MakePos<C>(9.0f));
        auto result = cmd.Execute();
        ASSERT_TRUE(result.IsErr());
        ASSERT_NE(result.GetError(), nullptr);
        EXPECT_EQ(*result.GetError(), Astra::CommandError::ExecutionFailed);
    }
    {
        Astra::CommandBuffer cmd(reg.get());
        cmd.EmplaceComponent<C>(e, 8.0f, 8.0f, 8.0f);
        EXPECT_TRUE(cmd.Execute().IsErr());
    }
    EXPECT_EQ(*reg->GetComponent<C>(e), MakePos<C>(1.0f));   // untouched

    constexpr size_t N = 4;
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{es}, [](size_t i) {
        return std::tuple{MakePos<C>(float(i)), Velocity{}};
    })), N);
    {
        Astra::CommandBuffer cmd(reg.get());
        cmd.AddComponents<C>(es, MakePos<C>(9.0f));
        auto result = cmd.Execute();
        ASSERT_TRUE(result.IsErr());
        ASSERT_NE(result.GetError(), nullptr);
        EXPECT_EQ(*result.GetError(), Astra::CommandError::ExecutionFailed);
        EXPECT_EQ(cmd.GetLastBatchFailureCount(), N);
    }
    for (size_t i = 0; i < N; ++i) EXPECT_EQ(*reg->GetComponent<C>(es[i]), MakePos<C>(float(i))) << i;
}

// Controller carry (b)+(c), remove side: Table/Sparse remove succeeds (single and batch);
// Universal is refused AT EXECUTE, and the deferred error is actually reported
// (ExecutionFailed, every batch slot counted failed, one assert per refused remove).
TYPED_TEST(StorageParityPos, CommandBufferRemoveSingleAndBatchReportTheUniversalRefusal)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 5;
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{es}, [](size_t i) {
        return std::tuple{MakePos<C>(float(i)), Velocity{}};
    })), N);

    if constexpr (V::kUniversal)
    {
        AssertCount cap;
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &cap);
        {
            Astra::CommandBuffer cmd(reg.get());
            cmd.RemoveComponent<C>(es[0]);
            auto result = cmd.Execute();
            ASSERT_TRUE(result.IsErr());
            ASSERT_NE(result.GetError(), nullptr);
            EXPECT_EQ(*result.GetError(), Astra::CommandError::ExecutionFailed);
        }
        EXPECT_EQ(cap.count, kAssertsActive ? 1 : 0);
        cap.count = 0;
        {
            Astra::CommandBuffer cmd(reg.get());
            cmd.RemoveComponents<C>(es);
            auto result = cmd.Execute();
            ASSERT_TRUE(result.IsErr());
            ASSERT_NE(result.GetError(), nullptr);
            EXPECT_EQ(*result.GetError(), Astra::CommandError::ExecutionFailed);
            EXPECT_EQ(cmd.GetLastBatchFailureCount(), N);
        }
        EXPECT_EQ(cap.count, kAssertsActive ? int(N) : 0);
        for (size_t i = 0; i < N; ++i)
        {
            ASSERT_TRUE(reg->HasComponent<C>(es[i])) << i;
            EXPECT_EQ(*reg->GetComponent<C>(es[i]), MakePos<C>(float(i))) << i;
        }
    }
    else
    {
        {
            Astra::CommandBuffer cmd(reg.get());
            cmd.RemoveComponent<C>(es[0]);
            ASSERT_TRUE(cmd.Execute().IsOk());
        }
        EXPECT_FALSE(reg->HasComponent<C>(es[0]));
        {
            Astra::CommandBuffer cmd(reg.get());
            cmd.RemoveComponents<C>(std::span<const Astra::Entity>(es).subspan(1));
            ASSERT_TRUE(cmd.Execute().IsOk());
        }
        for (Astra::Entity e : es) EXPECT_FALSE(reg->HasComponent<C>(e));
        // Removing an absent component fails Execute and counts every slot.
        Astra::CommandBuffer again(reg.get());
        again.RemoveComponents<C>(es);
        auto result = again.Execute();
        ASSERT_TRUE(result.IsErr());
        EXPECT_EQ(again.GetLastBatchFailureCount(), N);
    }
}

// ======================= Enableable variants (ViewTest.cpp:491-587, CommandBufferTest.cpp:735-761) =======================

template<class V> class StorageParityEnableable : public ::testing::Test {};
using EnVariants = ::testing::Types<TableV<Astra::Test::Hierarchy>,
                                    SparseV<ParityHierS>,
                                    UniversalV<Astra::Test::Hierarchy>>;
TYPED_TEST_SUITE(StorageParityEnableable, EnVariants, VariantNames);

TYPED_TEST(StorageParityEnableable, BornEnabledToggleReportsAndSignalsOnChangeOnly)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakeHier<C>(7), Velocity{});
    EXPECT_TRUE(reg->IsEnabled<C>(e));

    reg->EnableSignals(Astra::Signal::ComponentEnabled | Astra::Signal::ComponentDisabled);
    const Astra::ComponentID id = Astra::TypeID<C>::Value();
    int enabledSignals = 0, disabledSignals = 0;
    reg->GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const Astra::Events::ComponentEnabled& ev) { if (ev.componentId == id) ++enabledSignals; });
    reg->GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const Astra::Events::ComponentDisabled& ev) { if (ev.componentId == id) ++disabledSignals; });

    EXPECT_TRUE(reg->SetEnabled<C>(e, false));
    EXPECT_FALSE(reg->IsEnabled<C>(e));
    EXPECT_EQ(disabledSignals, 1);
    EXPECT_TRUE(reg->SetEnabled<C>(e, false));      // idempotent: still true, silent
    EXPECT_EQ(disabledSignals, 1);
    EXPECT_TRUE(reg->HasComponent<C>(e));           // existence never lies
    EXPECT_EQ(reg->GetComponent<C>(e)->parent, 7u);
    EXPECT_TRUE(reg->SetEnabled<C>(e, true));
    EXPECT_TRUE(reg->IsEnabled<C>(e));
    EXPECT_EQ(enabledSignals, 1);
}

// Review Focus 1 (SetEnabled): a stale handle never toggles the new occupant.
TYPED_TEST(StorageParityEnableable, StaleAndMissingAreRefused)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity dead = reg->CreateEntityWith(MakeHier<C>(1), Velocity{});
    reg->DestroyEntity(dead);
    Astra::Entity occupant = RecycleIndex(*reg, dead, [](Astra::Registry& r) {
        return r.CreateEntityWith(MakeHier<C>(2), Velocity{});
    });
    ASSERT_TRUE(occupant.IsValid());
    ASSERT_EQ(occupant.GetID(), dead.GetID());
    EXPECT_FALSE(reg->SetEnabled<C>(dead, false));
    EXPECT_FALSE(reg->IsEnabled<C>(dead));
    EXPECT_TRUE(reg->IsEnabled<C>(occupant));
    EXPECT_EQ(reg->GetComponent<C>(occupant)->parent, 2u);
    if constexpr (!V::kUniversal)
    {
        Astra::Entity without = reg->CreateEntity<Velocity>();
        EXPECT_FALSE(reg->SetEnabled<C>(without, false));   // missing component
        EXPECT_FALSE(reg->IsEnabled<C>(without));
    }
}

// Disabled state and values survive every relocation kind: archetype moves (Health is
// Table), swap-removes (destroy) and compaction (Defragment); views agree.
TYPED_TEST(StorageParityEnableable, DisabledStateSurvivesRelocationAndViewsAgree)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    constexpr size_t N = 300;
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg->CreateEntitiesWith<C, Velocity>(N, std::span{es}, [](size_t i) {
        return std::tuple{MakeHier<C>(uint32_t(i)), Velocity{}};
    })), N);
    for (size_t i = 0; i < N; i += 3) ASSERT_TRUE(reg->SetEnabled<C>(es[i], false));
    for (int i : {64, 65, 127, 128}) (void)reg->SetEnabled<C>(es[size_t(i)], false);   // word boundaries
    for (size_t i = 0; i < N; i += 2) ASSERT_TRUE(reg->AddComponent(es[i], Health{}));
    for (size_t i = 0; i < N; i += 5) reg->DestroyEntity(es[i]);
    (void)reg->Defragment();

    auto disabled = [](size_t i) { return i % 3 == 0 || i == 64 || i == 65 || i == 127 || i == 128; };
    size_t alive = 0, enabled = 0;
    for (size_t i = 0; i < N; ++i)
    {
        if (i % 5 == 0) { EXPECT_FALSE(reg->IsValid(es[i])); continue; }
        ++alive;
        enabled += disabled(i) ? 0 : 1;
        EXPECT_EQ(reg->IsEnabled<C>(es[i]), !disabled(i)) << "entity " << i;
        EXPECT_EQ(reg->GetComponent<C>(es[i])->parent, uint32_t(i)) << "entity " << i;
    }
    size_t seen = 0;
    reg->CreateView<C>().ForEach([&](Astra::Entity, C&) { ++seen; });
    EXPECT_EQ(seen, enabled);
    EXPECT_EQ(reg->CreateView<C>().Size(), enabled);
    EXPECT_EQ(reg->CreateView<Astra::IncludeDisabled<C>>().Size(), alive);
    size_t nullOpt = 0;
    reg->CreateView<Velocity, Astra::Optional<C>>().ForEach([&](Astra::Entity, Velocity&, C* c) { nullOpt += (c == nullptr); });
    EXPECT_EQ(nullOpt, alive - enabled);             // a disabled Optional<T> yields null
}

TYPED_TEST(StorageParityEnableable, DeferredSetEnabledAppliesAtFlushAndStaleFails)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntityWith(MakeHier<C>(1), Velocity{});
    Astra::CommandBuffer cmd(reg.get());
    cmd.SetEnabled<C>(e, false);
    EXPECT_TRUE(reg->IsEnabled<C>(e));               // not yet applied
    ASSERT_TRUE(cmd.Execute().IsOk());
    EXPECT_FALSE(reg->IsEnabled<C>(e));

    Astra::Entity dead = reg->CreateEntityWith(MakeHier<C>(2), Velocity{});
    reg->DestroyEntity(dead);
    Astra::CommandBuffer stale(reg.get());
    stale.SetEnabled<C>(dead, false);
    EXPECT_TRUE(stale.Execute().IsErr());
}

// ======================= Tag variants (enableable Table tag vs enableable Sparse tag) =======================
// s3.2: every tag may be enableable -- Table tags through a bit-only column (EnTag, Task 6),
// Sparse tags through the store (SpTag). The Universal tag has its own suite below.

template<class V> class StorageParityTag : public ::testing::Test {};
using TagVariants = ::testing::Types<TableV<Astra::Test::EnTag>, SparseV<ParityTagS>>;
TYPED_TEST_SUITE(StorageParityTag, TagVariants, VariantNames);

TYPED_TEST(StorageParityTag, AddHasRemoveAndFilters)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    std::vector<Astra::Entity> es(100);
    ASSERT_EQ(reg->CreateEntities<Velocity>(100, std::span{es}), 100u);
    for (size_t i = 0; i < 100; i += 4) ASSERT_TRUE(reg->AddComponent(es[i], C{}));
    for (size_t i = 0; i < 100; ++i) EXPECT_EQ(reg->HasComponent<C>(es[i]), i % 4 == 0) << i;

    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 25u);
    EXPECT_EQ((reg->CreateView<Velocity, Astra::Not<C>>().Size()), 75u);
    size_t n = 0;
    reg->CreateView<Velocity, Astra::With<C>>().ForEach([&](Astra::Entity, Velocity&) { ++n; });
    EXPECT_EQ(n, 25u);

    for (size_t i = 0; i < 100; i += 8) ASSERT_TRUE(reg->RemoveComponent<C>(es[i]));   // 13 removed
    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 12u);
    EXPECT_FALSE(reg->RemoveComponent<C>(es[0]));
    EXPECT_TRUE(reg->HasComponentByHash(es[4], Astra::TypeID<C>::Hash()));
    EXPECT_NE(reg->GetComponentByHash(es[4], Astra::TypeID<C>::Hash()), nullptr);   // present tag: sentinel, never null
    EXPECT_FALSE(reg->HasComponentByHash(es[8], Astra::TypeID<C>::Hash()));

    // Enableable tags (s3.2): toggling never changes existence, only visibility.
    EXPECT_TRUE(reg->IsEnabled<C>(es[4]));
    EXPECT_TRUE(reg->SetEnabled<C>(es[4], false));
    EXPECT_FALSE(reg->IsEnabled<C>(es[4]));
    EXPECT_TRUE(reg->HasComponent<C>(es[4]));
    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 11u);                          // disabled: filtered out
    EXPECT_EQ((reg->CreateView<Velocity, Astra::IncludeDisabled<C>>().Size()), 12u);
    EXPECT_FALSE(reg->SetEnabled<C>(es[8], false));                                  // absent: refused
    EXPECT_TRUE(reg->SetEnabled<C>(es[4], true));
    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 12u);
}

// Controller carry (a): an enableable Universal TAG (EmptyComponentSentinel payload)
// behaves like a Table enableable tag -- present, payload is the sentinel (never null),
// enabled state toggles visibility only. EnTag is reused (no new id).
template<class V> class StorageParityUniversalTag : public ::testing::Test {};
using UTagVariants = ::testing::Types<TableV<Astra::Test::EnTag>, UniversalV<Astra::Test::EnTag>>;
TYPED_TEST_SUITE(StorageParityUniversalTag, UTagVariants, VariantNames);

TYPED_TEST(StorageParityUniversalTag, PayloadIsTheSentinelAndEnabledStateToggles)
{
    using V = TypeParam;
    using C = typename V::Comp;
    auto reg = V::Make();
    Astra::Entity e = reg->CreateEntity<Velocity>();
    Astra::Entity other = reg->CreateEntity<Velocity>();
    Give<V>(*reg, e, C{});
    Give<V>(*reg, other, C{});
    const uint64_t hash = Astra::TypeID<C>::Hash();

    ASSERT_TRUE(reg->HasComponent<C>(e));
    C* typed = reg->GetComponent<C>(e);
    ASSERT_NE(typed, nullptr);                                    // a present tag is never a null payload
    // The by-hash (type-erased) payload of a present tag is the shared sentinel in every kind;
    // the typed pointer is only required to be non-null (Table hands out a column-relative address).
    EXPECT_EQ(reg->GetComponentByHash(e, hash), Astra::EmptyComponentSentinel());
    EXPECT_EQ(static_cast<const void*>(std::as_const(*reg).GetComponent<C>(e)), static_cast<void*>(typed));

    EXPECT_TRUE(reg->IsEnabled<C>(e));
    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 2u);
    EXPECT_TRUE(reg->SetEnabled<C>(e, false));
    EXPECT_FALSE(reg->IsEnabled<C>(e));
    EXPECT_TRUE(reg->IsEnabled<C>(other));                        // the neighbour is untouched
    EXPECT_TRUE(reg->HasComponent<C>(e));                         // existence never lies
    EXPECT_NE(reg->GetComponent<C>(e), nullptr);                  // payload survives the disable
    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 1u);       // disabled: filtered out
    EXPECT_EQ((reg->CreateView<Velocity, Astra::IncludeDisabled<C>>().Size()), 2u);
    EXPECT_TRUE(reg->SetEnabled<C>(e, true));
    EXPECT_EQ((reg->CreateView<Velocity, C>().Size()), 2u);
}
