#include <gtest/gtest.h>
#include <algorithm>
#include <Astra/Astra.hpp>
#include <map>
#include <set>
#include <span>
#include <vector>
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    template<typename V>
    std::set<Astra::Entity> Visited(V&& view)
    {
        std::set<Astra::Entity> s;
        view.ForEach([&](Astra::Entity e, auto&&...) { s.insert(e); });
        return s;
    }
}

// s6.3: a required Sparse argument filters per entity (present words) and skips every
// chunk without a presence block; Not<Sparse> excludes on the same words.
TEST(StorageViewFilter, RequiredSparseFiltersPerEntityAndSkipsChunks)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> a, b;
    for (int i = 0; i < 300; ++i) a.push_back(reg.CreateEntityWith(Position{float(i), 0, 0}));
    for (int i = 0; i < 300; ++i) b.push_back(reg.CreateEntityWith(Position{float(i), 0, 0}, Velocity{}));
    std::set<Astra::Entity> holders;
    for (int i = 0; i < 300; i += 3)
    {
        ASSERT_TRUE(reg.AddComponent(b[i], PosS{float(i), 1.0f, 2.0f}));
        holders.insert(b[i]);
    }

    auto view = reg.CreateView<Position, PosS>();
    std::map<Astra::Entity, float> seen;
    view.ForEach([&](Astra::Entity e, Position& p, PosS& s)
    {
        seen[e] = s.x;
        EXPECT_FLOAT_EQ(p.x, s.x);
    });
    EXPECT_EQ(seen.size(), holders.size());
    for (Astra::Entity e : holders) EXPECT_EQ(seen.count(e), 1u);
    EXPECT_EQ(view.Size(), holders.size());
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<PosS>>().Size()), 600u - holders.size());
    EXPECT_EQ(Visited(reg.CreateView<Position, Astra::Not<PosS>>()).count(b[0]), 0u);
}

TEST(StorageViewFilter, WritesThroughSparseYieldsPersist)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 64; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), PosS{float(i), 1.0f, 0.0f});
    }
    reg.CreateView<PosS>().ForEach([](PosS& s) { s.y += 10.0f; });
    for (int i = 0; i < 64; ++i) EXPECT_FLOAT_EQ(reg.GetComponent<PosS>(es[i])->y, 11.0f);
}

TEST(StorageViewFilter, SparseTagsFilterAsWithRequiredAndNot)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 100; ++i) es.push_back(reg.CreateEntity<Position>());
    for (int i = 0; i < 100; i += 4) reg.AddComponent(es[i], SpTag{});
    EXPECT_EQ((reg.CreateView<Position, Astra::With<SpTag>>().Size()), 25u);
    size_t n = 0;
    reg.CreateView<Position, SpTag>().ForEach([&](Astra::Entity, Position&, SpTag&) { ++n; });
    EXPECT_EQ(n, 25u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<SpTag>>().Size()), 75u);
}

// The disabled MIRROR (P3) filters an enableable Sparse argument; IncludeDisabled opts out.
TEST(StorageViewFilter, EnableableSparseUsesTheDisabledMirror)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 10; ++i)
    {
        es.push_back(reg.CreateEntity<Position>());
        reg.AddComponent(es.back(), SpE{float(i)});
    }
    ASSERT_TRUE(reg.SetEnabled<SpE>(es[2], false));
    ASSERT_TRUE(reg.SetEnabled<SpE>(es[7], false));

    EXPECT_EQ((reg.CreateView<Position, SpE>().Size()), 8u);
    const auto seen = Visited(reg.CreateView<Position, SpE>());
    EXPECT_EQ(seen.size(), 8u);
    EXPECT_EQ(seen.count(es[2]) + seen.count(es[7]), 0u);
    EXPECT_EQ((reg.CreateView<Position, Astra::IncludeDisabled<SpE>>().Size()), 10u);

    size_t nulls = 0;
    reg.CreateView<Position, Astra::Optional<const SpE>>().ForEach([&](Astra::Entity, Position&, const SpE* s) { nulls += (s == nullptr); });
    EXPECT_EQ(nulls, 2u);
}

// s6.4: Universal elements are fetched by entity index through the page cursor; 3000
// entities span three 1024-slot pages.
TEST(StorageViewFilter, UniversalYieldsAcrossPages)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es(3000);
    ASSERT_EQ(reg->CreateEntities<Position>(es.size(), std::span{es}), es.size());
    for (size_t i = 0; i < es.size(); ++i) reg->GetComponent<Health>(es[i])->current = int(i);

    std::map<Astra::Entity, int> seen;
    reg->CreateView<Position, Health>().ForEach([&](Astra::Entity e, Position&, Health& h)
    {
        seen[e] = h.current;
        h.max = 7;
    });
    ASSERT_EQ(seen.size(), es.size());
    for (size_t i = 0; i < es.size(); ++i)
    {
        EXPECT_EQ(seen[es[i]], int(i));
        EXPECT_EQ(reg->GetComponent<Health>(es[i])->max, 7);
    }
    (void)reg->CreateEntity();
    EXPECT_EQ(reg->CreateView<Health>().Size(), es.size() + 1);
}

// An enableable Universal component filters on the chunk's disabled mirror.
TEST(StorageViewFilter, EnableableUniversalUsesTheDisabledMirror)
{
    auto reg = MakeRegistry<Timer>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 50; ++i) es.push_back(reg->CreateEntity<Position>());
    for (int i = 0; i < 50; i += 5) ASSERT_TRUE(reg->SetEnabled<Timer>(es[i], false));

    EXPECT_EQ((reg->CreateView<Position, Timer>().Size()), 40u);
    const auto seen = Visited(reg->CreateView<Timer>());
    EXPECT_EQ(seen.size(), 40u);
    EXPECT_EQ(seen.count(es[0]), 0u);
    EXPECT_EQ(reg->CreateView<Astra::IncludeDisabled<Timer>>().Size(), 50u);

    size_t nulls = 0;
    reg->CreateView<Position, Astra::Optional<Timer>>().ForEach([&](Astra::Entity, Position&, Timer* t) { nulls += (t == nullptr); });
    EXPECT_EQ(nulls, 10u);
}

TEST(StorageViewFilter, OptionalSparseAndUniversalYieldPointers)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 20; ++i)
    {
        es.push_back(reg->CreateEntity<Position>());
        if (i % 2 == 0) reg->AddComponent(es.back(), PosS{float(i), 0.0f, 0.0f});
    }
    size_t visits = 0;
    reg->CreateView<Position, Astra::Optional<PosS>, Astra::Optional<const Health>>().ForEach(
        [&](Astra::Entity e, Position&, PosS* s, const Health* h)
        {
            ++visits;
            const size_t i = size_t(std::find(es.begin(), es.end(), e) - es.begin());
            EXPECT_EQ(s != nullptr, i % 2 == 0);
            if (s) EXPECT_FLOAT_EQ(s->x, float(i));
            EXPECT_NE(h, nullptr);
        });
    EXPECT_EQ(visits, 20u);
}

// Universal overrides the trait: a Sparse-trait type declared Universal is on every entity.
TEST(StorageViewFilter, UniversalOverridesASparseTrait)
{
    auto reg = MakeRegistry<PosS>();
    for (int i = 0; i < 30; ++i) (void)reg->CreateEntity<Position>();
    size_t n = 0;
    reg->CreateView<Position, PosS>().ForEach([&](Astra::Entity, Position&, PosS& s) { ++n; s.z = 3.0f; });
    EXPECT_EQ(n, 30u);
    EXPECT_EQ((reg->CreateView<Position, PosS>().Size()), 30u);
}

// RULINGS (Task 24): a Mut<T> over a Sparse or Universal element writes THROUGH to the
// store's own tick (no scratch slot): the written entity reads Changed, its neighbour not.
TEST(StorageViewFilter, MutOverTrackedSparseAndUniversalMarksTheStoresTicks)
{
    auto reg = MakeRegistry<TrackedPos>();
    const Astra::Entity a = reg->CreateEntity<Position>();
    const Astra::Entity b = reg->CreateEntity<Position>();
    ASSERT_TRUE(reg->AddComponent(a, SpE{1.0f}));
    ASSERT_TRUE(reg->AddComponent(b, SpE{2.0f}));
    reg->AdvanceTick();
    const Astra::Tick since = reg->CurrentTick();
    reg->AdvanceTick();

    reg->CreateView<Position, SpE, TrackedPos>().ForEach([&](Astra::Entity e, Position&, auto& s, auto& t)
    {
        if (e == a)
        {
            s.Write().v = 9.0f;
            t.Write().x = 5.0f;
        }
    });
    EXPECT_FLOAT_EQ(reg->GetComponent<SpE>(a)->v, 9.0f);
    EXPECT_FLOAT_EQ(reg->GetComponent<TrackedPos>(a)->x, 5.0f);
    EXPECT_TRUE(reg->IsChanged<SpE>(a, since));
    EXPECT_FALSE(reg->IsChanged<SpE>(b, since));
    EXPECT_TRUE(reg->IsChanged<TrackedPos>(a, since));
    EXPECT_FALSE(reg->IsChanged<TrackedPos>(b, since));
}

// Several Optional + required Sparse/Universal/Table arguments in one view, and a view
// that names a Sparse type no entity holds yet (no set exists: reads never create one).
TEST(StorageViewFilter, AbsentSparseTypeMatchesNothingAndCreatesNoStore)
{
    Astra::Registry reg;
    for (int i = 0; i < 10; ++i) (void)reg.CreateEntity<Position>();
    EXPECT_EQ((reg.CreateView<Position, PosS>().Size()), 0u);
    EXPECT_EQ((reg.CreateView<Position, Astra::Not<PosS>>().Size()), 10u);
    size_t n = 0;
    reg.CreateView<Position, PosS>().ForEach([&](Astra::Entity, Position&, PosS&) { ++n; });
    reg.CreateView<Position, Astra::Optional<PosS>>().ForEach([&](Astra::Entity, Position&, PosS* s) { n += (s == nullptr) ? 100 : 0; });
    EXPECT_EQ(n, 1000u);
    EXPECT_EQ(reg.GetComponentStorage()->FindSparse(Astra::TypeID<PosS>::Value()), nullptr);
}

// The page cursor re-tests the slot's live bit and handle on EVERY fetch (RULINGS Task 24):
// a cursor primed on a live neighbour must not hand out a dead or stale slot of the page.
TEST(StorageViewFilter, UniversalPageCursorRetestsLiveness)
{
    auto reg = MakeRegistry<Health>();
    const Astra::Entity a = reg->CreateEntity<Position>();
    const Astra::Entity b = reg->CreateEntity<Position>();
    const Astra::Entity c = reg->CreateEntity<Position>();
    Astra::UniversalColumn* col = reg->GetComponentStorage()->FindUniversal(Astra::TypeID<Health>::Value());
    ASSERT_NE(col, nullptr);

    Astra::UniversalColumn::Cursor cur;
    EXPECT_EQ(col->GetCached(a, cur), col->Get(a));
    EXPECT_EQ(col->GetCached(b, cur), col->Get(b));
    EXPECT_NE(col->GetCached(b, cur), nullptr);

    reg->DestroyEntity(b);
    EXPECT_EQ(col->GetCached(b, cur), nullptr);               // dead slot, same page
    EXPECT_EQ(col->GetCached(c, cur), col->Get(c));           // neighbours unaffected
    const Astra::Entity b2 = reg->CreateEntity<Position>();   // recycles b's id with a new version
    EXPECT_NE(col->GetCached(b2, cur), nullptr);
    EXPECT_EQ(col->GetCached(b, cur), nullptr);               // the stale handle still misses
}

// s4.3 memory safety: a callback that destroys every entity of a single-page chunk frees the
// Universal page the walk's cursor is sitting on. The walk must finish without touching it.
TEST(StorageViewFilter, UniversalWalkSurvivesDestroyingTheWholePage)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i) es.push_back(reg->CreateEntity<Position>());

    size_t visits = 0;
    reg->CreateView<Position, Health>().ForEach([&](Astra::Entity, Position&, Health&)
    {
        if (++visits == 1)
            for (Astra::Entity e : es) reg->DestroyEntity(e);
    });
    EXPECT_EQ(visits, 1u);
    EXPECT_EQ((reg->CreateView<Position, Health>().Size()), 0u);
    size_t after = 0;
    reg->CreateView<Position, Health>().ForEach([&](Astra::Entity, Position&, Health&) { ++after; });
    EXPECT_EQ(after, 0u);
    EXPECT_TRUE(reg->ValidateStorageInvariants().empty());
}

// Same, but the callback also recycles ids (create after destroy), so a fresh page may take
// the freed page's index while the walk still holds the old one.
TEST(StorageViewFilter, UniversalWalkSurvivesDestroyThenCreateInTheCallback)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i) es.push_back(reg->CreateEntity<Position>());

    size_t visits = 0;
    reg->CreateView<Position, Health>().ForEach([&](Astra::Entity, Position&, Health&)
    {
        if (++visits == 1)
        {
            for (Astra::Entity e : es) reg->DestroyEntity(e);
            for (int i = 0; i < 8; ++i) reg->CreateEntity<Position>();
        }
    });
    EXPECT_GE(visits, 1u);
    EXPECT_EQ((reg->CreateView<Position, Health>().Size()), 8u);
    size_t after = 0;
    reg->CreateView<Position, Health>().ForEach([&](Astra::Entity, Position&, Health&) { ++after; });
    EXPECT_EQ(after, 8u);
    EXPECT_TRUE(reg->ValidateStorageInvariants().empty());
}
