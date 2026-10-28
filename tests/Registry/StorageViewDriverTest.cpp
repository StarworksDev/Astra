#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <atomic>
#include <mutex>
#include <set>
#include <span>
#include <vector>
#include "../Support/TestWorkerPool.hpp"
#include "StorageQueryComponents.hpp"

using namespace Astra::Test;
using namespace Astra::Test::StorageQ;

namespace
{
    using S = std::set<Astra::Entity>;

    template<typename V>
    S Visited(V&& view)
    {
        S s;
        view.ForEach([&](Astra::Entity e, auto&&...) { s.insert(e); });
        return s;
    }

    // 5000 entities over three archetypes; PosS on every 7th (creation order), SpB on every 11th.
    struct World
    {
        Astra::Registry reg;
        std::vector<Astra::Entity> all, posS, spB;
        explicit World(const Astra::Registry::Config& cfg = {}) : reg(cfg)
        {
            for (int i = 0; i < 5000; ++i)
            {
                Astra::Entity e = (i % 3 == 0) ? reg.CreateEntity<Position>()
                                : (i % 3 == 1) ? reg.CreateEntity<Velocity>()
                                               : reg.CreateEntity();
                all.push_back(e);
                if (i % 7 == 0)  { reg.AddComponent(e, PosS{float(i), 0.0f, 0.0f}); posS.push_back(e); }
                if (i % 11 == 0) { reg.AddComponent(e, SpB{i}); spB.push_back(e); }
            }
        }
    };
}

// s6.2: no Table argument => the sparse set drives; order is its dense order.
TEST(StorageViewDriver, SparseDrivenVisitsTheSetInDenseOrder)
{
    World w;
    std::vector<Astra::Entity> order;
    w.reg.CreateView<PosS>().ForEach([&](Astra::Entity e, PosS& s)
    {
        order.push_back(e);
        EXPECT_FLOAT_EQ(s.x, float(7 * (order.size() - 1)));   // the k-th holder was created as entity 7k
    });
    EXPECT_EQ(order, w.posS);                          // no erase yet: dense order == insertion order
    EXPECT_EQ(w.reg.CreateView<const PosS>().Size(), w.posS.size());
}

// The smallest required set drives; the others are checked per entity.
TEST(StorageViewDriver, SmallestRequiredSetDrives)
{
    World w;
    std::vector<Astra::Entity> order, expected;
    for (Astra::Entity e : w.spB)
        if (w.reg.HasComponent<PosS>(e)) expected.push_back(e);
    w.reg.CreateView<const PosS, const SpB>().ForEach([&](Astra::Entity e, const PosS&, const SpB&) { order.push_back(e); });
    EXPECT_EQ(order, expected);                        // SpB (fewer members) drove: its dense order
}

TEST(StorageViewDriver, TableModifiersAndFiltersOnTheSparseDrivenPath)
{
    World w;
    S notVel, optPos, any;
    for (Astra::Entity e : w.posS)
    {
        if (!w.reg.HasComponent<Velocity>(e)) notVel.insert(e);
        if (w.reg.HasComponent<Velocity>(e) || w.reg.HasComponent<SpB>(e)) any.insert(e);
    }
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::Not<Velocity>>()), notVel);
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::Any<Velocity, SpB>>()), any);

    size_t withPos = 0, visits = 0;
    w.reg.CreateView<PosS, Astra::Optional<Position>>().ForEach([&](Astra::Entity e, PosS&, Position* p)
    {
        ++visits;
        EXPECT_EQ(p != nullptr, w.reg.HasComponent<Position>(e));
        withPos += (p != nullptr);
    });
    EXPECT_EQ(visits, w.posS.size());
    EXPECT_GT(withPos, 0u);

    for (size_t k = 0; k < w.posS.size(); k += 2) w.reg.AddComponent(w.posS[k], SpTag{});
    EXPECT_EQ((w.reg.CreateView<PosS, Astra::With<SpTag>>().Size()), (w.posS.size() + 1) / 2);

    for (size_t k = 0; k < 40; ++k) w.reg.AddComponent(w.all[k], SpE{});
    for (size_t k = 0; k < 40; k += 4) ASSERT_TRUE(w.reg.SetEnabled<SpE>(w.all[k], false));
    EXPECT_EQ(Visited(w.reg.CreateView<const SpE>()).size(), 30u);
    EXPECT_EQ(Visited(w.reg.CreateView<Astra::IncludeDisabled<SpE>>()).size(), 40u);
}

TEST(StorageViewDriver, AnEmptyRequiredSetMatchesNothing)
{
    World w;
    size_t n = 0;
    w.reg.CreateView<PosS, SpTag>().ForEach([&](Astra::Entity, PosS&, SpTag&) { ++n; });   // SpTag never added
    EXPECT_EQ(n, 0u);
    EXPECT_EQ((w.reg.CreateView<PosS, SpTag>().Size()), 0u);
}

// s6.5: sparse-driven views partition the dense range into fixed-size work items.
TEST(StorageViewDriver, ParallelSparseDrivenVisitsEachMemberOnce)
{
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    Astra::Registry reg(cfg);
    std::vector<Astra::Entity> es(20000);
    ASSERT_EQ(reg.CreateEntities<Position>(es.size(), std::span{es}), es.size());
    for (size_t i = 0; i < es.size(); i += 5) reg.AddComponent(es[i], PosS{});
    for (size_t i = 0; i < es.size(); i += 5) reg.AddComponent(es[i + 1], PosS{});   // 8000 members

    std::atomic<size_t> visits{0};
    reg.CreateView<PosS>().ParallelForEach([&](Astra::Entity, PosS& s)
    {
        s.y += 1.0f;
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visits.load(), 8000u);
    for (size_t i = 0; i < es.size(); ++i)
        if (reg.HasComponent<PosS>(es[i])) ASSERT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(es[i])->y, 1.0f) << i;
}

// Archetype-driven parallel walk with a Sparse filter and a Universal yield.
TEST(StorageViewDriver, ParallelArchetypeDrivenWithSparseAndUniversal)
{
    auto reg = MakeRegistry<Health>(std::make_shared<Astra::Testing::TestWorkerPool>());
    std::vector<Astra::Entity> es(20000);
    ASSERT_EQ(reg->CreateEntities<Position>(es.size(), std::span{es}), es.size());
    for (size_t i = 0; i < es.size(); i += 2) reg->AddComponent(es[i], PosS{});

    std::atomic<size_t> visits{0};
    reg->CreateView<Position, PosS, Health>().ParallelForEach([&](Astra::Entity, Position&, PosS&, Health& h)
    {
        h.current += 1;
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visits.load(), 10000u);

    std::atomic<size_t> all{0};
    reg->CreateView<Position, Health>().ParallelForEach([&](Astra::Entity, Position&, Health&) { all.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(all.load(), es.size());
    for (size_t i = 0; i < es.size(); ++i)
        ASSERT_EQ(std::as_const(*reg).GetComponent<Health>(es[i])->current, i % 2 == 0 ? 101 : 100) << i;
}

TEST(StorageViewDriver, ParallelForEachWithContextSparseDrivenStampsFlatItemIndices)
{
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    World w(cfg);
    std::mutex mu;
    std::set<uint32_t> items;
    std::atomic<size_t> visits{0};
    const size_t n = w.reg.CreateView<PosS>().ParallelForEachWithContext(
        [&](uint32_t idx) { std::lock_guard lock(mu); items.insert(idx); return idx; },
        [&](Astra::Entity, PosS&, uint32_t) { visits.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(visits.load(), w.posS.size());
    EXPECT_EQ(n, items.size());
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(*items.begin(), 0u);
    EXPECT_EQ(*items.rbegin(), uint32_t(n - 1));
}

// Task 28 (from Task 26 review): the sparse-driven walk is the first caller of
// EntityGroupsPass / EntityHolds, so Any/OneOf get explicit coverage here.
TEST(StorageViewDriver, SparseDrivenOneOfAndAnyGroups)
{
    World w;
    // OneOf<Sparse, Sparse>: exactly one of SpB / SpTag held.
    for (size_t k = 0; k < w.posS.size(); k += 3) w.reg.AddComponent(w.posS[k], SpTag{});
    S oneOfSS, oneOfTable, oneOfAll;
    for (Astra::Entity e : w.posS)
    {
        const bool b = w.reg.HasComponent<SpB>(e), t = w.reg.HasComponent<SpTag>(e);
        const bool v = w.reg.HasComponent<Velocity>(e), p = w.reg.HasComponent<Position>(e);
        if (b != t) oneOfSS.insert(e);
        if ((v ? 1 : 0) + (b ? 1 : 0) == 1) oneOfTable.insert(e);
        if ((v ? 1 : 0) + (p ? 1 : 0) + (b ? 1 : 0) == 1) oneOfAll.insert(e);
    }
    EXPECT_FALSE(oneOfSS.empty());
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::OneOf<SpB, SpTag>>()), oneOfSS);
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::OneOf<Velocity, SpB>>()), oneOfTable);
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::OneOf<Velocity, Position, SpB>>()), oneOfAll);

    S anyTT;
    for (Astra::Entity e : w.posS)
        if (w.reg.HasComponent<Velocity>(e) || w.reg.HasComponent<Position>(e)) anyTT.insert(e);
    EXPECT_EQ(Visited(w.reg.CreateView<PosS, Astra::Any<Velocity, Position>>()), anyTT);
}

TEST(StorageViewDriver, SparseDrivenOneOfWithAUniversalMember)
{
    auto reg = MakeRegistry<Health>();
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 300; ++i)
    {
        Astra::Entity e = (i % 2) ? reg->CreateEntity<Velocity>() : reg->CreateEntity<Position>();
        es.push_back(e);
        reg->AddComponent(e, PosS{});
        if (i % 3 == 0) reg->AddComponent(e, SpB{i});
    }
    // Universal Health is held by every entity: OneOf<Health, SpB> holds exactly-one iff SpB absent.
    S expected;
    for (Astra::Entity e : es) if (!reg->HasComponent<SpB>(e)) expected.insert(e);
    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(Visited(reg->CreateView<PosS, Astra::OneOf<Health, SpB>>()), expected);
}

// Task 28 fix round 1: above MIN_ENTITIES_FOR_PARALLEL the pool-dispatch branch of
// ParallelSparseDrivenWithContext runs (the test above is inline). 20000 entities, PosS on every
// 5th => 4000 members => 4 work items of 1024.
TEST(StorageViewDriver, ParallelForEachWithContextSparseDrivenDispatchesItems)
{
    Astra::Registry::Config cfg;
    cfg.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
    Astra::Registry reg(cfg);
    std::vector<Astra::Entity> members;
    for (int i = 0; i < 20000; ++i)
    {
        Astra::Entity e = reg.CreateEntity<Position>();
        if (i % 5 == 0) { reg.AddComponent(e, PosS{0.0f, 0.0f, 0.0f}); members.push_back(e); }
    }
    ASSERT_EQ(members.size(), 4000u);
    std::mutex mu;
    std::set<uint32_t> items;
    const size_t n = reg.CreateView<PosS>().ParallelForEachWithContext(
        [&](uint32_t idx) { std::lock_guard lock(mu); items.insert(idx); return idx; },
        [&](Astra::Entity, PosS& s, uint32_t) { s.x += 1.0f; });
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(items, (std::set<uint32_t>{0, 1, 2, 3}));
    for (Astra::Entity e : members)
        ASSERT_FLOAT_EQ(std::as_const(reg).GetComponent<PosS>(e)->x, 1.0f);   // exactly once each
}
