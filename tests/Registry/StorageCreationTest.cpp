#include <gtest/gtest.h>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <Astra/Astra.hpp>
#include <Astra/Registry/StorageSplit.hpp>
#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

using Astra::Entity;
using Astra::Registry;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace StorageCreationTypes
{
    // A lifetime-counted Sparse component (the Tracked idiom). It is copyable, so a value
    // can also be supplied by lvalue.
    struct SparseProbe
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static inline int s_live = 0;
        int v = 0;

        SparseProbe() { ++s_live; }
        explicit SparseProbe(int x) : v(x) { ++s_live; }
        SparseProbe(const SparseProbe& o) : v(o.v) { ++s_live; }
        SparseProbe(SparseProbe&& o) noexcept : v(o.v) { ++s_live; }
        SparseProbe& operator=(const SparseProbe& o) noexcept { v = o.v; return *this; }
        SparseProbe& operator=(SparseProbe&& o) noexcept { v = o.v; return *this; }
        ~SparseProbe() { --s_live; }

        // Not trivially copyable: registration odr-uses Serialize<T> (Tracked's rationale).
        template<typename Archive>
        void Serialize(Archive& ar) { ar(v); }
    };

    // A Sparse tag: presence only, no data buffer.
    struct SparseMark
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
    };
}
using StorageCreationTypes::SparseMark;
using StorageCreationTypes::SparseProbe;

namespace
{
    int g_initCalls = 0;

    void InitPositionY(Entity, void* component, void*)
    {
        static_cast<Position*>(component)->y = -1.0f;
        ++g_initCalls;
    }

    struct AssertCount { int n = 0; };

    // Records and CONTINUES, so a failing ASTRA_ASSERT is observable in Debug without
    // aborting the test process (the CheckedAssertsTest idiom).
    Astra::AssertAction CountAndContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++static_cast<AssertCount*>(user)->n;
        return Astra::AssertAction::Continue;
    }
}

TEST(StorageSplit, PartitionsAPackByStorageTraitInPackOrder)
{
    using Astra::Storage;
    using Astra::Detail::StorageIndexSeq;
    using Astra::Detail::TypesWithStorage;
    static_assert(std::is_same_v<StorageIndexSeq<Storage::Table, Position, SparseProbe, Velocity, SparseMark>,
                                 std::index_sequence<0, 2>>);
    static_assert(std::is_same_v<StorageIndexSeq<Storage::Sparse, Position, SparseProbe, Velocity, SparseMark>,
                                 std::index_sequence<1, 3>>);
    static_assert(std::is_same_v<TypesWithStorage<Storage::Table, Position&&, const SparseProbe&, Velocity&>,
                                 std::tuple<Position, Velocity>>);
    static_assert(std::is_same_v<TypesWithStorage<Storage::Sparse>, std::tuple<>>);
    SUCCEED();
}

TEST(StorageCreation, SparseMembersGoToTheSetNotTheArchetype)
{
    Registry reg;
    const Entity a = reg.CreateEntityWith(Position{1.0f, 2.0f, 3.0f}, SparseProbe{42});
    const Entity b = reg.CreateEntityWith(Position{4.0f, 5.0f, 6.0f});
    SparseProbe lvalue{7};
    const Entity c = reg.CreateEntityWith(Position{}, lvalue);   // an lvalue is copied, not moved

    ASSERT_TRUE(reg.HasComponent<SparseProbe>(a));
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(b));
    const SparseProbe* s = std::as_const(reg).GetComponent<SparseProbe>(a);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->v, 42);
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(c)->v, 7);
    EXPECT_EQ(lvalue.v, 7);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(a)->x, 1.0f);

    // Non-fragmenting (D3): with and without the sparse member, one {Position} archetype.
    const Astra::EntityRecord* ra = reg.GetArchetypeManager()->GetEntityRecord(a);
    const Astra::EntityRecord* rb = reg.GetArchetypeManager()->GetEntityRecord(b);
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(ra->archetype, rb->archetype);
    EXPECT_FALSE(ra->archetype->GetMask().Test(Astra::TypeID<SparseProbe>::Value()));
}

TEST(StorageCreation, CreateEntityDefaultConstructsSparseMembersAndTags)
{
    Registry reg;
    const Entity e = reg.CreateEntity<Velocity, SparseProbe, SparseMark>();
    EXPECT_TRUE(reg.HasComponent<Velocity>(e));
    ASSERT_TRUE(reg.HasComponent<SparseProbe>(e));
    EXPECT_TRUE(reg.HasComponent<SparseMark>(e));
    EXPECT_NE(std::as_const(reg).GetComponent<SparseMark>(e), nullptr);   // a present tag is non-null
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(e)->v, 0);

    const Entity bare = reg.CreateEntity<Velocity>();
    EXPECT_FALSE(reg.HasComponent<SparseMark>(bare));
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseMark>(bare), nullptr);
}

TEST(StorageCreation, BatchCreatorsSplitToo)
{
    Registry reg;
    std::vector<Entity> ents(8);
    ASSERT_EQ((reg.CreateEntities<Position, SparseProbe>(8, std::span{ents})), 8u);
    for (Entity e : ents)
    {
        EXPECT_TRUE(reg.HasComponent<Position>(e));
        EXPECT_TRUE(reg.HasComponent<SparseProbe>(e));
    }

    std::vector<size_t> calls;
    ASSERT_EQ((reg.CreateEntitiesWith<Position, SparseProbe>(8, std::span{ents}, [&](size_t i)
    {
        calls.push_back(i);
        return std::tuple{Position{static_cast<float>(i), 0.0f, 0.0f}, SparseProbe{static_cast<int>(i)}};
    })), 8u);
    EXPECT_EQ(calls, (std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7}));   // exactly once each, in order
    for (size_t i = 0; i < ents.size(); ++i)
    {
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(ents[i])->x, static_cast<float>(i));
        EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(ents[i])->v, static_cast<int>(i));
    }
}

TEST(StorageCreation, SuppliedUniversalIsMovedInAndNeverInitialised)
{
    g_initCalls = 0;
    Registry::Config cfg;
    cfg.AddUniversal<Position>(&InitPositionY);
    Registry reg(cfg);

    const Entity supplied = reg.CreateEntityWith(Position{9.0f, 9.0f, 9.0f}, Velocity{});
    const Position* p = std::as_const(reg).GetComponent<Position>(supplied);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->y, 9.0f);               // s4.2: a supplied value is never overwritten by init
    EXPECT_EQ(g_initCalls, 0);
    const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(supplied);
    ASSERT_NE(rec, nullptr);
    EXPECT_TRUE(rec->archetype->GetMask().Test(Astra::TypeID<Velocity>::Value()));
    EXPECT_FALSE(rec->archetype->GetMask().Test(Astra::TypeID<Position>::Value()));

    Position lvalue(3.0f, 3.0f, 3.0f);
    const Entity copied = reg.CreateEntityWith(lvalue);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(copied)->y, 3.0f);
    EXPECT_FLOAT_EQ(lvalue.y, 3.0f);
    EXPECT_EQ(g_initCalls, 0);

    const Entity named = reg.CreateEntity<Position, Velocity>();   // named, no value: default + init
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(named)->y, -1.0f);
    EXPECT_EQ(g_initCalls, 1);

    std::vector<Entity> ents(3);
    ASSERT_EQ((reg.CreateEntitiesWith<Position>(3, std::span{ents},
        [](size_t i) { return std::tuple{Position{0.0f, static_cast<float>(i), 0.0f}}; })), 3u);
    for (size_t i = 0; i < 3; ++i)
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(ents[i])->y, static_cast<float>(i));
    EXPECT_EQ(g_initCalls, 1);                 // supplied by the generator: no init
    ASSERT_EQ((reg.CreateEntities<Position>(3, std::span{ents})), 3u);
    EXPECT_EQ(g_initCalls, 4);                 // named only: init for each
}

TEST(StorageCreation, UniversalOverridesTheSparseTrait)
{
    Registry::Config cfg;
    cfg.AddUniversal<SparseProbe>();
    Registry reg(cfg);
    const Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.HasComponent<SparseProbe>(e));        // universal: present on every entity
    const Entity withValue = reg.CreateEntityWith(SparseProbe{5});
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(withValue)->v, 5);
}

// Spec s7.1 (final review I1): "Table members choose the archetype" in ONE placement, also
// when the pack supplies or names a member this registry declares Universal (Arcane scene
// load re-creates entities with their saved Identity). No root hop, no per-member move, so
// none of the intermediate archetypes {V} and {V, H} is ever created. Every creation form,
// each in a fresh registry so an earlier form cannot have pre-created an intermediate.
namespace
{
    using Astra::Test::Health;
    using Astra::Test::Player;
    using Astra::Test::Transform;

    Registry::Config UniversalPositionConfig()
    {
        Registry::Config cfg;
        cfg.AddUniversal<Position>(&InitPositionY);
        return cfg;
    }

    // The entity sits in exactly {Velocity, Health, Transform, Player}: Position is a column
    // of no archetype in this registry.
    void ExpectTablePartPlacedOnce(Registry& reg, Entity e, size_t archetypesBefore)
    {
        EXPECT_EQ(reg.GetArchetypeCount(), archetypesBefore + 1) << "one placement: only the destination archetype is created";
        const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        ASSERT_NE(rec, nullptr);
        EXPECT_EQ(rec->archetype->GetMask(), (Astra::MakeComponentMask<Velocity, Health, Transform, Player>()));
        EXPECT_TRUE(reg.HasComponent<Position>(e));
        EXPECT_EQ(reg.ValidateStorageInvariants(), "");
    }
}

TEST(StorageCreation, AUniversalInThePackStillPlacesTheTableMembersInOnePlacement)
{
    {   // supplied values (CreateEntityWith): moved in, the universal never initialised
        g_initCalls = 0;
        Registry reg(UniversalPositionConfig());
        const size_t before = reg.GetArchetypeCount();
        const Entity e = reg.CreateEntityWith(Position{1.0f, 2.0f, 3.0f}, Velocity{4.0f, 5.0f, 6.0f},
                                              Health{7, 8}, Transform{}, Player{});
        ExpectTablePartPlacedOnce(reg, e, before);
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->y, 2.0f);
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Velocity>(e)->dz, 6.0f);
        EXPECT_EQ(std::as_const(reg).GetComponent<Health>(e)->max, 8);
        EXPECT_EQ(g_initCalls, 0);
        // A Sparse member rides along to its set, as on the all-Table path.
        const Entity withSparse = reg.CreateEntityWith(Velocity{}, Position{}, SparseProbe{9}, Health{}, Transform{}, Player{});
        EXPECT_EQ(reg.GetArchetypeCount(), before + 1);
        EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(withSparse)->v, 9);
    }
    {   // named only (CreateEntity<...>): every Table member default-constructed, the universal default + init
        g_initCalls = 0;
        Registry reg(UniversalPositionConfig());
        const size_t before = reg.GetArchetypeCount();
        const Entity e = reg.CreateEntity<Velocity, Position, Health, Transform, Player>();
        ExpectTablePartPlacedOnce(reg, e, before);
        EXPECT_EQ(std::as_const(reg).GetComponent<Health>(e)->current, 100);
        EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->y, -1.0f);
        EXPECT_EQ(g_initCalls, 1);
    }
    {   // batch, named only (CreateEntities<...>)
        g_initCalls = 0;
        Registry reg(UniversalPositionConfig());
        const size_t before = reg.GetArchetypeCount();
        std::vector<Entity> ents(5);
        ASSERT_EQ((reg.CreateEntities<Velocity, Health, Position, Transform, Player>(5, std::span{ents})), 5u);
        for (const Entity e : ents)
            ExpectTablePartPlacedOnce(reg, e, before);
        EXPECT_EQ(g_initCalls, 5);
    }
    {   // batch, supplied by a generator (CreateEntitiesWith<...>): once per index, in order
        g_initCalls = 0;
        Registry reg(UniversalPositionConfig());
        const size_t before = reg.GetArchetypeCount();
        std::vector<Entity> ents(5);
        std::vector<size_t> calls;
        ASSERT_EQ((reg.CreateEntitiesWith<Velocity, Position, Health, Transform, Player>(5, std::span{ents}, [&](size_t i)
        {
            calls.push_back(i);
            const float f = static_cast<float>(i);
            return std::tuple{Velocity{f, 0.0f, 0.0f}, Position{0.0f, f, 0.0f}, Health{static_cast<int>(i), 50}, Transform{}, Player{}};
        })), 5u);
        EXPECT_EQ(calls, (std::vector<size_t>{0, 1, 2, 3, 4}));
        for (size_t i = 0; i < ents.size(); ++i)
        {
            ExpectTablePartPlacedOnce(reg, ents[i], before);
            EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Velocity>(ents[i])->dx, static_cast<float>(i));
            EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(ents[i])->y, static_cast<float>(i));
            EXPECT_EQ(std::as_const(reg).GetComponent<Health>(ents[i])->current, static_cast<int>(i));
        }
        EXPECT_EQ(g_initCalls, 0);
    }
}

// The same creates keep today's signal order and payloads: EntityCreated, then the pack's
// ComponentAdded in pack order (Table members' payload is their chunk slot), then the
// universal's (Position) last.
TEST(StorageCreation, AUniversalInThePackKeepsTheSignalOrderAndPayloads)
{
    Registry reg(UniversalPositionConfig());
    reg.EnableSignals(Astra::Signal::EntityCreated | Astra::Signal::ComponentAdded);
    std::vector<Astra::ComponentID> ids;
    int created = 0;
    bool payloadsAreTheComponents = true;
    auto* signals = reg.GetSignalManager();
    auto onCreated = signals->On<Astra::Events::EntityCreated>().Register(
        [&](const Astra::Events::EntityCreated&) { EXPECT_TRUE(ids.empty()); ++created; });
    auto onAdded = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev)
        {
            ids.push_back(ev.componentId);
            if (ev.componentId == Astra::TypeID<Health>::Value())
                payloadsAreTheComponents = payloadsAreTheComponents && ev.component == std::as_const(reg).GetComponent<Health>(ev.entity);
            if (ev.componentId == Astra::TypeID<Position>::Value())
                payloadsAreTheComponents = payloadsAreTheComponents && ev.component == std::as_const(reg).GetComponent<Position>(ev.entity);
        });
    (void)reg.CreateEntityWith(Velocity{}, Position{}, Health{3, 4});
    EXPECT_EQ(created, 1);
    EXPECT_EQ(ids, (std::vector<Astra::ComponentID>{Astra::TypeID<Velocity>::Value(),
                                                    Astra::TypeID<Health>::Value(),
                                                    Astra::TypeID<Position>::Value()}));
    EXPECT_TRUE(payloadsAreTheComponents);
    signals->On<Astra::Events::EntityCreated>().Unregister(onCreated);
    signals->On<Astra::Events::ComponentAdded>().Unregister(onAdded);
}

TEST(StorageCreation, ComponentAddedCoversSparseMembersInPackOrder)
{
    Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentAdded);
    std::vector<Astra::ComponentID> ids;
    int seenValue = -1;
    auto* signals = reg.GetSignalManager();
    auto handle = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& ev)
        {
            ids.push_back(ev.componentId);
            if (ev.componentId == Astra::TypeID<SparseProbe>::Value())
                seenValue = static_cast<const SparseProbe*>(ev.component)->v;
        });
    (void)reg.CreateEntityWith(Position{}, SparseProbe{5});
    EXPECT_EQ(ids, (std::vector<Astra::ComponentID>{Astra::TypeID<Position>::Value(),
                                                    Astra::TypeID<SparseProbe>::Value()}));
    EXPECT_EQ(seenValue, 5);
    signals->On<Astra::Events::ComponentAdded>().Unregister(handle);
}

TEST(StorageCreation, DestroyClearAndRegistryDestructionDestroySparseValues)
{
    const int base = SparseProbe::s_live;
    {
        Registry reg;
        std::vector<Entity> ents(4);
        ASSERT_EQ((reg.CreateEntities<Position, SparseProbe>(4, std::span{ents})), 4u);
        EXPECT_EQ(SparseProbe::s_live, base + 4);
        reg.DestroyEntity(ents[0]);
        EXPECT_EQ(SparseProbe::s_live, base + 3);
        reg.DestroyEntities(std::span{ents}.subspan(1, 2));
        EXPECT_EQ(SparseProbe::s_live, base + 1);
        reg.Clear();
        EXPECT_EQ(SparseProbe::s_live, base);
        (void)reg.CreateEntity<SparseProbe>();
        EXPECT_EQ(SparseProbe::s_live, base + 1);
    }   // ~Registry
    EXPECT_EQ(SparseProbe::s_live, base);
}

// s7.1 "no silent drops": a value with no column reaching ConstructComponentAt is a bug
// (the Registry diverts Sparse/Universal values first), so it must be loud.
TEST(StorageCreation, ConstructComponentAtRefusesAColumnlessValueLoudly)
{
    Astra::ComponentRegistry components;
    components.RegisterComponents<Position, Velocity>();
    Astra::ArchetypeChunkPool pool;
    Astra::Archetype archetype(Astra::MakeComponentMask<Position>());
    archetype.SetComponentPool(&pool);
    archetype.Initialize({*components.GetComponentDescriptor(Astra::TypeID<Position>::Value())});

    AssertCount count;
    Astra::Testing::ScopedAssertHandler guard(&CountAndContinue, &count);
    const auto location = archetype.AddEntityWith(Entity(1, 1), Position{1.0f, 0.0f, 0.0f}, Velocity{});
    EXPECT_TRUE(location.IsValid());
#if MOSAIC_ASSERTS_ACTIVE
    EXPECT_EQ(count.n, 1);   // Velocity has no column in {Position}: refused loudly
#else
    EXPECT_EQ(count.n, 0);   // Release: compiled out; the value is still written nowhere
#endif
}

// ---- Review Focus 1 (Sparse), registry level: Get/Has ----
// Chunk C extends this suite with Add, Remove and SetEnabled once those route to the set.

TEST(SparseStorageStaleHandle, DestroyRecycleMissesOnGetAndHas)
{
    Registry reg;
    const Entity stale = reg.CreateEntityWith(SparseProbe{7});
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntityWith(SparseProbe{8});
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(stale));
    const SparseProbe* now = std::as_const(reg).GetComponent<SparseProbe>(fresh);
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(now->v, 8);
}

TEST(SparseStorageStaleHandle, RecycledOccupantWithoutTheComponentStillMisses)
{
    // The id's next owner does NOT carry the sparse component: neither handle may see
    // the erased element.
    Registry reg;
    const Entity stale = reg.CreateEntityWith(SparseProbe{7});
    reg.DestroyEntity(stale);
    const Entity fresh = reg.CreateEntity<Velocity>();
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(stale));
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(fresh));
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(fresh), nullptr);
}

TEST(SparseStorageStaleHandle, ClearRecycleMissesOnGetAndHas)
{
    Registry reg;
    const Entity stale = reg.CreateEntityWith(SparseProbe{7});
    reg.Clear();
    const Entity fresh = reg.CreateEntityWith(SparseProbe{8});
    ASSERT_EQ(fresh.GetID(), stale.GetID());
    ASSERT_NE(fresh.GetVersion(), stale.GetVersion());

    EXPECT_EQ(reg.GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(stale), nullptr);
    EXPECT_FALSE(reg.HasComponent<SparseProbe>(stale));
    EXPECT_EQ(std::as_const(reg).GetComponent<SparseProbe>(fresh)->v, 8);
}
