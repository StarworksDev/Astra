#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
#include <gtest/gtest.h>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include "../TestComponents.hpp"
#include "../Support/StorageOverride.hpp"

namespace
{
    using namespace Astra::Test;

    struct ReleaseLog
    {
        Astra::ComponentRegistry* creg = nullptr;
        std::vector<Astra::ComponentID> ids;
        std::vector<bool> installedAtCall;
        std::vector<Astra::ComponentDescriptor::DestructFn*> destructs;
    };

    void RecordRelease(void* ctx, Astra::ComponentID id, const Astra::ComponentDescriptor& departing)
    {
        auto* log = static_cast<ReleaseLog*>(ctx);
        log->ids.push_back(id);
        log->installedAtCall.push_back(log->creg->GetComponentDescriptor(id) != nullptr);
        log->destructs.push_back(departing.destruct);
    }
}

TEST(ComponentRegistryStoreRelease, ReleaseModuleNotifiesBeforeTheSlotChangesAndRemovedHooksStaySilent)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    ReleaseLog log;
    log.creg = creg.get();
    const uint64_t handle = creg->AddStoreReleaseHook(&log, &RecordRelease);
    EXPECT_EQ(creg->StoreReleaseHookCount(), 1u);

    const auto id = Astra::TypeID<Position>::Value();
    {
        auto mod = Astra::ComponentModule::Open(creg, "HookedPlugin");
        ASSERT_TRUE(static_cast<bool>(mod));
        mod.Register<Position>();
        const auto destruct = creg->GetComponentDescriptor(id)->destruct;
        mod.Reset();   // the plugin's Shutdown

        ASSERT_EQ(log.ids.size(), 1u);
        EXPECT_EQ(log.ids[0], id);
        EXPECT_TRUE(log.installedAtCall[0]) << "stores must be torn down while the descriptor is still installed";
        EXPECT_EQ(log.destructs[0], destruct);
        EXPECT_EQ(creg->GetComponentDescriptor(id), nullptr);
    }

    creg->RemoveStoreReleaseHook(handle);
    EXPECT_EQ(creg->StoreReleaseHookCount(), 0u);
    auto again = Astra::ComponentModule::Open(creg, "SecondPlugin");
    again.Register<Position>();
    again.Reset();
    EXPECT_EQ(log.ids.size(), 1u) << "a removed hook is never called";
}

TEST(ComponentRegistryStoreRelease, RangePurgeNotifiesLiveAndShadowedEntries)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    ReleaseLog log;
    log.creg = creg.get();
    const uint64_t handle = creg->AddStoreReleaseHook(&log, &RecordRelease);

    const auto id = Astra::TypeID<Position>::Value();
    creg->RegisterComponent<Position>();                       // anonymous base (shadowed below)
    auto mod = Astra::ComponentModule::Open(creg, "PurgeVictim");
    mod.Register<Position>();                                  // live override
    const auto* live = creg->GetComponentDescriptor(id);
    ASSERT_NE(live, nullptr);
    const void* base = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(live->defaultConstruct) & ~uintptr_t(0xFFFF));
    EXPECT_GE(creg->UnregisterModuleRange(base, size_t(1) << 30), 1u);

    EXPECT_EQ(std::count(log.ids.begin(), log.ids.end(), id), 2) << "live override + shadowed base";
    for (const bool installed : log.installedAtCall) EXPECT_TRUE(installed);
    creg->RemoveStoreReleaseHook(handle);
    mod.Reset();   // idempotent after the purge
}

TEST(ComponentStorageRelease, ReleaseComponentDestroysEveryElementAndFreesTheSlot)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID id = Astra::TestSupport::InstallWithStorage<RelocationCanary>(*creg, Astra::Storage::Sparse);
    const int liveAtStart = RelocationCanary::s_live;
    {
        Astra::Registry reg(creg);
        for (int i = 0; i < 16; ++i)
        {
            const Astra::Entity e = reg.CreateEntity<Position>();
            RelocationCanary v(i);
            ASSERT_TRUE(reg.AddComponentByID(e, id, &v, sizeof(v)));
        }
        EXPECT_EQ(RelocationCanary::s_live - liveAtStart, 16);

        Astra::ComponentStorage& storage = *reg.GetComponentStorage();
        ASSERT_GE(storage.SparseSlotOf(id), 0);
        storage.ReleaseComponent(id);
        EXPECT_EQ(RelocationCanary::s_live, liveAtStart) << "every element destroyed";
        EXPECT_EQ(storage.SparseSlotOf(id), -1) << "the slot is freed";
        EXPECT_GE(storage.AcquireSparseSlot(id), 0) << "and can be acquired again";
    }
    EXPECT_EQ(RelocationCanary::s_live, liveAtStart);
}

TEST(ComponentStorageRelease, ReleaseSparseBlockDropsOnlyThatSlotsBlock)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    const Astra::ComponentID hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
    Astra::Registry reg(creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Name n("held");
    Hierarchy h{};
    ASSERT_TRUE(reg.AddComponentByID(e, nameId, &n, sizeof(n)));
    ASSERT_TRUE(reg.AddComponentByID(e, hierId, &h, sizeof(h)));

    const auto slotName = static_cast<uint16_t>(reg.GetComponentStorage()->SparseSlotOf(nameId));
    const auto slotHier = static_cast<uint16_t>(reg.GetComponentStorage()->SparseSlotOf(hierId));
    const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
    ASSERT_NE(rec, nullptr);
    ASSERT_NE(rec->chunk->FindSparseBlock(slotName), nullptr);
    ASSERT_NE(rec->chunk->FindSparseBlock(slotHier), nullptr);

    rec->chunk->ReleaseSparseBlock(slotName);
    EXPECT_EQ(rec->chunk->FindSparseBlock(slotName), nullptr);
    ASSERT_NE(rec->chunk->FindSparseBlock(slotHier), nullptr);
    EXPECT_TRUE(rec->chunk->IsSparsePresent(slotHier, rec->location.GetEntityIndex()));
    rec->chunk->ReleaseSparseBlock(slotName);   // no block: a no-op
}

TEST(ComponentStorageRelease, FreedSlotRoutesNothingSparseAndAReRegisteredTableDescriptorStaysTable)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID id = Astra::TestSupport::InstallWithStorage<RelocationCanary>(*creg, Astra::Storage::Sparse);
    Astra::Registry reg(creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    RelocationCanary v(1);
    ASSERT_TRUE(reg.AddComponentByID(e, id, &v, sizeof(v)));
    Astra::ComponentStorage& storage = *reg.GetComponentStorage();
    ASSERT_GE(storage.SparseSlotOf(id), 0);

    storage.ReleaseComponent(id);
    EXPECT_EQ(storage.SparseSlotOf(id), -1);
    EXPECT_EQ(storage.UsedSparseSlotCount(), 0u);
    EXPECT_EQ(storage.FindSparse(id), nullptr);

    // A Table-trait descriptor registered for the same id afterwards is not routed Sparse.
    Astra::TestSupport::InstallWithStorage<RelocationCanary>(*creg, Astra::Storage::Table);
    EXPECT_FALSE(reg.SetEnabledByID(e, id, true));
    EXPECT_FALSE(reg.IsEnabledByID(e, id));
    EXPECT_EQ(storage.SparseSlotOf(id), -1) << "the Table answer never re-acquires a sparse slot";
    EXPECT_EQ(storage.FindSparse(id), nullptr);
}

TEST(ComponentStorageRelease, ReleaseLeavesTheAllSetsLoopsSafe)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    const Astra::ComponentID hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
    Astra::Registry reg(creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Name n("x");
    Hierarchy h{};
    ASSERT_TRUE(reg.AddComponentByID(e, nameId, &n, sizeof(n)));
    ASSERT_TRUE(reg.AddComponentByID(e, hierId, &h, sizeof(h)));
    Astra::ComponentStorage& storage = *reg.GetComponentStorage();
    ASSERT_TRUE(storage.HasAnyStore());

    storage.ReleaseComponent(nameId);
    EXPECT_NE(storage.FindSparse(hierId), nullptr) << "the other set survives";
    EXPECT_EQ(storage.FindSparse(nameId), nullptr);
    storage.Clear();                          // iterates the live-set list: must not touch the released set
    storage.ReleaseComponent(nameId);         // idempotent
    storage.ReleaseComponent(hierId);
    EXPECT_FALSE(storage.HasAnyStore());
}

namespace
{
    struct DestroyOtherCtx
    {
        std::unique_ptr<Astra::Registry> victim;
        uint64_t victimHandle = 0;
        Astra::ComponentRegistry* creg = nullptr;
        int calls = 0;
    };
    void DestroyVictim(void* ctx, Astra::ComponentID, const Astra::ComponentDescriptor&)
    {
        auto* c = static_cast<DestroyOtherCtx*>(ctx);
        ++c->calls;
        c->victim.reset();   // the "Registry" being destroyed removes its own hook
        c->creg->RemoveStoreReleaseHook(c->victimHandle);
    }
    void MustNotRun(void* ctx, Astra::ComponentID, const Astra::ComponentDescriptor&)
    {
        ++*static_cast<int*>(ctx);
    }
}

TEST(ComponentRegistryStoreRelease, AHookRemovedByAnEarlierHookIsNotCalled)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    DestroyOtherCtx a;
    a.creg = creg.get();
    int victimCalls = 0;
    const uint64_t ha = creg->AddStoreReleaseHook(&a, &DestroyVictim);
    a.victimHandle = creg->AddStoreReleaseHook(&victimCalls, &MustNotRun);
    {
        auto mod = Astra::ComponentModule::Open(creg, "TwoHooks");
        ASSERT_TRUE(static_cast<bool>(mod));
        mod.Register<Position>();
        mod.Reset();
    }
    EXPECT_EQ(a.calls, 1);
    EXPECT_EQ(victimCalls, 0) << "the second hook was removed during the first: never called with a dangling ctx";
    creg->RemoveStoreReleaseHook(ha);
}
