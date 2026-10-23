#include <Astra/Astra.hpp>
#include <Support/TestWorkerPool.hpp>  // reference pool via the "tests" includedir
#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
    // Shared pool for all parallel benchmarks. Astra itself creates no
    // threads; benchmarks inject the test-support reference scheduler.
    std::shared_ptr<Astra::IWorkScheduler> BenchPool()
    {
        static auto s_pool = std::make_shared<Astra::Testing::TestWorkerPool>();
        return s_pool;
    }
}

struct Position
{
    std::uint64_t x;
    std::uint64_t y;
};

struct StablePosition
{
    std::uint64_t x;
    std::uint64_t y;
};

struct Velocity
{
    std::uint64_t x;
    std::uint64_t y;
};

template<int N>
struct Comp
{
    int data[N + 1];
};

template<int N>
struct Comp2
{
    int data;
};

struct MoveSystem : Astra::SystemTraits<Astra::Reads<Velocity>, Astra::Writes<Position>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Position, Velocity>();
        view.ForEach([](Astra::Entity, Position& pos, Velocity& vel)
        {
            pos.x += vel.x;
            pos.y += vel.y;
        });
    }
};

struct BoundsCheckSystem : Astra::SystemTraits<Astra::Reads<Position>, Astra::Writes<Position>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Position>();
        view.ForEach([](Astra::Entity, Position& pos)
        {
            if (pos.x > 1000) pos.x = 0;
            if (pos.y > 1000) pos.y = 0;
        });
    }
};

struct SpecialProcessingSystem : Astra::SystemTraits<Astra::Reads<Position>, Astra::Writes<Comp<0>>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<const Position, Comp<0>>();
        view.ForEach([](Astra::Entity, const Position& pos, Comp<0>& c)
        {
            c.data[0] = static_cast<int>(pos.x + pos.y);
        });
    }
};

struct PhysicsSystem : Astra::SystemTraits<Astra::Reads<Velocity>, Astra::Writes<Position>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Position, const Velocity>();
        view.ForEach([](Astra::Entity, Position& pos, const Velocity& vel)
        {
            pos.x += vel.x;
            pos.y += vel.y;
        });
    }
};

struct Comp0ProcessingSystem : Astra::SystemTraits<Astra::Writes<Comp<0>>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Comp<0>>();
        view.ForEach([](Astra::Entity, Comp<0>& c)
        {
            c.data[0] = c.data[0] * 2 + 1;
        });
    }
};

struct Comp1ProcessingSystem : Astra::SystemTraits<Astra::Writes<Comp<1>>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Comp<1>>();
        view.ForEach([](Astra::Entity, Comp<1>& c)
        {
            c.data[0] = c.data[0] * 3 - 1;
            c.data[1] = c.data[1] * 4 + 2;
        });
    }
};

struct Comp2ProcessingSystem : Astra::SystemTraits<Astra::Writes<Comp<2>>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Comp<2>>();
        view.ForEach([](Astra::Entity, Comp<2>& c)
        {
            c.data[0] = c.data[0] * 3 - 1;
            c.data[1] = c.data[1] * 4 + 2;
            c.data[2] = c.data[2] * 5 / 3;
        });
    }
};

template<int N>
struct ChainSystem : Astra::SystemTraits<Astra::Reads<Position>, Astra::Writes<Position>>
{
    void operator()(Astra::Registry& registry)
    {
        auto view = registry.CreateView<Position>();
        view.ForEach([](Astra::Entity, Position& pos)
        {
            benchmark::DoNotOptimize(pos.x += N);
        });
    }
};

struct BenchmarkExecutor : Astra::ISystemExecutor
{
    void Execute(const Astra::SystemExecutionContext& context) override
    {
        for (const auto& group : context.parallelGroups)
        {
            for (size_t index : group)
            {
                context.systems[index](*context.registry);
            }
        }
    }
};

// Tests entity creation performance
static void BM_CreateEntities(benchmark::State& state)
{
    const size_t count = state.range(0);
    
    for (auto _ : state)
    {
        Astra::Registry registry;
        for (size_t i = 0; i < count; ++i)
        {
            benchmark::DoNotOptimize(registry.CreateEntity());
        }
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests batch entity creation performance
static void BM_CreateEntitiesBatch(benchmark::State& state)
{
    const size_t count = state.range(0);
    std::vector<Astra::Entity> entities(count);
    
    for (auto _ : state)
    {
        Astra::Registry registry;
        registry.CreateEntitiesWith(count, entities, [](size_t)
        { 
            return std::tuple<>(); 
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests adding components to existing entities one by one
static void BM_AddComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    
    for (auto _ : state)
    {
        state.PauseTiming();
        Astra::Registry registry;
        std::vector<Astra::Entity> entities;
        for (size_t i = 0; i < count; ++i)
        {
            entities.push_back(registry.CreateEntity());
        }
        state.ResumeTiming();
        
        for (auto entity : entities)
        {
            registry.EmplaceComponent<Position>(entity);
            registry.EmplaceComponent<Velocity>(entity);
        }
    }
    
    state.SetItemsProcessed(state.iterations() * count * 2);
}

// Tests removing components from entities one by one
static void BM_RemoveComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    
    for (auto _ : state)
    {
        state.PauseTiming();
        Astra::Registry registry;
        std::vector<Astra::Entity> entities;
        for (size_t i = 0; i < count; ++i)
        {
            auto e = registry.CreateEntity();
            registry.EmplaceComponent<Position>(e);
            entities.push_back(e);
        }
        state.ResumeTiming();
        
        for (auto entity : entities)
        {
            registry.RemoveComponent<Position>(entity);
        }
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests batch adding components to multiple entities at once
static void BM_AddComponentsBatch(benchmark::State& state)
{
    const size_t count = state.range(0);
    
    for (auto _ : state)
    {
        state.PauseTiming();
        Astra::Registry registry;
        std::vector<Astra::Entity> entities;
        entities.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            entities.push_back(registry.CreateEntity());
        }
        state.ResumeTiming();
        
        registry.AddComponents<Position>(entities, Position{42, 42});
        registry.AddComponents<Velocity>(entities, Velocity{10, 10});
    }
    
    state.SetItemsProcessed(state.iterations() * count * 2);
}

// Tests batch removing components from multiple entities at once
static void BM_RemoveComponentsBatch(benchmark::State& state)
{
    const size_t count = state.range(0);
    
    for (auto _ : state)
    {
        state.PauseTiming();
        Astra::Registry registry;
        std::vector<Astra::Entity> entities;
        entities.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            auto e = registry.CreateEntity();
            registry.EmplaceComponent<Position>(e, Position{42, 42});
            entities.push_back(e);
        }
        state.ResumeTiming();
        
        size_t removed = registry.RemoveComponents<Position>(entities);
        benchmark::DoNotOptimize(removed);
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests iteration over entities with a single component
static void BM_IterateSingleComponent(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
    }
    
    auto view = registry.CreateView<Position>();
    
    for (auto _ : state)
    {
        view.ForEach([](Astra::Entity, Position& pos)
        {
            benchmark::DoNotOptimize(pos.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests iteration over entities with two components
static void BM_IterateTwoComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
    }
    
    auto view = registry.CreateView<Position, Velocity>();
    
    for (auto _ : state)
    {
        view.ForEach([](Astra::Entity, Position& pos, Velocity& vel)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests iteration when only half of entities match the query
static void BM_IterateTwoComponentsHalf(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Velocity>(entity);
        
        if (i % 2)
        {
            registry.EmplaceComponent<Position>(entity);
        }
    }
    
    auto view = registry.CreateView<Position, Velocity>();
    
    size_t matched = 0;
    for (auto _ : state)
    {
        matched = 0;
        view.ForEach([&matched](Astra::Entity, Position& pos, Velocity& vel)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            ++matched;
        });
    }
    
    state.SetItemsProcessed(state.iterations() * matched);
}

// Tests iteration when only one entity matches the query
static void BM_IterateTwoComponentsOne(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Velocity>(entity);
        
        if (i == count / 2)
        {
            registry.EmplaceComponent<Position>(entity);
        }
    }
    
    auto view = registry.CreateView<Position, Velocity>();
    
    for (auto _ : state)
    {
        view.ForEach([](Astra::Entity, Position& pos, Velocity& vel)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * 1);
}

// Tests iteration over entities with three components
static void BM_IterateThreeComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        registry.EmplaceComponent<Comp<0>>(entity);
    }
    
    auto view = registry.CreateView<Position, Velocity, Comp<0>>();
    
    for (auto _ : state)
    {
        view.ForEach([](Astra::Entity, Position& pos, Velocity& vel, Comp<0>& c)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            benchmark::DoNotOptimize(c.data[0] = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests iteration over entities with five components
static void BM_IterateFiveComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        registry.EmplaceComponent<Comp<0>>(entity);
        registry.EmplaceComponent<Comp<1>>(entity);
        registry.EmplaceComponent<Comp<2>>(entity);
    }

    auto view = registry.CreateView<Position, Velocity, Comp<0>, Comp<1>, Comp<2>>();

    for (auto _ : state)
    {
        view.ForEach([](Astra::Entity, Position& pos, Velocity& vel, Comp<0>& c0, Comp<1>& c1, Comp<2>& c2)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            benchmark::DoNotOptimize(c0.data[0] = 0);
            benchmark::DoNotOptimize(c1.data[0] = 0);
            benchmark::DoNotOptimize(c1.data[1] = 0);
            benchmark::DoNotOptimize(c2.data[0] = 0);
            benchmark::DoNotOptimize(c2.data[1] = 0);
            benchmark::DoNotOptimize(c2.data[2] = 0);
        });
    }

    state.SetItemsProcessed(state.iterations() * count);
}

// ============================================================================
// Range-based for loop iterator benchmarks (compare against ForEach above)
// ============================================================================

// Tests range-based for loop iteration with a single component
static void BM_RangeForSingleComponent(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
    }

    auto view = registry.CreateView<Position>();

    for (auto _ : state)
    {
        for (auto [entity, pos] : view)
        {
            benchmark::DoNotOptimize(pos.x = 0);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
}

// Tests range-based for loop iteration with two components
static void BM_RangeForTwoComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
    }

    auto view = registry.CreateView<Position, Velocity>();

    for (auto _ : state)
    {
        for (auto [entity, pos, vel] : view)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
}

// Tests range-based for loop iteration when only half match
static void BM_RangeForTwoComponentsHalf(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Velocity>(entity);

        if (i % 2)
        {
            registry.EmplaceComponent<Position>(entity);
        }
    }

    auto view = registry.CreateView<Position, Velocity>();

    size_t matched = 0;
    for (auto _ : state)
    {
        matched = 0;
        for (auto [entity, pos, vel] : view)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            ++matched;
        }
    }

    state.SetItemsProcessed(state.iterations() * matched);
}

// Tests range-based for loop iteration with three components
static void BM_RangeForThreeComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        registry.EmplaceComponent<Comp<0>>(entity);
    }

    auto view = registry.CreateView<Position, Velocity, Comp<0>>();

    for (auto _ : state)
    {
        for (auto [entity, pos, vel, c] : view)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            benchmark::DoNotOptimize(c.data[0] = 0);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
}

// Tests range-based for loop iteration with five components
static void BM_RangeForFiveComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        registry.EmplaceComponent<Comp<0>>(entity);
        registry.EmplaceComponent<Comp<1>>(entity);
        registry.EmplaceComponent<Comp<2>>(entity);
    }

    auto view = registry.CreateView<Position, Velocity, Comp<0>, Comp<1>, Comp<2>>();

    for (auto _ : state)
    {
        for (auto [entity, pos, vel, c0, c1, c2] : view)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            benchmark::DoNotOptimize(c0.data[0] = 0);
            benchmark::DoNotOptimize(c1.data[0] = 0);
            benchmark::DoNotOptimize(c1.data[1] = 0);
            benchmark::DoNotOptimize(c2.data[0] = 0);
            benchmark::DoNotOptimize(c2.data[1] = 0);
            benchmark::DoNotOptimize(c2.data[2] = 0);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
}

// Tests parallel iteration over entities with a single component
static void BM_ParallelIterateSingleComponent(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
    }
    
    auto view = registry.CreateView<Position>();
    
    for (auto _ : state)
    {
        view.ParallelForEach([](Astra::Entity, Position& pos)
        {
            benchmark::DoNotOptimize(pos.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests parallel iteration over entities with two components
static void BM_ParallelIterateTwoComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
    }
    
    auto view = registry.CreateView<Position, Velocity>();
    
    for (auto _ : state)
    {
        view.ParallelForEach([](Astra::Entity, Position& pos, Velocity& vel)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests parallel iteration when only half of entities match
static void BM_ParallelIterateTwoComponentsHalf(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Velocity>(entity);

        if (i % 2)
        {
            registry.EmplaceComponent<Position>(entity);
        }
    }
    
    auto view = registry.CreateView<Position, Velocity>();
    
    size_t matched = count / 2;  // Approximately half match
    for (auto _ : state)
    {
        view.ParallelForEach([](Astra::Entity, Position& pos, Velocity& vel)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * matched);
}

// Tests parallel iteration when only one entity matches
static void BM_ParallelIterateTwoComponentsOne(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Velocity>(entity);

        if (i == count / 2)
        {
            registry.EmplaceComponent<Position>(entity);
        }
    }
    
    auto view = registry.CreateView<Position, Velocity>();
    
    for (auto _ : state)
    {
        view.ParallelForEach([](Astra::Entity, Position& pos, Velocity& vel)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * 1);
}

// Tests parallel iteration over entities with three components
static void BM_ParallelIterateThreeComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        registry.EmplaceComponent<Comp<0>>(entity);
    }
    
    auto view = registry.CreateView<Position, Velocity, Comp<0>>();
    
    for (auto _ : state)
    {
        view.ParallelForEach([](Astra::Entity, Position& pos, Velocity& vel, Comp<0>& c)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            benchmark::DoNotOptimize(c.data[0] = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests parallel iteration over entities with five components
static void BM_ParallelIterateFiveComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        registry.EmplaceComponent<Comp<0>>(entity);
        registry.EmplaceComponent<Comp<1>>(entity);
        registry.EmplaceComponent<Comp<2>>(entity);
    }
    
    auto view = registry.CreateView<Position, Velocity, Comp<0>, Comp<1>, Comp<2>>();
    
    for (auto _ : state)
    {
        view.ParallelForEach([](Astra::Entity, Position& pos, Velocity& vel, Comp<0>& c0, Comp<1>& c1, Comp<2>& c2)
        {
            benchmark::DoNotOptimize(pos.x = 0);
            benchmark::DoNotOptimize(vel.x = 0);
            benchmark::DoNotOptimize(c0.data[0] = 0);
            benchmark::DoNotOptimize(c1.data[0] = 0);
            benchmark::DoNotOptimize(c1.data[1] = 0);
            benchmark::DoNotOptimize(c2.data[0] = 0);
            benchmark::DoNotOptimize(c2.data[1] = 0);
            benchmark::DoNotOptimize(c2.data[2] = 0);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests random access to a single component
static void BM_GetComponent(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    std::vector<Astra::Entity> entities;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        entities.push_back(entity);
    }
    
    for (auto _ : state)
    {
        for (auto entity : entities)
        {
            auto* pos = registry.GetComponent<Position>(entity);
            benchmark::DoNotOptimize(pos->x = 0);
        }
    }
    
    state.SetItemsProcessed(state.iterations() * count);
}

// Tests random access to multiple components
static void BM_GetMultipleComponents(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;
    std::vector<Astra::Entity> entities;
    
    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
        entities.push_back(entity);
    }
    
    for (auto _ : state)
    {
        for (auto entity : entities)
        {
            auto* pos = registry.GetComponent<Position>(entity);
            auto* vel = registry.GetComponent<Velocity>(entity);
            benchmark::DoNotOptimize(pos->x = 0);
            benchmark::DoNotOptimize(vel->y = 0);
        }
    }
    
    state.SetItemsProcessed(state.iterations() * count * 2);
}

// Tests iteration over direct children in a hierarchy
static void BM_ForEachChild(benchmark::State& state)
{
    const size_t targetCount = state.range(0);
    
    Astra::Registry registry;
    
    auto root = registry.CreateEntity();
    registry.EmplaceComponent<Position>(root);
    
    std::vector<Astra::Entity> children;
    for (size_t i = 0; i < targetCount; ++i)
    {
        auto child = registry.CreateEntity();
        registry.EmplaceComponent<Position>(child);
        registry.SetParent(child, root);
        children.push_back(child);
    }
    
    for (auto _ : state)
    {
        auto relations = registry.GetRelations<Position>(root);
        size_t count = 0;
        relations.ForEachChild([&count](Astra::Entity, Position&)
        {
            benchmark::DoNotOptimize(count++);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * targetCount);
}

// Tests iteration over all descendants in a tree hierarchy
static void BM_ForEachDescendant(benchmark::State& state)
{
    const size_t targetCount = state.range(0);
    
    size_t depth = 4;
    size_t branching = 4;
    
    if (targetCount <= 1000)
    {
        depth = 4;
        branching = 4;
    }
    else if (targetCount <= 10000)
    {
        depth = 5;
        branching = 5;
    }
    else
    {
        depth = 6;
        branching = 6;
    }
    
    Astra::Registry registry;
    std::vector<Astra::Entity> allEntities;
    
    std::queue<std::pair<Astra::Entity, size_t>> toProcess;
    auto root = registry.CreateEntity();
    registry.EmplaceComponent<Position>(root);
    allEntities.push_back(root);
    toProcess.push({root, 0});
    
    while (!toProcess.empty() && allEntities.size() < targetCount)
    {
        auto [parent, currentDepth] = toProcess.front();
        toProcess.pop();
        
        if (currentDepth < depth)
        {
            for (size_t i = 0; i < branching && allEntities.size() < targetCount; ++i) {
                auto child = registry.CreateEntity();
                registry.EmplaceComponent<Position>(child);
                registry.SetParent(child, parent);
                allEntities.push_back(child);
                toProcess.push({child, currentDepth + 1});
            }
        }
    }
    
    for (auto _ : state)
    {
        auto relations = registry.GetRelations<Position>(root);
        size_t count = 0;
        relations.ForEachDescendant([&count](Astra::Entity, size_t, Position&)
        {
            benchmark::DoNotOptimize(count++);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * allEntities.size());
}

// Tests iteration over ancestors from leaf to root
static void BM_ForEachAncestor(benchmark::State& state)
{
    const size_t targetCount = state.range(0);
    
    Astra::Registry registry;
    
    Astra::Entity current = registry.CreateEntity();
    registry.EmplaceComponent<Position>(current);
    
    std::vector<Astra::Entity> ancestors;
    ancestors.push_back(current);
    
    for (size_t i = 1; i < targetCount; ++i)
    {
        auto parent = registry.CreateEntity();
        registry.EmplaceComponent<Position>(parent);
        registry.SetParent(current, parent);
        ancestors.push_back(parent);
        current = parent;
    }
    
    auto leaf = ancestors[0];
    
    for (auto _ : state)
    {
        auto relations = registry.GetRelations<Position>(leaf);
        size_t count = 0;
        relations.ForEachAncestor([&count](Astra::Entity, size_t, Position&)
        {
            benchmark::DoNotOptimize(count++);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * (targetCount - 1));
}

// Tests iteration over linked entities
static void BM_ForEachLink(benchmark::State& state)
{
    const size_t targetCount = state.range(0);
    
    Astra::Registry registry;
    
    auto hub = registry.CreateEntity();
    registry.EmplaceComponent<Position>(hub);
    
    std::vector<Astra::Entity> linked;
    for (size_t i = 0; i < targetCount; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.AddLink(hub, entity);
        linked.push_back(entity);
    }
    
    for (auto _ : state)
    {
        auto relations = registry.GetRelations<Position>(hub);
        size_t count = 0;
        relations.ForEachLink([&count](Astra::Entity, Position&)
        {
            benchmark::DoNotOptimize(count++);
        });
    }
    
    state.SetItemsProcessed(state.iterations() * targetCount);
}

// Tests parallel iteration over all descendants in a tree
static void BM_ParallelForEachDescendant(benchmark::State& state)
{
    const size_t targetCount = state.range(0);
    
    size_t depth = 4;
    size_t branching = 4;
    
    if (targetCount <= 1000)
    {
        depth = 4;
        branching = 4;
    }
    else if (targetCount <= 10000)
    {
        depth = 5;
        branching = 5;
    }
    else
    {
        depth = 6;
        branching = 6;
    }
    
    Astra::Registry::Config config;
    config.workScheduler = BenchPool();
    Astra::Registry registry(config);
    std::vector<Astra::Entity> allEntities;

    std::queue<std::pair<Astra::Entity, size_t>> toProcess;
    auto root = registry.CreateEntity();
    registry.EmplaceComponent<Position>(root);
    allEntities.push_back(root);
    toProcess.push({root, 0});

    while (!toProcess.empty() && allEntities.size() < targetCount)
    {
        auto [parent, currentDepth] = toProcess.front();
        toProcess.pop();

        if (currentDepth < depth)
        {
            for (size_t i = 0; i < branching && allEntities.size() < targetCount; ++i) {
                auto child = registry.CreateEntity();
                registry.EmplaceComponent<Position>(child);
                registry.SetParent(child, parent);
                allEntities.push_back(child);
                toProcess.push({child, currentDepth + 1});
            }
        }
    }

    for (auto _ : state)
    {
        auto relations = registry.GetRelations<Position>(root);
        std::atomic<size_t> count{0};
        relations.ParallelForEachDescendant([&count](Astra::Entity, size_t, Position&)
        {
            benchmark::DoNotOptimize(count.fetch_add(1));
        });
    }

    state.SetItemsProcessed(state.iterations() * allEntities.size());
}

// Tests sequential system execution performance
static void BM_SystemScheduler_Sequential(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity, Position{i, i});
        registry.EmplaceComponent<Velocity>(entity, Velocity{1, 1});
        if (i % 3 == 0)
        {
            registry.EmplaceComponent<Comp<0>>(entity);
        }
    }

    Astra::SystemScheduler scheduler;

    (void)scheduler.AddSystem<MoveSystem>();
    (void)scheduler.AddSystem<BoundsCheckSystem>();
    (void)scheduler.AddSystem<SpecialProcessingSystem>();

    for (auto _ : state)
    {
        scheduler.Execute(registry);
    }

    state.SetItemsProcessed(state.iterations() * count * 3);  // 3 systems
}

// Tests lambda-based system execution
static void BM_SystemScheduler_Lambda(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity, Position{i, i});
        registry.EmplaceComponent<Velocity>(entity, Velocity{1, 1});
    }

    Astra::SystemScheduler scheduler;

    (void)scheduler.AddSystem([](Astra::Entity e, const Velocity& vel, Position& pos)
    {
        pos.x += vel.x;
        pos.y += vel.y;
    });  // Auto-detects: Reads<Velocity>, Writes<Position>

    (void)scheduler.AddSystem([](Astra::Entity e, Position& pos)
    {
        if (pos.x > 1000) pos.x = 0;
        if (pos.y > 1000) pos.y = 0;
    });  // Auto-detects: Writes<Position>

    for (auto _ : state)
    {
        scheduler.Execute(registry);
    }

    state.SetItemsProcessed(state.iterations() * count * 2);  // 2 systems
}

// Tests parallel system execution with dependencies
static void BM_SystemScheduler_Parallel(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity, Position{i, i});
        registry.EmplaceComponent<Velocity>(entity, Velocity{1, 1});
        if (i % 2 == 0)
        {
            registry.EmplaceComponent<Comp<0>>(entity);
        }
        if (i % 3 == 0)
        {
            registry.EmplaceComponent<Comp<1>>(entity);
        }
    }

    Astra::SystemScheduler scheduler;

    (void)scheduler.AddSystem<PhysicsSystem>();         // Auto-detects: Reads<Velocity>, Writes<Position>
    (void)scheduler.AddSystem<Comp0ProcessingSystem>(); // Auto-detects: Writes<Comp<0>>
    (void)scheduler.AddSystem<Comp1ProcessingSystem>(); // Auto-detects: Writes<Comp<1>>

    // This system depends on Position from Physics
    (void)scheduler.AddSystem<BoundsCheckSystem>();     // Auto-detects: Reads<Position>, Writes<Position>

    Astra::ParallelExecutor executor(BenchPool());

    for (auto _ : state)
    {
        scheduler.Execute(registry, &executor);
    }

    state.SetItemsProcessed(state.iterations() * count * 4);  // 4 systems
}

// Tests execution of many independent systems
static void BM_SystemScheduler_ManyIndependent(benchmark::State& state)
{
    Astra::Registry registry;

    for (size_t i = 0; i < 10000; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        if (i % 2 == 0) registry.EmplaceComponent<Velocity>(entity);
        if (i % 3 == 0) registry.EmplaceComponent<Comp<0>>(entity);
        if (i % 4 == 0) registry.EmplaceComponent<Comp<1>>(entity);
        if (i % 5 == 0) registry.EmplaceComponent<Comp<2>>(entity);
    }

    Astra::SystemScheduler scheduler;

    (void)scheduler.AddSystem<MoveSystem>();          // Auto-detects: Reads<Velocity>, Writes<Position>
    (void)scheduler.AddSystem<PhysicsSystem>();       // Auto-detects: Reads<Velocity>, Writes<Position>
    (void)scheduler.AddSystem<Comp0ProcessingSystem>(); // Auto-detects: Writes<Comp<0>>
    (void)scheduler.AddSystem<Comp1ProcessingSystem>(); // Auto-detects: Writes<Comp<1>>
    (void)scheduler.AddSystem<Comp2ProcessingSystem>(); // Auto-detects: Writes<Comp<2>>

    for (auto _ : state)
    {
        scheduler.Execute(registry);
    }

    state.SetItemsProcessed(state.iterations() * 5);  // 5 systems
}

// Tests execution of systems with complex dependency chains
static void BM_SystemScheduler_WithDependencies(benchmark::State& state)
{
    Astra::Registry registry;

    // Create entities
    for (size_t i = 0; i < 10000; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity, Position{i, i});
    }

    Astra::SystemScheduler scheduler;

    (void)scheduler.AddSystem<ChainSystem<0>>();
    (void)scheduler.AddSystem<ChainSystem<1>>();
    (void)scheduler.AddSystem<ChainSystem<2>>();
    (void)scheduler.AddSystem<ChainSystem<3>>();
    (void)scheduler.AddSystem<ChainSystem<4>>();

    for (auto _ : state)
    {
        scheduler.Execute(registry);
    }

    state.SetItemsProcessed(state.iterations() * 5 * 10000);  // 5 systems, 10000 entities
}

// Tests custom executor implementation performance
static void BM_SystemScheduler_CustomExecutor(benchmark::State& state)
{
    const size_t count = state.range(0);
    Astra::Registry registry;

    for (size_t i = 0; i < count; ++i)
    {
        auto entity = registry.CreateEntity();
        registry.EmplaceComponent<Position>(entity);
        registry.EmplaceComponent<Velocity>(entity);
    }

    Astra::SystemScheduler scheduler;

    (void)scheduler.AddSystem<PhysicsSystem>();
    (void)scheduler.AddSystem<BoundsCheckSystem>();

    BenchmarkExecutor customExecutor;

    for (auto _ : state)
    {
        scheduler.Execute(registry, &customExecutor);
    }

    state.SetItemsProcessed(state.iterations() * count * 2);
}

// Register benchmarks with common entity counts
BENCHMARK(BM_CreateEntities)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_CreateEntitiesBatch)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_AddComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_RemoveComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Batch component operations
BENCHMARK(BM_AddComponentsBatch)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_RemoveComponentsBatch)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Most important iteration benchmarks (ForEach)
BENCHMARK(BM_IterateSingleComponent)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_IterateTwoComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_IterateTwoComponentsHalf)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_IterateTwoComponentsOne)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_IterateThreeComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_IterateFiveComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Range-based for loop iteration benchmarks (compare against ForEach above)
BENCHMARK(BM_RangeForSingleComponent)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_RangeForTwoComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_RangeForTwoComponentsHalf)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_RangeForThreeComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_RangeForFiveComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Parallel iteration benchmarks
BENCHMARK(BM_ParallelIterateSingleComponent)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_ParallelIterateTwoComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_ParallelIterateTwoComponentsHalf)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_ParallelIterateTwoComponentsOne)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_ParallelIterateThreeComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_ParallelIterateFiveComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Random access
BENCHMARK(BM_GetComponent)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_GetMultipleComponents)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Hierarchy/Relations benchmarks
BENCHMARK(BM_ForEachChild)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_ForEachDescendant)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_ForEachAncestor)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_ForEachLink)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_ParallelForEachDescendant)->Arg(1'000)->Arg(10'000)->Arg(100'000);

// System Scheduling benchmarks
BENCHMARK(BM_SystemScheduler_Sequential)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_SystemScheduler_Lambda)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_SystemScheduler_Parallel)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_SystemScheduler_ManyIndependent);
BENCHMARK(BM_SystemScheduler_WithDependencies);
BENCHMARK(BM_SystemScheduler_CustomExecutor)->Arg(10'000)->Arg(100'000);

// ============================================================================
// S4 Table-only by-id set (RULINGS Task 41, from the Task 18 review). The by-id add/remove
// paths now read a descriptor via IsSparseId; nothing else measures them. These are
// deliberately NOT named BM_Storage_*: Task 42's `--benchmark_filter=-BM_Storage_` gate keeps
// them in the 2% comparison against the pre-storage baselines. Only APIs that exist at 975cdb7
// are used, so the same source builds on both baselines.
// ============================================================================

// Registry::AddComponentByID / RemoveComponentByID churn of a Table component (Velocity).
static void BM_AddRemoveByID(benchmark::State& state)
{
    const size_t count = static_cast<size_t>(state.range(0));
    Astra::Registry registry;
    std::vector<Astra::Entity> entities;
    entities.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        Astra::Entity e = registry.CreateEntity();
        registry.EmplaceComponent<Position>(e);
        entities.push_back(e);
    }
    // Registers Velocity and warms the {Position} <-> {Position, Velocity} archetype pair.
    registry.EmplaceComponent<Velocity>(entities[0]);
    registry.RemoveComponent<Velocity>(entities[0]);
    const Astra::ComponentID id = Astra::TypeID<Velocity>::Value();
    const Velocity value{1, 2};

    for (auto _ : state)
    {
        for (Astra::Entity e : entities) benchmark::DoNotOptimize(registry.AddComponentByID(e, id, &value, sizeof(value)));
        for (Astra::Entity e : entities) benchmark::DoNotOptimize(registry.RemoveComponentByID(e, id));
    }
    state.SetItemsProcessed(state.iterations() * count * 2);
}

// CommandBuffer playback of the same add/remove (Execute -> Registry::AddComponentByID /
// RemoveComponentByID). The timed op records N adds, plays them back, records N removes, plays those back.
static void BM_CommandBufferAddRemove(benchmark::State& state)
{
    const size_t count = static_cast<size_t>(state.range(0));
    Astra::Registry registry;
    std::vector<Astra::Entity> entities;
    entities.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        Astra::Entity e = registry.CreateEntity();
        registry.EmplaceComponent<Position>(e);
        entities.push_back(e);
    }
    registry.EmplaceComponent<Velocity>(entities[0]);
    registry.RemoveComponent<Velocity>(entities[0]);
    Astra::CommandBuffer cmd(&registry);

    for (auto _ : state)
    {
        for (Astra::Entity e : entities) cmd.AddComponent(e, Velocity{1, 2});
        benchmark::DoNotOptimize(cmd.Execute());
        for (Astra::Entity e : entities) cmd.RemoveComponent<Velocity>(e);
        benchmark::DoNotOptimize(cmd.Execute());
    }
    state.SetItemsProcessed(state.iterations() * count * 2);
}

// ============================================================================
// Storage policies (spec 2026-10-01 s9.3). Every name starts with BM_Storage_ so
// the S4 Table-only regression gate excludes them: --benchmark_filter=-BM_Storage_
// ============================================================================
namespace StorageBench
{
    // The Arcane Identity shape: 16 B GUID + std::string name (48 B on MSVC x64 with
    // _ITERATOR_DEBUG_LEVEL 0; the Debug runtime's checked std::string is larger),
    // non-trivially copyable -- every archetype move string-moves it while it is Table.
    // bench-compare/bench_storage.cpp repeats these small types on purpose: the two are
    // independent executables.
    struct BenchIdentity
    {
        std::uint64_t guidHi = 0;
        std::uint64_t guidLo = 0;
        std::string name;

        template<typename Archive>
        void Serialize(Archive& ar) { ar(guidHi)(guidLo)(name); }
    };
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL == 0
    static_assert(sizeof(BenchIdentity) == 48, "BenchIdentity must match Arcane::Identity's 48 bytes");
#endif
    static_assert(!std::is_trivially_copyable_v<BenchIdentity>);

    struct ChurnT { std::uint64_t a, b; };                                                    // 16 B, Table
    struct ChurnS { static constexpr auto AstraStorage = Astra::Storage::Sparse; std::uint64_t a, b; };   // 16 B, Sparse
    static_assert(sizeof(ChurnT) == 16 && sizeof(ChurnS) == 16);

    template<class... Ts>
    std::shared_ptr<Astra::ComponentRegistry> Components()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Ts...>();
        return creg;
    }

    // Scattered membership at `pct` percent (multiplicative hash, deterministic): the
    // realistic case -- holders are interleaved with non-holders inside every chunk.
    inline bool Holds(size_t i, int pct) { return ((i * 2654435761ull) >> 7) % 100 < static_cast<size_t>(pct); }

    // 1. Structural churn with the 48 B Identity present on every entity: one timed op =
    //    add Velocity + remove Velocity (2 archetype moves). Universal takes Identity out of both.
    template<bool Universal>
    void IdentityChurn(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<BenchIdentity>();
        Astra::Registry registry(Components<Position, Velocity, BenchIdentity>(), cfg);
        std::vector<Astra::Entity> entities;
        entities.reserve(count);
        for (size_t i = 0; i < count; ++i)
            entities.push_back(registry.CreateEntityWith(Position{i, i}, BenchIdentity{i, ~i, "Entity " + std::to_string(i)}));
        for (auto _ : state)
        {
            for (Astra::Entity e : entities) registry.EmplaceComponent<Velocity>(e);
            for (Astra::Entity e : entities) registry.RemoveComponent<Velocity>(e);
        }
        state.SetItemsProcessed(state.iterations() * count * 2);
    }

    // 2. Add/remove churn of a 16 B component (S4: Sparse must beat Table at every size).
    template<class C>
    void AddRemoveChurn(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        Astra::Registry registry(Components<Position, C>());
        std::vector<Astra::Entity> entities;
        entities.reserve(count);
        for (size_t i = 0; i < count; ++i) entities.push_back(registry.CreateEntityWith(Position{i, i}));
        for (auto _ : state)
        {
            for (Astra::Entity e : entities) registry.AddComponent(e, C{1, 2});
            for (Astra::Entity e : entities) registry.RemoveComponent<C>(e);
        }
        state.SetItemsProcessed(state.iterations() * count * 2);
    }

    // 3. Filtered iteration View<Position, C> with C on pct% of the entities.
    template<class C>
    void FilteredIter(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        const int pct = static_cast<int>(state.range(1));
        Astra::Registry registry(Components<Position, C>());
        size_t matched = 0;
        for (size_t i = 0; i < count; ++i)
        {
            Astra::Entity e = registry.CreateEntityWith(Position{i, i});
            if (Holds(i, pct)) { registry.AddComponent(e, C{i, i}); ++matched; }
        }
        auto view = registry.CreateView<Position, C>();
        for (auto _ : state)
        {
            std::uint64_t sum = 0;
            view.ForEach([&](Astra::Entity, Position& p, C& c) { sum += p.x + c.a; });
            benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * count);   // per scanned entity
        state.counters["matched"] = static_cast<double>(matched);
    }

    // 3b. View<C> alone at the same densities: the Sparse one is sparse-driven (s6.2).
    template<class C>
    void SoleIter(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        const int pct = static_cast<int>(state.range(1));
        Astra::Registry registry(Components<Position, C>());
        size_t matched = 0;
        for (size_t i = 0; i < count; ++i)
        {
            Astra::Entity e = registry.CreateEntityWith(Position{i, i});
            if (Holds(i, pct)) { registry.AddComponent(e, C{i, i}); ++matched; }
        }
        auto view = registry.CreateView<C>();
        for (auto _ : state)
        {
            std::uint64_t sum = 0;
            view.ForEach([&](Astra::Entity, C& c) { sum += c.a; });
            benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * matched);
        state.counters["matched"] = static_cast<double>(matched);
    }

    // 3c. Sparse-driven View<C> with a NON-const, untracked yield: every visited entity writes,
    //     so the walk stamps the shared per-component coarse version. Serial vs ParallelForEach
    //     on a worker pool measures that shared-stamp cache-line contention (RULINGS Task 41,
    //     Task 28 review). Compare the two rows: a Parallel row slower than Serial at the same
    //     size is the contention signature.
    template<class C, bool Parallel>
    void SparseDrivenWrite(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        const int pct = static_cast<int>(state.range(1));
        Astra::Registry::Config cfg;
        if constexpr (Parallel) cfg.workScheduler = BenchPool();
        Astra::Registry registry(Components<Position, C>(), cfg);
        size_t matched = 0;
        for (size_t i = 0; i < count; ++i)
        {
            Astra::Entity e = registry.CreateEntityWith(Position{i, i});
            if (Holds(i, pct)) { registry.AddComponent(e, C{i, i}); ++matched; }
        }
        auto view = registry.CreateView<C>();
        for (auto _ : state)
        {
            if constexpr (Parallel) view.ParallelForEach([](Astra::Entity, C& c) { c.a += 1; });
            else view.ForEach([](Astra::Entity, C& c) { c.a += 1; });
        }
        state.SetItemsProcessed(state.iterations() * matched);
        state.counters["matched"] = static_cast<double>(matched);
    }

    // 4. Random GetComponent, shuffled order (cache-hostile), per kind.
    template<class C, bool Universal>
    void RandomGet(benchmark::State& state)
    {
        const size_t count = static_cast<size_t>(state.range(0));
        Astra::Registry::Config cfg;
        if constexpr (Universal) cfg.AddUniversal<C>();
        Astra::Registry registry(Components<Position, C>(), cfg);
        std::vector<Astra::Entity> entities(count);
        for (size_t i = 0; i < count; ++i) entities[i] = registry.CreateEntityWith(Position{i, i}, C{i, i});
        std::mt19937 rng(0xC0FFEE);
        std::shuffle(entities.begin(), entities.end(), rng);
        for (auto _ : state)
        {
            std::uint64_t sum = 0;
            for (Astra::Entity e : entities) sum += registry.GetComponent<C>(e)->a;
            benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * count);
    }
} // namespace StorageBench

static void BM_Storage_IdentityChurn_Table(benchmark::State& s)     { StorageBench::IdentityChurn<false>(s); }
static void BM_Storage_IdentityChurn_Universal(benchmark::State& s) { StorageBench::IdentityChurn<true>(s); }
static void BM_Storage_AddRemoveChurn_Table(benchmark::State& s)    { StorageBench::AddRemoveChurn<StorageBench::ChurnT>(s); }
static void BM_Storage_AddRemoveChurn_Sparse(benchmark::State& s)   { StorageBench::AddRemoveChurn<StorageBench::ChurnS>(s); }
static void BM_Storage_FilteredIter_Table(benchmark::State& s)      { StorageBench::FilteredIter<StorageBench::ChurnT>(s); }
static void BM_Storage_FilteredIter_Sparse(benchmark::State& s)     { StorageBench::FilteredIter<StorageBench::ChurnS>(s); }
static void BM_Storage_SoleIter_Table(benchmark::State& s)          { StorageBench::SoleIter<StorageBench::ChurnT>(s); }
static void BM_Storage_SoleIter_Sparse(benchmark::State& s)         { StorageBench::SoleIter<StorageBench::ChurnS>(s); }
static void BM_Storage_SparseDrivenWrite_Serial(benchmark::State& s)   { StorageBench::SparseDrivenWrite<StorageBench::ChurnS, false>(s); }
static void BM_Storage_SparseDrivenWrite_Parallel(benchmark::State& s) { StorageBench::SparseDrivenWrite<StorageBench::ChurnS, true>(s); }
static void BM_Storage_RandomGet_Table(benchmark::State& s)         { StorageBench::RandomGet<StorageBench::ChurnT, false>(s); }
static void BM_Storage_RandomGet_Sparse(benchmark::State& s)        { StorageBench::RandomGet<StorageBench::ChurnS, false>(s); }
static void BM_Storage_RandomGet_Universal(benchmark::State& s)     { StorageBench::RandomGet<StorageBench::ChurnT, true>(s); }

BENCHMARK(BM_AddRemoveByID)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_CommandBufferAddRemove)->Arg(1'000)->Arg(10'000)->Arg(100'000);

BENCHMARK(BM_Storage_IdentityChurn_Table)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_IdentityChurn_Universal)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_AddRemoveChurn_Table)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_AddRemoveChurn_Sparse)->Arg(1'000)->Arg(10'000)->Arg(100'000);
BENCHMARK(BM_Storage_FilteredIter_Table)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_FilteredIter_Sparse)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_SoleIter_Table)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_SoleIter_Sparse)->Args({100'000, 1})->Args({100'000, 50})->Args({100'000, 99});
BENCHMARK(BM_Storage_SparseDrivenWrite_Serial)->Args({100'000, 50})->Args({1'000'000, 50});
BENCHMARK(BM_Storage_SparseDrivenWrite_Parallel)->Args({100'000, 50})->Args({1'000'000, 50});
BENCHMARK(BM_Storage_RandomGet_Table)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_Storage_RandomGet_Sparse)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);
BENCHMARK(BM_Storage_RandomGet_Universal)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

BENCHMARK_MAIN();
