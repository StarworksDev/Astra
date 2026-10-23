// Storage invariant fuzzer (spec 2026-10-01 s9.2): seeded random operation sequences over
// Table / Sparse / Sparse-enableable / Sparse-tag / Universal components against a reference
// model; ValidateStorageInvariants (P1-P3) + a full model comparison after EVERY operation.
//
// Normal suite:  StorageInvariantFuzz.FixedSeedsShortRun (4 fixed seeds x 600 ops).
// Long run:      --gtest_also_run_disabled_tests --gtest_filter=StorageInvariantFuzz.DISABLED_LongRun
//                env ASTRA_FUZZ_SEED (first seed, default 1000), ASTRA_FUZZ_SEEDS (count, 64),
//                ASTRA_FUZZ_OPS (ops per seed, 20000). Reproduce one seed with ASTRA_FUZZ_SEEDS=1.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "StorageParity.hpp"

using namespace Astra::Test::Parity;
using Astra::Test::EnTag;
using Astra::Test::Hierarchy;
using Astra::Test::Name;
using Astra::Test::Player;
using Astra::Test::Timer;
using Astra::Test::Velocity;

namespace
{
    constexpr size_t kMaxAlive = 400;

    struct ModelEntity
    {
        std::optional<Velocity> vel;
        std::optional<Hierarchy> hier;
        bool hierEnabled = true;
        bool player = false;
        bool enTag = false;
        bool enTagEnabled = true;
        std::optional<ParityPosS> pos;
        std::optional<ParityHierS> hs;
        bool hsEnabled = true;
        bool tag = false;
        std::optional<int> live;
        std::string name;                 // Universal: always present
        float timerDuration = 1.0f;       // Universal: Timer{} has duration 1.0
        bool timerEnabled = true;
    };

    std::string InitName(Astra::Entity e) { return "init" + std::to_string(e.GetID()); }

    // The registry's universal init: runs only when no Name value was supplied (s4.2).
    void NameInit(Astra::Entity e, void* component, void* /*user*/)
    {
        static_cast<Name*>(component)->value = InitName(e);
    }

    Astra::Registry::Config FuzzConfig()
    {
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>(&NameInit, nullptr);
        cfg.AddUniversal<Timer>();
        return cfg;
    }

    std::shared_ptr<Astra::ComponentRegistry> FuzzComponents()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Velocity, Hierarchy, Player, ParityPosS, ParityHierS,
                                 ParityTagS, ParityLiveS, Name, Timer, EnTag>();
        return creg;
    }

    uint64_t EnvU64(const char* name, uint64_t fallback)
    {
#ifdef _MSC_VER
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return fallback;
        const uint64_t parsed = std::strtoull(value, nullptr, 0);
        std::free(value);
        return parsed;
#else
        const char* value = std::getenv(name);
        return value ? std::strtoull(value, nullptr, 0) : fallback;
#endif
    }

    class StorageFuzzer
    {
    public:
        explicit StorageFuzzer(uint64_t seed)
            : m_seed(seed), m_rng(seed), m_creg(FuzzComponents()),
              m_reg(std::make_unique<Astra::Registry>(m_creg, FuzzConfig())),
              m_liveBase(ParityLiveS::s_live)
        {}

        // Returns false at the first divergence (a gtest failure has been recorded).
        bool Run(size_t ops)
        {
            Astra::Testing::ScopedAssertHandler guard(&OnAssert, this);
            for (m_opIndex = 0; m_opIndex < ops; ++m_opIndex)
            {
                m_op = Step();
                if (!Check()) return false;
            }
            return true;
        }

        // Destroys the registry: every ParityLiveS it held must be destructed exactly once.
        void Finish()
        {
            m_reg.reset();
            EXPECT_EQ(ParityLiveS::s_live, m_liveBase) << "seed " << m_seed << ": ParityLiveS leaked or double-destroyed";
        }

    private:
        uint64_t m_seed;
        std::mt19937_64 m_rng;
        std::shared_ptr<Astra::ComponentRegistry> m_creg;
        std::unique_ptr<Astra::Registry> m_reg;
        int m_liveBase;
        std::unordered_map<Astra::Entity, ModelEntity> m_model;
        std::vector<Astra::Entity> m_alive;
        std::vector<Astra::Entity> m_dead;
        size_t m_opIndex = 0;
        const char* m_op = "";
        uint64_t m_counter = 0;
        bool m_expectRefusal = false;
        int m_refusals = 0;
        int m_unexpectedAsserts = 0;
        std::string m_lastUnexpected;
        // Expected refusals (Universal removes) count and continue; anything else is a
        // defect: print the reproduction line FIRST (an ASTRA_ASSERT aborts after Break).
        static Astra::AssertAction OnAssert(const Astra::AssertContext& ctx, void* user) noexcept
        {
            auto* self = static_cast<StorageFuzzer*>(user);
            if (self->m_expectRefusal)
            {
                ++self->m_refusals;
                return Astra::AssertAction::Continue;
            }
            ++self->m_unexpectedAsserts;
            self->m_lastUnexpected = ctx.message ? ctx.message : (ctx.expression ? ctx.expression : "?");
            std::fprintf(stderr, "[ fuzz ] UNEXPECTED ASSERT seed=%" PRIu64 " op#%zu (%s): %s\n",
                         self->m_seed, self->m_opIndex, self->m_op, self->m_lastUnexpected.c_str());
            std::fflush(stderr);
            return Astra::AssertAction::Break;
        }

        uint32_t Rand(size_t n) { return static_cast<uint32_t>(m_rng() % (n ? n : 1)); }
        bool Chance(uint32_t pct) { return Rand(100) < pct; }
        float F() { return static_cast<float>(Rand(1000)); }
        Astra::Entity PickAlive() { return m_alive[Rand(m_alive.size())]; }

        std::vector<Astra::Entity> PickDistinct(size_t k)
        {
            std::vector<Astra::Entity> pool = m_alive;
            std::shuffle(pool.begin(), pool.end(), m_rng);
            pool.resize(std::min(k, pool.size()));
            return pool;
        }

        void Track(Astra::Entity e, ModelEntity m)
        {
            ASSERT_TRUE(e.IsValid()) << "create returned Invalid (seed " << m_seed << ")";
            m_model.emplace(e, std::move(m));
            m_alive.push_back(e);
        }

        void Untrack(Astra::Entity e)
        {
            m_model.erase(e);
            auto it = std::find(m_alive.begin(), m_alive.end(), e);
            if (it != m_alive.end()) { *it = m_alive.back(); m_alive.pop_back(); }
            NoteDead(e);
        }

        // Only the 128 most recent dead handles are probed: an index recycled ~2^8 times
        // (8 version bits) could legitimately make a very old handle compare live again.
        void NoteDead(Astra::Entity e)
        {
            m_dead.push_back(e);
            if (m_dead.size() > 128) m_dead.erase(m_dead.begin());
        }

        const char* Step()
        {
            const uint32_t r = Rand(1000);
            if (m_alive.empty() || r < 120) return OpCreate();
            if (r < 170) return OpBatchCreate();
            if (r < 250) return OpDestroy();
            if (r < 270) return OpBatchDestroy();
            if (r < 470) return OpAdd();
            if (r < 620) return OpRemove();
            if (r < 700) return OpEnable();
            if (r < 790) return OpWrite();
            if (r < 840) return OpBatchSparse();
            if (r < 870) return OpCommandBuffer();
            if (r < 890) return OpStaleProbe();
            if (r < 920) { (void)m_reg->Defragment(); return "defragment"; }
            if (r < 927) return OpSaveLoad();
            if (r < 930) return OpClear();
            return OpRangeFor();
        }

        const char* OpCreate()
        {
            if (m_alive.size() >= kMaxAlive) return OpDestroy();
            ModelEntity m;
            Astra::Entity e;
            bool nameGiven = false;
            switch (Rand(4))
            {
            case 0:
                e = m_reg->CreateEntity();
                break;
            case 1:
            {
                Velocity v{F(), F(), F()};   // non-const lvalues: the Component concept rejects const T&
                ParityPosS p{F(), F(), F()};
                e = m_reg->CreateEntityWith(v, p);
                m.vel = v;
                m.pos = p;
                break;
            }
            case 2:
            {
                std::string n = "given" + std::to_string(m_counter++);
                e = m_reg->CreateEntityWith(Name{n}, ParityTagS{}, Hierarchy{});   // explicit Name: init must NOT run
                m.name = n;
                nameGiven = true;
                m.tag = true;
                m.hier = Hierarchy{};
                break;
            }
            default:
                e = m_reg->CreateEntity<Velocity, ParityHierS, Player>();
                m.vel = Velocity{};
                m.hs = ParityHierS{};
                m.player = true;
                break;
            }
            if (!nameGiven) m.name = InitName(e);
            Track(e, std::move(m));
            return "create";
        }

        const char* OpBatchCreate()
        {
            if (m_alive.size() >= kMaxAlive) return OpBatchDestroy();
            const size_t k = 1 + Rand(48);
            std::vector<Astra::Entity> out(k);
            if (Chance(50))
            {
                const float base = F();
                const size_t made = m_reg->CreateEntitiesWith<Velocity, ParityPosS>(k, std::span{out}, [&](size_t i) {
                    return std::tuple{Velocity{base + float(i), 0.0f, 0.0f}, ParityPosS{base, float(i), 0.0f}};
                });
                EXPECT_EQ(made, k);
                for (size_t i = 0; i < made; ++i)
                {
                    ModelEntity m;
                    m.vel = Velocity{base + float(i), 0.0f, 0.0f};
                    m.pos = ParityPosS{base, float(i), 0.0f};
                    m.name = InitName(out[i]);
                    Track(out[i], std::move(m));
                }
                return "batch-create Velocity+PosS";
            }
            const size_t made = m_reg->CreateEntities<ParityLiveS, ParityTagS>(k, std::span{out});
            EXPECT_EQ(made, k);
            for (size_t i = 0; i < made; ++i)
            {
                ModelEntity m;
                m.live = 0;
                m.tag = true;
                m.name = InitName(out[i]);
                Track(out[i], std::move(m));
            }
            return "batch-create LiveS+TagS";
        }

        const char* OpDestroy()
        {
            const Astra::Entity e = PickAlive();
            m_reg->DestroyEntity(e);
            Untrack(e);
            return "destroy";
        }

        const char* OpBatchDestroy()
        {
            std::vector<Astra::Entity> victims = PickDistinct(1 + Rand(16));
            m_reg->DestroyEntities(std::span{victims});
            for (Astra::Entity e : victims) Untrack(e);
            return "batch-destroy";
        }
        // Typed, emplace or by-id add of T; `present` is the model's view BEFORE the call.
        // Typed/emplace adds of a present component are a true no-op; the by-id add returns
        // true iff the component was absent (false when present, always false for Universal).
        template<class T>
        void AddOne(Astra::Entity e, const T& value, bool present, bool universal = false)
        {
            bool ok = false;
            const uint32_t how = std::is_empty_v<T> ? Rand(2) : Rand(3);   // no by-id for tags
            if (how == 0)      ok = m_reg->AddComponent(e, value);
            else if (how == 1) ok = m_reg->EmplaceComponent<T>(e, value);
            else               ok = m_reg->AddComponentByID(e, Astra::TypeID<T>::Value(), &value, sizeof(T));
            if (how == 2) EXPECT_EQ(ok, !present && !universal) << "by-id add: true iff absent";
            else          EXPECT_TRUE(ok) << "typed/emplace add of a live entity must return true (present => no-op)";
        }

        const char* OpAdd()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(10))
            {
            case 0: { const Velocity v{F(), F(), F()}; AddOne(e, v, m.vel.has_value()); if (!m.vel) m.vel = v; return "add Velocity"; }
            case 1: { Hierarchy h; h.parent = Rand(1000); AddOne(e, h, m.hier.has_value()); if (!m.hier) { m.hier = h; m.hierEnabled = true; } return "add Hierarchy"; }
            case 2: { AddOne(e, Player{}, m.player); m.player = true; return "add Player"; }
            case 3: { const ParityPosS p{F(), F(), F()}; AddOne(e, p, m.pos.has_value()); if (!m.pos) m.pos = p; return "add PosS"; }
            case 4: { ParityHierS h; h.parent = Rand(1000); AddOne(e, h, m.hs.has_value()); if (!m.hs) { m.hs = h; m.hsEnabled = true; } return "add HierS"; }
            case 5: { AddOne(e, ParityTagS{}, m.tag); m.tag = true; return "add TagS"; }
            case 6: { const ParityLiveS l(static_cast<int>(Rand(1000))); AddOne(e, l, m.live.has_value()); if (!m.live) m.live = l.value; return "add LiveS"; }
            case 7: { AddOne(e, Name{"ignored"}, true, true); return "add Name (universal no-op)"; }
            case 8: { AddOne(e, Timer{5.0f}, true, true); return "add Timer (universal no-op)"; }
            default: { AddOne(e, EnTag{}, m.enTag); if (!m.enTag) { m.enTag = true; m.enTagEnabled = true; } return "add EnTag"; }
            }
        }

        template<class T>
        bool RemoveOne(Astra::Entity e)
        {
            return Chance(50) ? m_reg->RemoveComponent<T>(e)
                              : m_reg->RemoveComponentByID(e, Astra::TypeID<T>::Value());
        }

        template<class T>
        void RemoveUniversal(Astra::Entity e)
        {
            const int before = m_refusals;
            m_expectRefusal = true;
            const bool removed = RemoveOne<T>(e);
            m_expectRefusal = false;
            EXPECT_FALSE(removed) << "a Universal component must never be removed";
            if (kAssertsActive) EXPECT_GT(m_refusals, before) << "Debug must assert on a Universal remove";
        }

        const char* OpRemove()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(10))
            {
            case 0: EXPECT_EQ(RemoveOne<Velocity>(e), m.vel.has_value()); m.vel.reset(); return "remove Velocity";
            case 1: EXPECT_EQ(RemoveOne<Hierarchy>(e), m.hier.has_value()); m.hier.reset(); m.hierEnabled = true; return "remove Hierarchy";
            case 2: EXPECT_EQ(RemoveOne<Player>(e), m.player); m.player = false; return "remove Player";
            case 3: EXPECT_EQ(RemoveOne<ParityPosS>(e), m.pos.has_value()); m.pos.reset(); return "remove PosS";
            case 4: EXPECT_EQ(RemoveOne<ParityHierS>(e), m.hs.has_value()); m.hs.reset(); m.hsEnabled = true; return "remove HierS";
            case 5: EXPECT_EQ(RemoveOne<ParityTagS>(e), m.tag); m.tag = false; return "remove TagS";
            case 6: EXPECT_EQ(RemoveOne<ParityLiveS>(e), m.live.has_value()); m.live.reset(); return "remove LiveS";
            case 7: RemoveUniversal<Name>(e); return "remove Name (refused)";
            case 8: RemoveUniversal<Timer>(e); return "remove Timer (refused)";
            default: EXPECT_EQ(RemoveOne<EnTag>(e), m.enTag); m.enTag = false; m.enTagEnabled = true; return "remove EnTag";
            }
        }

        const char* OpEnable()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            const bool enable = Chance(50);
            switch (Rand(4))
            {
            case 0:
                EXPECT_EQ(m_reg->SetEnabled<Hierarchy>(e, enable), m.hier.has_value());
                if (m.hier) m.hierEnabled = enable;
                return "enable Hierarchy";
            case 1:
                EXPECT_EQ(m_reg->SetEnabled<ParityHierS>(e, enable), m.hs.has_value());
                if (m.hs) m.hsEnabled = enable;
                return "enable HierS";
            case 2:
                EXPECT_TRUE(m_reg->SetEnabled<Timer>(e, enable));   // Universal: always present
                m.timerEnabled = enable;
                return "enable Timer";
            default:
                EXPECT_EQ(m_reg->SetEnabled<EnTag>(e, enable), m.enTag);   // enableable Table tag (bit-only column)
                if (m.enTag) m.enTagEnabled = enable;
                return "enable EnTag";
            }
        }

        // Value writes through GetComponent: a relocation that loses or swaps data shows up
        // as a value mismatch on the next check.
        const char* OpWrite()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(6))
            {
            case 0: if (m.vel) { const float v = F(); m_reg->GetComponent<Velocity>(e)->dx = v; m.vel->dx = v; } return "write Velocity";
            case 1: if (m.pos) { const float v = F(); m_reg->GetComponent<ParityPosS>(e)->y = v; m.pos->y = v; } return "write PosS";
            case 2: if (m.hs) { const uint32_t v = Rand(1000); m_reg->GetComponent<ParityHierS>(e)->parent = v; m.hs->parent = v; } return "write HierS";
            case 3: if (m.live) { const int v = static_cast<int>(Rand(1000)); m_reg->GetComponent<ParityLiveS>(e)->value = v; m.live = v; } return "write LiveS";
            case 4: { std::string v = "w" + std::to_string(m_counter++); m_reg->GetComponent<Name>(e)->value = v; m.name = v; return "write Name"; }
            default: { const float v = F(); m_reg->GetComponent<Timer>(e)->duration = v; m.timerDuration = v; return "write Timer"; }
            }
        }
        const char* OpBatchSparse()
        {
            std::vector<Astra::Entity> es = PickDistinct(1 + Rand(32));
            switch (Rand(4))
            {
            case 0:
            {
                const ParityPosS p{F(), F(), F()};
                m_reg->AddComponents(std::span{es}, p);
                for (Astra::Entity e : es) if (!m_model[e].pos) m_model[e].pos = p;
                return "batch-add PosS";
            }
            case 1:
            {
                size_t had = 0;
                for (Astra::Entity e : es) had += m_model[e].pos.has_value();
                EXPECT_EQ(m_reg->RemoveComponents<ParityPosS>(std::span{es}), had);
                for (Astra::Entity e : es) m_model[e].pos.reset();
                return "batch-remove PosS";
            }
            case 2:
            {
                m_reg->EmplaceComponents<ParityTagS>(std::span{es});
                for (Astra::Entity e : es) m_model[e].tag = true;
                return "batch-emplace TagS";
            }
            default:
            {
                const ParityLiveS l(static_cast<int>(Rand(1000)));
                m_reg->AddComponents(std::span{es}, l);
                for (Astra::Entity e : es) if (!m_model[e].live) m_model[e].live = l.value;
                return "batch-add LiveS";
            }
            }
        }

        // Recorded commands apply in record order at Execute (eager CommandBuffer).
        const char* OpCommandBuffer()
        {
            Astra::CommandBuffer cmd(m_reg.get());
            const Astra::Entity t = cmd.CreateEntity();
            ParityPosS p{F(), F(), F()};   // non-const lvalue (Component concept)
            cmd.AddComponent(t, p);
            std::vector<Astra::Entity> ab = PickDistinct(2);
            bool destroyB = false;
            if (ab.size() == 2)
            {
                if (!m_model[ab[0]].tag) cmd.AddComponent(ab[0], ParityTagS{});   // a recorded add of a present tag fails Execute
                if (m_model[ab[0]].live) cmd.RemoveComponent<ParityLiveS>(ab[0]);
                else                     cmd.AddComponent(ab[0], ParityLiveS(7));
                if (m_model[ab[1]].hs) cmd.SetEnabled<ParityHierS>(ab[1], false);
                destroyB = Chance(30);
                if (destroyB) cmd.DestroyEntity(ab[1]);
            }
            EXPECT_TRUE(cmd.Execute().IsOk()) << "CommandBuffer playback failed";

            ModelEntity mt;
            mt.pos = p;
            mt.name = InitName(t);              // created through the funnel: universal init ran
            Track(t, std::move(mt));
            if (ab.size() == 2)
            {
                ModelEntity& a = m_model[ab[0]];
                a.tag = true;
                if (a.live) a.live.reset(); else a.live = 7;
                if (m_model[ab[1]].hs) m_model[ab[1]].hsEnabled = false;
                if (destroyB) Untrack(ab[1]);
            }
            return "command-buffer";
        }

        // Review Focus 1: a stale handle misses or is refused for every kind.
        const char* OpStaleProbe()
        {
            if (m_dead.empty()) return "stale-probe (none)";
            const Astra::Entity d = m_dead[Rand(m_dead.size())];
            EXPECT_FALSE(m_reg->IsValid(d));
            EXPECT_EQ(m_reg->GetComponent<ParityPosS>(d), nullptr);
            EXPECT_EQ(m_reg->GetComponent<Name>(d), nullptr);
            EXPECT_FALSE(m_reg->HasComponent<ParityTagS>(d));
            EXPECT_FALSE(m_reg->HasComponent<Timer>(d));
            EXPECT_FALSE(m_reg->AddComponent(d, ParityPosS{}));
            EXPECT_FALSE(m_reg->AddComponent(d, ParityTagS{}));
            EXPECT_FALSE(m_reg->SetEnabled<ParityHierS>(d, false));
            EXPECT_FALSE(m_reg->SetEnabled<Timer>(d, false));
            EXPECT_FALSE(m_reg->RemoveComponent<ParityLiveS>(d));
            return "stale-probe";
        }

        // The old registry is destroyed when m_reg is replaced; s_live accounting must hold.
        const char* OpSaveLoad()
        {
            Astra::Registry::SaveConfig sc;
            sc.compressionMode = Chance(50) ? Astra::CompressionMode::LZ4 : Astra::CompressionMode::None;
            auto saved = m_reg->Save(sc);
            if (!saved.IsOk()) { ADD_FAILURE() << "Save failed"; return "save-load"; }
            auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), m_creg, FuzzConfig());
            if (!loaded.IsOk()) { ADD_FAILURE() << "Load failed: " << static_cast<int>(*loaded.GetError()); return "save-load"; }
            m_reg = std::move(*loaded.GetValue());
            return sc.compressionMode == Astra::CompressionMode::LZ4 ? "save-load LZ4" : "save-load raw";
        }

        const char* OpClear()
        {
            m_reg->Clear();
            for (Astra::Entity e : m_alive) NoteDead(e);
            m_alive.clear();
            m_model.clear();
            return "clear";
        }

        const char* OpRangeFor()
        {
            size_t n = 0;
            for (auto [e, vel, pos] : m_reg->CreateView<Velocity, ParityPosS>())
            {
                ++n;
                auto it = m_model.find(e);
                EXPECT_TRUE(it != m_model.end() && it->second.pos && *it->second.pos == pos);
                (void)vel;
            }
            size_t expected = 0;
            for (const auto& [e, m] : m_model) expected += (m.vel && m.pos) ? 1 : 0;
            EXPECT_EQ(n, expected);
            return "range-for";
        }
        // First mismatch between the registry and the model for `e`, or empty.
        std::string CompareEntity(Astra::Entity e, const ModelEntity& m) const
        {
            const Astra::Registry& r = *m_reg;
            if (!r.IsValid(e)) return "entity not valid";

            const Velocity* vel = r.GetComponent<Velocity>(e);
            if ((vel != nullptr) != m.vel.has_value() || r.HasComponent<Velocity>(e) != m.vel.has_value()) return "Velocity presence";
            if (vel && (vel->dx != m.vel->dx || vel->dy != m.vel->dy || vel->dz != m.vel->dz)) return "Velocity value";

            const Hierarchy* hier = r.GetComponent<Hierarchy>(e);
            if ((hier != nullptr) != m.hier.has_value()) return "Hierarchy presence";
            if (hier && hier->parent != m.hier->parent) return "Hierarchy value";
            if (r.IsEnabled<Hierarchy>(e) != (m.hier.has_value() && m.hierEnabled)) return "Hierarchy enabled";

            if (r.HasComponent<Player>(e) != m.player) return "Player presence";
            if (r.HasComponent<EnTag>(e) != m.enTag) return "EnTag presence";
            if (r.IsEnabled<EnTag>(e) != (m.enTag && m.enTagEnabled)) return "EnTag enabled";

            const ParityPosS* pos = r.GetComponent<ParityPosS>(e);
            if ((pos != nullptr) != m.pos.has_value() || r.HasComponent<ParityPosS>(e) != m.pos.has_value()) return "PosS presence";
            if (pos && !(*pos == *m.pos)) return "PosS value";

            const ParityHierS* hs = r.GetComponent<ParityHierS>(e);
            if ((hs != nullptr) != m.hs.has_value()) return "HierS presence";
            if (hs && hs->parent != m.hs->parent) return "HierS value";
            if (r.IsEnabled<ParityHierS>(e) != (m.hs.has_value() && m.hsEnabled)) return "HierS enabled";

            if (r.HasComponent<ParityTagS>(e) != m.tag) return "TagS presence";

            const ParityLiveS* live = r.GetComponent<ParityLiveS>(e);
            if ((live != nullptr) != m.live.has_value()) return "LiveS presence";
            if (live && live->value != *m.live) return "LiveS value";

            const Name* name = r.GetComponent<Name>(e);
            if (!name || !r.HasComponent<Name>(e)) return "Name (universal) missing";
            if (name->value != m.name) return "Name value: got '" + name->value + "' want '" + m.name + "'";

            const Timer* timer = r.GetComponent<Timer>(e);
            if (!timer || !r.HasComponent<Timer>(e)) return "Timer (universal) missing";
            if (timer->duration != m.timerDuration) return "Timer value";
            if (r.IsEnabled<Timer>(e) != m.timerEnabled) return "Timer enabled";
            return {};
        }

        bool Fail(const std::string& what)
        {
            ADD_FAILURE() << "[ fuzz ] seed=" << m_seed << " op#" << m_opIndex << " (" << m_op << "): " << what;
            return false;
        }

        bool Check()
        {
            if (::testing::Test::HasFailure()) return Fail("an EXPECT inside the op failed (see above)");
            const std::string invariants = m_reg->ValidateStorageInvariants();
            if (!invariants.empty()) return Fail("ValidateStorageInvariants: " + invariants);
            if (m_unexpectedAsserts != 0) return Fail("unexpected assert: " + m_lastUnexpected);
            if (m_reg->Size() != m_model.size())
                return Fail("Size " + std::to_string(m_reg->Size()) + " != model " + std::to_string(m_model.size()));
            size_t live = 0, pos = 0, velPos = 0, velNoTag = 0, hsOn = 0, hsAll = 0, timerOn = 0, tag = 0, vel = 0, hierOn = 0, enTagOn = 0;
            for (const auto& [e, m] : m_model)
            {
                if (std::string why = CompareEntity(e, m); !why.empty())
                    return Fail("entity " + std::to_string(e.GetID()) + "v" + std::to_string(e.GetVersion()) + ": " + why);
                live += m.live.has_value();
                pos += m.pos.has_value();
                velPos += (m.vel && m.pos);
                velNoTag += (m.vel && !m.tag);
                hsOn += (m.hs && m.hsEnabled);
                hsAll += m.hs.has_value();
                timerOn += m.timerEnabled;
                tag += m.tag;
                vel += m.vel.has_value();
                hierOn += (m.hier && m.hierEnabled);
                enTagOn += (m.vel && m.enTag && m.enTagEnabled);
            }
            if (ParityLiveS::s_live != m_liveBase + static_cast<int>(live))
                return Fail("ParityLiveS live count " + std::to_string(ParityLiveS::s_live - m_liveBase) + " != model " + std::to_string(live));
            auto expectSize = [&](size_t got, size_t want, const char* view) {
                return got == want ? true : Fail(std::string(view) + " Size " + std::to_string(got) + " != " + std::to_string(want));
            };
            return expectSize(m_reg->CreateView<ParityPosS>().Size(), pos, "View<PosS>")
                && expectSize(m_reg->CreateView<Velocity, ParityPosS>().Size(), velPos, "View<Velocity,PosS>")
                && expectSize(m_reg->CreateView<Velocity, Astra::Not<ParityTagS>>().Size(), velNoTag, "View<Velocity,Not<TagS>>")
                && expectSize(m_reg->CreateView<ParityHierS>().Size(), hsOn, "View<HierS>")
                && expectSize(m_reg->CreateView<Astra::IncludeDisabled<ParityHierS>>().Size(), hsAll, "View<IncludeDisabled<HierS>>")
                && expectSize(m_reg->CreateView<Timer>().Size(), timerOn, "View<Timer> (universal)")
                && expectSize(m_reg->CreateView<ParityTagS>().Size(), tag, "View<TagS>")
                && expectSize(m_reg->CreateView<Velocity, Astra::Optional<ParityLiveS>>().Size(), vel, "View<Velocity,Optional<LiveS>>")
                && expectSize(m_reg->CreateView<Hierarchy>().Size(), hierOn, "View<Hierarchy>")
                && expectSize(m_reg->CreateView<Velocity, EnTag>().Size(), enTagOn, "View<Velocity,EnTag>");
        }
    };
} // namespace

TEST(StorageInvariantFuzz, FixedSeedsShortRun)
{
    constexpr size_t kOps = 600;   // ~22 s Debug on the desk (1500 took 56 s); the long run covers depth
    for (uint64_t seed : {1ull, 2ull, 3ull, 0xA57A5EEDull})
    {
        std::printf("[ fuzz ] seed=%" PRIu64 " ops=%zu\n", seed, kOps);
        StorageFuzzer fuzzer(seed);
        const bool ok = fuzzer.Run(kOps);
        fuzzer.Finish();
        if (!ok) return;   // the failure message carries seed + op index
    }
}

TEST(StorageInvariantFuzz, DISABLED_LongRun)
{
    const uint64_t first = EnvU64("ASTRA_FUZZ_SEED", 1000);
    const uint64_t count = EnvU64("ASTRA_FUZZ_SEEDS", 64);
    const uint64_t ops = EnvU64("ASTRA_FUZZ_OPS", 20000);
    for (uint64_t seed = first; seed < first + count; ++seed)
    {
        std::printf("[ fuzz ] seed=%" PRIu64 " ops=%" PRIu64 "\n", seed, ops);
        std::fflush(stdout);
        StorageFuzzer fuzzer(seed);
        const bool ok = fuzzer.Run(static_cast<size_t>(ops));
        fuzzer.Finish();
        if (!ok) return;
    }
}
