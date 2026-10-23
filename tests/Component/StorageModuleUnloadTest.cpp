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
#include "../Support/DiagnosticsTestGuards.hpp"

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

    struct AssertCount { int n = 0; };

    Astra::AssertAction CountAndContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++static_cast<AssertCount*>(user)->n;
        return Astra::AssertAction::Continue;
    }

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

// ---- Task 36: Registry tears its stores down at module unload (Review Focus 5) ----

namespace
{
    bool g_pluginUnloaded = false;
    int g_pluginDestructs = 0;
    int g_postUnloadDestructs = 0;

    // The plugin image's destructor for RelocationCanary. Once the plugin is
    // "unloaded", a call here stands for a jump into unmapped code: it is counted
    // and does nothing else.
    void PluginCanaryDestruct(void* p)
    {
        if (g_pluginUnloaded)
        {
            ++g_postUnloadDestructs;
            return;
        }
        ++g_pluginDestructs;
        static_cast<RelocationCanary*>(p)->~RelocationCanary();
    }

    struct PluginInstall { uint32_t owner; Astra::ComponentID id; };

    // Installs RelocationCanary as a plugin-owned type whose destructor is
    // PluginCanaryDestruct: the in-process stand-in for a DLL's descriptors.
    PluginInstall InstallPluginCanary(Astra::ComponentRegistry& creg, Astra::Storage storage)
    {
        static int s_pluginImage = 0;
        g_pluginUnloaded = false;
        g_pluginDestructs = 0;
        g_postUnloadDestructs = 0;
        const Astra::ComponentID id = Astra::TypeID<RelocationCanary>::Value();
        Astra::ComponentDescriptor desc = Astra::ComponentRegistry::MakeDescriptor<RelocationCanary>(id, nullptr);
        desc.storage = storage;
        desc.destruct = &PluginCanaryDestruct;
        desc.name = "PluginCanary";
        const uint32_t owner = creg.OpenModuleId("CanaryPlugin");
        EXPECT_EQ(creg.InstallOwned(id, owner, desc, &s_pluginImage), Astra::InstallResult::Installed);
        return {owner, id};
    }

    bool AnyChunkHasBlock(Astra::Registry& reg, int slot)
    {
        for (Astra::Archetype* archetype : reg.GetArchetypeManager()->GetArchetypes())
            for (const auto& chunk : archetype->GetChunks())
                if (chunk && chunk->FindSparseBlock(static_cast<uint16_t>(slot)))
                    return true;
        return false;
    }

    enum class UnloadPath { ReleaseModule, RangePurge };

    void RunSparsePluginUnload(UnloadPath path)
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position>();
        const PluginInstall plugin = InstallPluginCanary(*creg, Astra::Storage::Sparse);
        const int liveAtStart = RelocationCanary::s_live;

        Astra::Registry reg(creg);
        std::vector<Astra::Entity> holders;
        for (int i = 0; i < 100; ++i)   // > 64: blocks span several words
        {
            const Astra::Entity e = reg.CreateEntity<Position>();
            RelocationCanary v(i);
            ASSERT_TRUE(reg.AddComponentByID(e, plugin.id, &v, sizeof(v)));
            holders.push_back(e);
        }
        const Astra::Entity bystander = reg.CreateEntity<Position>();
        const int slot = reg.GetComponentStorage()->SparseSlotOf(plugin.id);
        ASSERT_GE(slot, 0);
        ASSERT_TRUE(AnyChunkHasBlock(reg, slot));
        ASSERT_EQ(reg.ValidateStorageInvariants(), "");

        const int destructsBefore = g_pluginDestructs;   // set growth may already have relocated elements
        if (path == UnloadPath::ReleaseModule)
            (void)creg->ReleaseModule(plugin.owner);    // what the plugin's ComponentModule::Reset() runs
        else
            EXPECT_EQ(creg->UnregisterModuleRange(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&PluginCanaryDestruct)), 1), 1u);
        g_pluginUnloaded = true;                        // the image is gone from here on

        EXPECT_EQ(g_pluginDestructs - destructsBefore, 100) << "every element destroyed at unload, through the still-mapped destructor";
        EXPECT_EQ(RelocationCanary::s_live, liveAtStart);
        EXPECT_EQ(creg->GetComponentDescriptor(plugin.id), nullptr);
        EXPECT_EQ(reg.GetComponentStorage()->SparseSlotOf(plugin.id), -1);
        EXPECT_FALSE(AnyChunkHasBlock(reg, slot)) << "no presence block may survive for the released slot";
        EXPECT_EQ(reg.ValidateStorageInvariants(), "");

        reg.DestroyEntities(holders);
        reg.DestroyEntity(bystander);
        EXPECT_EQ(g_postUnloadDestructs, 0) << "destroying the former holders must not reach the unloaded destructor";
        EXPECT_EQ(reg.Size(), 0u);
        EXPECT_EQ(reg.ValidateStorageInvariants(), "");
    }
}

// Review Focus 5 (PINNED): unload a plugin module whose Sparse component is still
// held by live entities, then destroy those entities. No destructor may be
// reached through the unloaded descriptor, and no presence block may be left
// for the released slot. Both unload paths: the RAII ComponentModule::Reset()
// route (ReleaseModule) and the UnregisterModuleRange safety valve.
TEST(StorageModuleUnload, UnloadingAPluginSparseComponentHeldByLiveEntitiesLeavesNoDanglingDestructorOrBlock)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    RunSparsePluginUnload(UnloadPath::ReleaseModule);
}

TEST(StorageModuleUnload, RangePurgeOfAPluginSparseComponentLeavesNoDanglingDestructorOrBlock)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    RunSparsePluginUnload(UnloadPath::RangePurge);
}

// Order pin (Task 35 review): the chunk presence blocks are released before any
// AcquireSparseSlot can run again. A new Sparse type that reuses the freed slot
// must not inherit the former holders' presence bits.
TEST(StorageModuleUnload, AReusedSparseSlotNeverAliasesTheUnloadedTypesPresenceBits)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const PluginInstall plugin = InstallPluginCanary(*creg, Astra::Storage::Sparse);

    Astra::Registry reg(creg);
    std::vector<Astra::Entity> holders;
    for (int i = 0; i < 70; ++i)
    {
        const Astra::Entity e = reg.CreateEntity<Position>();
        RelocationCanary v(i);
        ASSERT_TRUE(reg.AddComponentByID(e, plugin.id, &v, sizeof(v)));
        holders.push_back(e);
    }
    const int slot = reg.GetComponentStorage()->SparseSlotOf(plugin.id);
    ASSERT_GE(slot, 0);

    (void)creg->ReleaseModule(plugin.owner);
    g_pluginUnloaded = true;

    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    const Astra::Entity fresh = reg.CreateEntity<Position>();    // same archetype, same chunk as the holders
    Name n;
    ASSERT_TRUE(reg.AddComponentByID(fresh, nameId, &n, sizeof(n)));
    EXPECT_EQ(reg.GetComponentStorage()->SparseSlotOf(nameId), slot) << "the freed slot is the lowest free one: reused";

    const Astra::SparseSet* nameSet = reg.GetComponentStorage()->FindSparse(nameId);
    ASSERT_NE(nameSet, nullptr);
    for (const Astra::Entity e : holders)
        EXPECT_FALSE(nameSet->Contains(e)) << "stale presence bits would alias the new type";
    EXPECT_TRUE(nameSet->Contains(fresh));
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
    reg.DestroyEntities(holders);
    EXPECT_EQ(g_postUnloadDestructs, 0);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
}

TEST(StorageModuleUnload, UnloadingAPluginUniversalComponentDestroysItsColumn)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const PluginInstall plugin = InstallPluginCanary(*creg, Astra::Storage::Table);   // universal by Config
    Astra::Registry::Config cfg;
    cfg.AddUniversal<RelocationCanary>();
    {
        Astra::Registry reg(creg, cfg);
        std::vector<Astra::Entity> es;
        for (int i = 0; i < 10; ++i) es.push_back(reg.CreateEntity<Position>());

        const int destructsBefore = g_pluginDestructs;
        (void)creg->ReleaseModule(plugin.owner);
        g_pluginUnloaded = true;
        EXPECT_EQ(g_pluginDestructs - destructsBefore, 10);

        reg.DestroyEntities(es);
        EXPECT_EQ(g_postUnloadDestructs, 0);
    }   // registry teardown: no store is left holding the plugin's descriptor
    EXPECT_EQ(g_postUnloadDestructs, 0);
}

// RULINGS Task 36 (PINNED): the suspended-Universal state (spec s4.4). After the
// column is dropped "always present" is suspended for that type: typed
// Has/Get answer absent, typed Add/Emplace answer false (an add never revives a
// slot, never creates a column and emits nothing), a typed remove is a quiet
// false, views never visit the entity (Not<U> matches every entity), and
// nothing reads an unconstructed slot. Once the type is registered again the
// next create rebuilds the column AND fills it for EVERY live entity (default +
// init, the s4.4 "until it is registered again" reading), so no partially-live
// column exists and every View surface agrees with HasComponent.
TEST(StorageModuleUnload, ASuspendedUniversalAnswersAbsentThroughTheTypedApiAndViews)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const PluginInstall plugin = InstallPluginCanary(*creg, Astra::Storage::Table);
    Astra::Registry::Config cfg;
    cfg.AddUniversal<RelocationCanary>();
    Astra::Registry reg(creg, cfg);
    std::vector<Astra::Entity> old;
    for (int i = 0; i < 5; ++i) old.push_back(reg.CreateEntity<Position>());
    ASSERT_TRUE(reg.HasComponent<RelocationCanary>(old[0])) << "live before unload";
    EXPECT_EQ(reg.CreateView<RelocationCanary>().Size(), 5u);

    (void)creg->ReleaseModule(plugin.owner);
    g_pluginUnloaded = true;

    const Astra::ComponentID id = plugin.id;
    ASSERT_EQ(reg.GetComponentStorage()->FindUniversal(id), nullptr);
    for (const Astra::Entity e : old)
    {
        EXPECT_FALSE(reg.HasComponent<RelocationCanary>(e));
        EXPECT_EQ(reg.GetComponent<RelocationCanary>(e), nullptr);
        EXPECT_EQ(std::as_const(reg).GetComponent<RelocationCanary>(e), nullptr);
        EXPECT_FALSE(reg.AddComponent(e, RelocationCanary(1))) << "an add never revives a suspended slot";
        EXPECT_FALSE(reg.EmplaceComponent<RelocationCanary>(e, 2));
    }
    EXPECT_EQ(reg.GetComponentStorage()->FindUniversal(id), nullptr) << "no read or add path creates a column";
    {
        AssertCount count;
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        for (const Astra::Entity e : old)
        {
            EXPECT_FALSE(reg.RemoveComponent<RelocationCanary>(e)) << "the entity does not hold it (Has is false)";
            EXPECT_FALSE(reg.RemoveComponentByID(e, id));
        }
        EXPECT_EQ(count.n, 0) << "a quiet false, not a refusal: nothing to refuse";
    }
    size_t notMatches = 0;
    reg.CreateView<Position, Astra::Not<RelocationCanary>>().ForEach([&](Astra::Entity, Position&) { ++notMatches; });
    EXPECT_EQ(notMatches, old.size()) << "Not<U> matches every entity while U is suspended, like HasComponent == false";
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<RelocationCanary>>().Size()), old.size());
    EXPECT_TRUE((reg.CreateView<Position, Astra::Not<RelocationCanary>>().Contains(old[0])));
    int visited = 0;
    reg.CreateView<RelocationCanary>().ForEach([&](Astra::Entity, RelocationCanary&) { ++visited; });
    reg.CreateView<Position, RelocationCanary>().ForEach([&](Astra::Entity, Position&, RelocationCanary&) { ++visited; });
    EXPECT_EQ(visited, 0);
    EXPECT_EQ(reg.CreateView<RelocationCanary>().Size(), 0u);
    EXPECT_FALSE(reg.CreateView<RelocationCanary>().Contains(old[0]));
    const Astra::Entity whileSuspended = reg.CreateEntity<Position>();   // descriptor gone: nothing to construct
    EXPECT_FALSE(reg.HasComponent<RelocationCanary>(whileSuspended));
    EXPECT_EQ(g_postUnloadDestructs, 0);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");

    // The type is registered again (a reloaded module): the next create rebuilds the
    // column and fills it for every live entity, predating ones included.
    (void)InstallPluginCanary(*creg, Astra::Storage::Table);
    EXPECT_EQ(reg.GetComponentStorage()->FindUniversal(id), nullptr) << "registering alone builds nothing: a create or access does";
    const Astra::Entity reborn = reg.CreateEntity<Position>();
    ASSERT_NE(reg.GetComponentStorage()->FindUniversal(id), nullptr);
    std::vector<Astra::Entity> everyone = old;
    everyone.push_back(whileSuspended);
    everyone.push_back(reborn);
    for (const Astra::Entity e : everyone)
    {
        EXPECT_TRUE(reg.HasComponent<RelocationCanary>(e)) << "retro-filled: always present holds again";
        EXPECT_NE(reg.GetComponent<RelocationCanary>(e), nullptr);
    }
    EXPECT_TRUE(reg.AddComponent(old[0], RelocationCanary(1))) << "a live Universal: the present-component no-op answer";
    EXPECT_EQ(reg.Size(), everyone.size());
    // Every View surface agrees with HasComponent: Size == ForEach count == live entities.
    size_t visits = 0;
    reg.CreateView<RelocationCanary>().ForEach([&](Astra::Entity, RelocationCanary&) { ++visits; });
    EXPECT_EQ(visits, everyone.size());
    EXPECT_EQ(reg.CreateView<RelocationCanary>().Size(), everyone.size());
    EXPECT_EQ((reg.CreateView<Position, RelocationCanary>().Size()), everyone.size());
    for (const Astra::Entity e : everyone)
        EXPECT_TRUE(reg.CreateView<RelocationCanary>().Contains(e));
    size_t withVisits = 0;
    reg.CreateView<Position, Astra::With<RelocationCanary>>().ForEach([&](Astra::Entity, Position&) { ++withVisits; });
    EXPECT_EQ(withVisits, everyone.size());
    EXPECT_EQ((reg.CreateView<Position, Astra::With<RelocationCanary>>().Size()), everyone.size());
    EXPECT_TRUE((reg.CreateView<Position, Astra::With<RelocationCanary>>().Contains(old[0])));
    size_t notVisits = 0;
    reg.CreateView<Position, Astra::Not<RelocationCanary>>().ForEach([&](Astra::Entity, Position&) { ++notVisits; });
    EXPECT_EQ(notVisits, 0u) << "every entity holds U again, so Not<U> matches none";
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<RelocationCanary>>().Size()), 0u);
    {
        AssertCount count;
        Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
        EXPECT_FALSE(reg.RemoveComponent<RelocationCanary>(reborn));
#if MOSAIC_ASSERTS_ACTIVE
        EXPECT_EQ(count.n, 1) << "a live Universal remove stays refused (and asserts in Debug)";
#else
        EXPECT_EQ(count.n, 0);
#endif
        EXPECT_TRUE(reg.HasComponent<RelocationCanary>(reborn));
    }
    reg.DestroyEntities(everyone);
    EXPECT_EQ(g_postUnloadDestructs, 0);
    EXPECT_EQ(RelocationCanary::s_live, 0);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
}

// Spec s5.1: an ENABLEABLE Universal also owns a mirror slot, and its chunk blocks
// carry present == nullptr ("all present") plus the disabled words. The unload
// releases those mirror-only blocks in every chunk, so a slot reused by a new
// Sparse type starts clean. (Timer is the suite's enableable test type.)
TEST(StorageModuleUnload, UnloadingAnEnableableUniversalReleasesItsMirrorOnlyBlocksInEveryChunk)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    static int s_timerImage = 0;
    static bool s_timerUnloaded = false;
    static int s_timerPostUnloadDestructs = 0;
    s_timerUnloaded = false;
    s_timerPostUnloadDestructs = 0;
    struct TimerImage
    {
        static void Destruct(void* p)
        {
            if (s_timerUnloaded) { ++s_timerPostUnloadDestructs; return; }
            static_cast<Timer*>(p)->~Timer();
        }
    };

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID id = Astra::TypeID<Timer>::Value();
    Astra::ComponentDescriptor desc = Astra::ComponentRegistry::MakeDescriptor<Timer>(id, nullptr);
    desc.destruct = &TimerImage::Destruct;
    desc.name = "PluginTimer";
    const uint32_t owner = creg->OpenModuleId("TimerPlugin");
    ASSERT_EQ(creg->InstallOwned(id, owner, desc, &s_timerImage), Astra::InstallResult::Installed);

    Astra::Registry::Config cfg;
    cfg.AddUniversal<Timer>();
    Astra::Registry reg(creg, cfg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 300; ++i) es.push_back(reg.CreateEntity<Position>());   // several chunks
    for (int i = 0; i < 300; i += 7) ASSERT_TRUE(reg.SetEnabled<Timer>(es[i], false));

    const int slot = reg.GetComponentStorage()->SparseSlotOf(id);
    ASSERT_GE(slot, 0) << "an enableable Universal owns a mirror slot";
    size_t chunksWithBlock = 0;
    size_t chunks = 0;
    for (Astra::Archetype* archetype : reg.GetArchetypeManager()->GetArchetypes())
        for (const auto& chunk : archetype->GetChunks())
            if (chunk && chunk->GetCount() > 0)
            {
                ++chunks;
                if (const auto* b = chunk->FindSparseBlock(static_cast<uint16_t>(slot)))
                {
                    ++chunksWithBlock;
                    EXPECT_EQ(b->present, nullptr) << "mirror-only block";
                }
            }
    ASSERT_GE(chunksWithBlock, 1u);
    ASSERT_EQ(reg.ValidateStorageInvariants(), "");

    (void)creg->ReleaseModule(owner);
    s_timerUnloaded = true;

    EXPECT_EQ(reg.GetComponentStorage()->SparseSlotOf(id), -1);
    EXPECT_FALSE(AnyChunkHasBlock(reg, slot)) << "every chunk's mirror-only block is released";
    EXPECT_EQ(reg.GetComponentStorage()->FindUniversal(id), nullptr);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");

    // A new Sparse type reusing the freed slot must not see stale disabled bits.
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    Name n;
    ASSERT_TRUE(reg.AddComponentByID(es[0], nameId, &n, sizeof(n)));
    EXPECT_EQ(reg.GetComponentStorage()->SparseSlotOf(nameId), slot);
    const Astra::SparseSet* nameSet = reg.GetComponentStorage()->FindSparse(nameId);
    ASSERT_NE(nameSet, nullptr);
    EXPECT_FALSE(nameSet->IsDisabled(es[0]));
    EXPECT_FALSE(nameSet->Contains(es[7]));
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");

    reg.DestroyEntities(es);
    EXPECT_EQ(s_timerPostUnloadDestructs, 0);
    EXPECT_EQ(reg.ValidateStorageInvariants(), "");
}

TEST(StorageModuleUnload, EveryRegistrySubscribesForItsLifetimeIncludingLoadedOnes)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    EXPECT_EQ(creg->StoreReleaseHookCount(), 0u);
    {
        Astra::Registry reg(creg);
        EXPECT_EQ(creg->StoreReleaseHookCount(), 1u);
        auto saved = reg.Save();
        ASSERT_TRUE(saved.IsOk());
        auto loaded = Astra::Registry::Load(*saved.GetValue(), creg);
        ASSERT_TRUE(loaded.IsOk());
        EXPECT_EQ(creg->StoreReleaseHookCount(), 2u);
    }
    EXPECT_EQ(creg->StoreReleaseHookCount(), 0u) << "a destroyed Registry must never be called back";
}
