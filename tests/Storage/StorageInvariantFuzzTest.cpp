// Storage invariant fuzzer (spec 2026-10-01 s9.2): seeded random operation sequences over
// Table / Sparse / Sparse-enableable / Sparse-tag / Universal components against a reference
// model; ValidateStorageInvariants (P1-P3) + a full model comparison after EVERY operation.
//
// Normal suite:  StorageInvariantFuzz.FixedSeedsShortRun (4 fixed seeds x kOps ops); prints a
//                per-op hit histogram and asserts the structural ops were reached.
// Long run:      --gtest_also_run_disabled_tests --gtest_filter=StorageInvariantFuzz.DISABLED_LongRun
//                env ASTRA_FUZZ_SEED (first seed, default 1000), ASTRA_FUZZ_SEEDS (count, 64),
//                ASTRA_FUZZ_OPS (ops per seed, 20000); decimal only, garbage or zero is a failure.
//                Reproduce one seed with ASTRA_FUZZ_SEEDS=1.
// Reproduction: a seed is deterministic only on the same STL (std::shuffle is implementation-
//                defined); the project is MSVC-only (G10), so the printed seed is enough.
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
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
    // A rare burst pushes the population past one sparse page (4096) and four Universal pages
    // (1024 slots each), runs a full Check, then a bulk destroy returns it under the cap.
    constexpr size_t kBurstTarget = 4300;

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
        bool tagEnabled = true;           // Sparse enableable tag: true whenever the tag is absent
        std::optional<int> live;
        std::string name;                 // Universal: always present
        Timer timer;                      // Universal: always present (Timer{} duration 1.0)
        bool timerEnabled = true;
    };

    // What the run actually reached (printed; the structural ops are asserted).
    struct FuzzStats
    {
        std::map<std::string, size_t> ops;
        size_t maxChunks = 0;             // max chunk count of any one archetype
        uint64_t maxIndex = 0;            // max entity index handed out
        size_t defragRuns = 0, defragMoved = 0, defragChunksRemoved = 0;

        size_t CountPrefix(const std::string& prefix) const
        {
            size_t n = 0;
            for (const auto& [name, hits] : ops)
                if (name.compare(0, prefix.size(), prefix) == 0) n += hits;
            return n;
        }

        void Print() const
        {
            std::printf("[ fuzz ] op histogram:\n");
            for (const auto& [name, hits] : ops) std::printf("[ fuzz ]   %-40s %zu\n", name.c_str(), hits);
            std::printf("[ fuzz ] max chunks/archetype=%zu max entity index=%" PRIu64 " defragment runs=%zu moved=%zu chunks removed=%zu\n",
                        maxChunks, maxIndex, defragRuns, defragMoved, defragChunksRemoved);
            std::fflush(stdout);
        }
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

    // Decimal parse with an end-pointer + errno check. Garbage (or zero when !allowZero) is a
    // test failure, never a silent zero-seed pass.
    std::optional<uint64_t> EnvU64(const char* name, uint64_t fallback, bool allowZero)
    {
        std::string text;
#ifdef _MSC_VER
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return fallback;
        text = value;
        std::free(value);
#else
        const char* value = std::getenv(name);
        if (!value) return fallback;
        text = value;
#endif
        errno = 0;
        char* end = nullptr;
        const bool digits = !text.empty() && text[0] >= '0' && text[0] <= '9';   // no sign, no space
        const uint64_t parsed = digits ? std::strtoull(text.c_str(), &end, 10) : 0;
        if (!digits || errno != 0 || end == text.c_str() || *end != '\0')
        {
            ADD_FAILURE() << name << "='" << text << "' is not a decimal unsigned integer";
            return std::nullopt;
        }
        if (parsed == 0 && !allowZero)
        {
            ADD_FAILURE() << name << " must be > 0";
            return std::nullopt;
        }
        return parsed;
    }

    class StorageFuzzer
    {
    public:
        StorageFuzzer(uint64_t seed, FuzzStats* stats)
            : m_seed(seed), m_stats(stats), m_rng(seed), m_creg(FuzzComponents()),
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
                if (m_stats)
                {
                    ++m_stats->ops[m_op];
                    for (Astra::Archetype* arch : m_reg->GetAllArchetypes())
                        m_stats->maxChunks = std::max(m_stats->maxChunks, arch->GetChunkCount());
                }
                if (!Check()) return false;
            }
            return true;
        }

        // Destroys the registry: every ParityLiveS it held must be destructed exactly once. The
        // assert handler stays installed so a teardown assert still names the seed. False on a failure.
        bool Finish()
        {
            Astra::Testing::ScopedAssertHandler guard(&OnAssert, this);
            m_op = "teardown";
            m_reg.reset();
            bool ok = true;
            if (ParityLiveS::s_live != m_liveBase)
            {
                ADD_FAILURE() << "[ fuzz ] seed=" << m_seed << " teardown: ParityLiveS leaked or double-destroyed ("
                              << (ParityLiveS::s_live - m_liveBase) << " live beyond base)";
                ok = false;
            }
            if (m_unexpectedAsserts != 0)
            {
                ADD_FAILURE() << "[ fuzz ] seed=" << m_seed << " teardown: unexpected assert: " << m_lastUnexpected;
                ok = false;
            }
            return ok;
        }

    private:
        uint64_t m_seed;
        FuzzStats* m_stats;
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
            if (m_stats) m_stats->maxIndex = std::max<uint64_t>(m_stats->maxIndex, e.GetID());
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
            if (m_alive.size() > kMaxAlive) return OpShrink();   // a burst just ran: bulk-destroy back under the cap
            const uint32_t r = Rand(1000);
            if (m_alive.empty() || r < 120) return OpCreate();
            if (r < 170) return OpBatchCreate();
            if (r < 250) return OpDestroy();
            if (r < 270) return OpBatchDestroy();
            if (r < 470) return OpAdd();
            if (r < 620) return OpRemove();
            if (r < 700) return OpEnable();
            if (r < 790) return OpWrite();
            if (r < 840) return OpBatch();
            if (r < 870) return OpCommandBuffer();
            if (r < 890) return OpStaleProbe();
            if (r < 920) return OpDefragment();
            if (r < 927) return OpSaveLoad();
            if (r < 930) return OpClear();
            if (r < 933) return OpBurst();
            return OpRangeFor();
        }

        const char* OpDefragment()
        {
            const auto result = m_reg->Defragment();
            if (m_stats)
            {
                ++m_stats->defragRuns;
                m_stats->defragMoved += result.entitiesMoved;
                m_stats->defragChunksRemoved += result.chunksRemoved;
            }
            return "defragment";
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

        // kind: 0 = Velocity+PosS, 1 = LiveS+TagS, -1 = random.
        const char* CreateBatch(size_t k, int kind)
        {
            std::vector<Astra::Entity> out(k);
            if (kind == 0 || (kind < 0 && Chance(50)))
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

        const char* OpBatchCreate()
        {
            if (m_alive.size() >= kMaxAlive) return OpBatchDestroy();
            return CreateBatch(1 + Rand(48), -1);
        }

        // Rare: past one sparse page and four Universal pages. The Check after this op is the one
        // full-population comparison; OpShrink (the next Step) brings the population back down.
        const char* OpBurst()
        {
            const size_t want = kBurstTarget > m_alive.size() ? kBurstTarget - m_alive.size() : 0;
            if (want == 0) return "burst (none)";
            const size_t first = want / 2;
            CreateBatch(first, 0);
            CreateBatch(want - first, 1);
            return "burst-create";
        }

        const char* OpShrink()
        {
            const size_t drop = m_alive.size() - kMaxAlive / 2;
            std::vector<Astra::Entity> victims = PickDistinct(drop);
            m_reg->DestroyEntities(std::span{victims});
            for (Astra::Entity e : victims) { m_model.erase(e); NoteDead(e); }
            std::erase_if(m_alive, [&](Astra::Entity e) { return !m_model.contains(e); });
            return "shrink";
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

        static Hierarchy RandomHierarchy(StorageFuzzer& f)
        {
            Hierarchy h;
            h.parent = f.Rand(1000);
            h.firstChild = f.Rand(1000);
            h.nextSibling = f.Rand(1000);
            h.childCount = f.Rand(1000);
            return h;
        }

        const char* OpAdd()
        {
            const Astra::Entity e = PickAlive();
            ModelEntity& m = m_model[e];
            switch (Rand(10))
            {
            case 0: { const Velocity v{F(), F(), F()}; AddOne(e, v, m.vel.has_value()); if (!m.vel) m.vel = v; return "add Velocity"; }
            case 1: { Hierarchy h = RandomHierarchy(*this); AddOne(e, h, m.hier.has_value()); if (!m.hier) { m.hier = h; m.hierEnabled = true; } return "add Hierarchy"; }
            case 2: { AddOne(e, Player{}, m.player); m.player = true; return "add Player"; }
            case 3: { const ParityPosS p{F(), F(), F()}; AddOne(e, p, m.pos.has_value()); if (!m.pos) m.pos = p; return "add PosS"; }
            case 4: { ParityHierS h; h.parent = Rand(1000); AddOne(e, h, m.hs.has_value()); if (!m.hs) { m.hs = h; m.hsEnabled = true; } return "add HierS"; }
            case 5: { AddOne(e, ParityTagS{}, m.tag); m.tag = true; return "add TagS"; }   // a present tag keeps its enabled state
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
            if (kAssertsActive) EXPECT_EQ(m_refusals, before + 1) << "Debug must assert exactly once on a Universal remove";
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
            case 5: EXPECT_EQ(RemoveOne<ParityTagS>(e), m.tag); m.tag = false; m.tagEnabled = true; return "remove TagS";
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
            switch (Rand(5))
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
            case 3:
                EXPECT_EQ(m_reg->SetEnabled<ParityTagS>(e, enable), m.tag);   // Sparse enableable tag (bit-only mirror)
                if (m.tag) m.tagEnabled = enable;
                return "enable TagS";
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
            switch (Rand(8))
            {
            case 0: if (m.vel) { const float v = F(); m_reg->GetComponent<Velocity>(e)->dx = v; m.vel->dx = v; } return "write Velocity";
            case 1: if (m.pos) { const float v = F(); m_reg->GetComponent<ParityPosS>(e)->y = v; m.pos->y = v; } return "write PosS";
            case 2: if (m.hs) { const uint32_t v = Rand(1000); m_reg->GetComponent<ParityHierS>(e)->parent = v; m.hs->parent = v; } return "write HierS";
            case 3: if (m.live) { const int v = static_cast<int>(Rand(1000)); m_reg->GetComponent<ParityLiveS>(e)->value = v; m.live = v; } return "write LiveS";
            case 4: { std::string v = "w" + std::to_string(m_counter++); m_reg->GetComponent<Name>(e)->value = v; m.name = v; return "write Name"; }
            case 5:
            {
                Timer* t = m_reg->GetComponent<Timer>(e);
                t->duration = F(); t->elapsed = F(); t->loop = Chance(50); t->paused = Chance(50);
                m.timer = *t;
                return "write Timer";
            }
            default:
                if (m.hier)
                {
                    Hierarchy* h = m_reg->GetComponent<Hierarchy>(e);
                    h->firstChild = Rand(1000); h->nextSibling = Rand(1000); h->childCount = Rand(1000);
                    m.hier->firstChild = h->firstChild; m.hier->nextSibling = h->nextSibling; m.hier->childCount = h->childCount;
                }
                return "write Hierarchy";
            }
        }

        // Batch paths over Sparse (PosS / TagS / LiveS), Table (Velocity / Hierarchy / EnTag) and
        // the Universal batch-remove refusal. A batch add of a present component keeps its value.
        const char* OpBatch()
        {
            std::vector<Astra::Entity> es = PickDistinct(1 + Rand(32));
            switch (Rand(10))
            {
            case 0:
            {
                const ParityPosS p{F(), F(), F()};
                m_reg->AddComponents(std::span{es}, p);   // void: nothing to check, the model comparison is the check
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
                for (Astra::Entity e : es) m_model[e].tag = true;   // a present tag keeps its enabled state
                return "batch-emplace TagS";
            }
            case 3:
            {
                const ParityLiveS l(static_cast<int>(Rand(1000)));
                m_reg->AddComponents(std::span{es}, l);
                for (Astra::Entity e : es) if (!m_model[e].live) m_model[e].live = l.value;
                return "batch-add LiveS";
            }
            case 4:
            {
                const Velocity v{F(), F(), F()};
                m_reg->AddComponents(std::span{es}, v);
                for (Astra::Entity e : es) if (!m_model[e].vel) m_model[e].vel = v;
                return "batch-add Velocity (Table)";
            }
            case 5:
            {
                size_t had = 0;
                for (Astra::Entity e : es) had += m_model[e].vel.has_value();
                EXPECT_EQ(m_reg->RemoveComponents<Velocity>(std::span{es}), had);
                for (Astra::Entity e : es) m_model[e].vel.reset();
                return "batch-remove Velocity (Table)";
            }
            case 6:
            {
                const Hierarchy h = RandomHierarchy(*this);
                m_reg->AddComponents(std::span{es}, h);
                for (Astra::Entity e : es) if (!m_model[e].hier) { m_model[e].hier = h; m_model[e].hierEnabled = true; }
                return "batch-add Hierarchy (Table enableable)";
            }
            case 7:
            {
                size_t had = 0;
                for (Astra::Entity e : es) had += m_model[e].hier.has_value();
                EXPECT_EQ(m_reg->RemoveComponents<Hierarchy>(std::span{es}), had);
                for (Astra::Entity e : es) { m_model[e].hier.reset(); m_model[e].hierEnabled = true; }
                return "batch-remove Hierarchy (Table enableable)";
            }
            case 8:
            {
                m_reg->EmplaceComponents<EnTag>(std::span{es});
                for (Astra::Entity e : es) if (!m_model[e].enTag) { m_model[e].enTag = true; m_model[e].enTagEnabled = true; }
                return "batch-emplace EnTag (Table tag)";
            }
            default:
            {
                // Task 12 helper: the Universal batch remove refuses ONCE per call and removes nothing.
                const int before = m_refusals;
                m_expectRefusal = true;
                const size_t removed = Chance(50) ? m_reg->RemoveComponents<Name>(std::span{es})
                                                  : m_reg->RemoveComponents<Timer>(std::span{es});
                m_expectRefusal = false;
                EXPECT_EQ(removed, 0u);
                if (kAssertsActive) EXPECT_EQ(m_refusals, before + 1) << "Debug must assert exactly once on a Universal batch remove";
                return "batch-remove Universal (refused)";
            }
            }
        }

        // A recorded Universal remove is refused at execute: the buffer reports a failure and
        // (Debug) exactly one assert fires.
        const char* OpCommandBufferRefusedRemove()
        {
            const Astra::Entity e = PickAlive();
            Astra::CommandBuffer cmd(m_reg.get());
            if (Chance(50)) cmd.RemoveComponent<Name>(e);
            else            cmd.RemoveComponent<Timer>(e);
            const int before = m_refusals;
            m_expectRefusal = true;
            const auto result = cmd.Execute();
            m_expectRefusal = false;
            EXPECT_FALSE(result.IsOk()) << "a recorded Universal remove must fail at Execute";
            if (kAssertsActive) EXPECT_EQ(m_refusals, before + 1) << "Debug must assert exactly once on a recorded Universal remove";
            return "command-buffer Universal remove (refused)";
        }

        // Recorded commands apply in record order at Execute (eager CommandBuffer).
        const char* OpCommandBuffer()
        {
            if (Chance(15)) return OpCommandBufferRefusedRemove();
            Astra::CommandBuffer cmd(m_reg.get());
            const Astra::Entity t = cmd.CreateEntity();
            ParityPosS p{F(), F(), F()};   // non-const lvalues (Component concept)
            Velocity tv{F(), F(), F()};
            cmd.AddComponent(t, p);
            cmd.AddComponent(t, tv);       // a Table component recorded on a created entity
            std::vector<Astra::Entity> ab = PickDistinct(2);
            bool destroyB = false;
            bool tagEn = true, timerEn = true;
            Velocity av{F(), F(), F()};
            if (ab.size() == 2)
            {
                if (!m_model[ab[0]].tag) cmd.AddComponent(ab[0], ParityTagS{});   // a recorded add of a present tag fails Execute
                tagEn = Chance(50);
                cmd.SetEnabled<ParityTagS>(ab[0], tagEn);                         // present after the add above
                if (m_model[ab[0]].live) cmd.RemoveComponent<ParityLiveS>(ab[0]);
                else                     cmd.AddComponent(ab[0], ParityLiveS(7));
                if (m_model[ab[0]].vel)  cmd.RemoveComponent<Velocity>(ab[0]);    // Table remove / add
                else                     cmd.AddComponent(ab[0], av);
                if (m_model[ab[1]].hs) cmd.SetEnabled<ParityHierS>(ab[1], false);
                timerEn = Chance(50);
                cmd.SetEnabled<Timer>(ab[1], timerEn);                            // Universal SetEnabled
                destroyB = Chance(30);
                if (destroyB) cmd.DestroyEntity(ab[1]);
            }
            EXPECT_TRUE(cmd.Execute().IsOk()) << "CommandBuffer playback failed";

            ModelEntity mt;
            mt.pos = p;
            mt.vel = tv;
            mt.name = InitName(t);              // created through the funnel: universal init ran
            Track(t, std::move(mt));
            if (ab.size() == 2)
            {
                ModelEntity& a = m_model[ab[0]];
                a.tag = true;
                a.tagEnabled = tagEn;
                if (a.live) a.live.reset(); else a.live = 7;
                if (a.vel) a.vel.reset(); else a.vel = av;
                if (m_model[ab[1]].hs) m_model[ab[1]].hsEnabled = false;
                m_model[ab[1]].timerEnabled = timerEn;
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
            EXPECT_FALSE(m_reg->SetEnabled<ParityTagS>(d, false));
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
            if ((hier != nullptr) != m.hier.has_value() || r.HasComponent<Hierarchy>(e) != m.hier.has_value()) return "Hierarchy presence";
            if (hier && (hier->parent != m.hier->parent || hier->firstChild != m.hier->firstChild
                         || hier->nextSibling != m.hier->nextSibling || hier->childCount != m.hier->childCount)) return "Hierarchy value";
            if (r.IsEnabled<Hierarchy>(e) != (m.hier.has_value() && m.hierEnabled)) return "Hierarchy enabled";

            if (r.HasComponent<Player>(e) != m.player) return "Player presence";
            if (r.HasComponent<EnTag>(e) != m.enTag) return "EnTag presence";
            if (r.IsEnabled<EnTag>(e) != (m.enTag && m.enTagEnabled)) return "EnTag enabled";

            const ParityPosS* pos = r.GetComponent<ParityPosS>(e);
            if ((pos != nullptr) != m.pos.has_value() || r.HasComponent<ParityPosS>(e) != m.pos.has_value()) return "PosS presence";
            if (pos && !(*pos == *m.pos)) return "PosS value";

            const ParityHierS* hs = r.GetComponent<ParityHierS>(e);
            if ((hs != nullptr) != m.hs.has_value() || r.HasComponent<ParityHierS>(e) != m.hs.has_value()) return "HierS presence";
            if (hs && hs->parent != m.hs->parent) return "HierS value";
            if (r.IsEnabled<ParityHierS>(e) != (m.hs.has_value() && m.hsEnabled)) return "HierS enabled";

            if (r.HasComponent<ParityTagS>(e) != m.tag) return "TagS presence";
            if (r.IsEnabled<ParityTagS>(e) != (m.tag && m.tagEnabled)) return "TagS enabled";

            const ParityLiveS* live = r.GetComponent<ParityLiveS>(e);
            if ((live != nullptr) != m.live.has_value() || r.HasComponent<ParityLiveS>(e) != m.live.has_value()) return "LiveS presence";
            if (live && live->value != *m.live) return "LiveS value";

            const Name* name = r.GetComponent<Name>(e);
            if (!name || !r.HasComponent<Name>(e)) return "Name (universal) missing";
            if (name->value != m.name) return "Name value: got '" + name->value + "' want '" + m.name + "'";

            const Timer* timer = r.GetComponent<Timer>(e);
            if (!timer || !r.HasComponent<Timer>(e)) return "Timer (universal) missing";
            if (timer->duration != m.timer.duration || timer->elapsed != m.timer.elapsed
                || timer->loop != m.timer.loop || timer->paused != m.timer.paused) return "Timer value";
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
            size_t live = 0, pos = 0, velPos = 0, velNoTag = 0, hsOn = 0, hsAll = 0, timerOn = 0, tagOn = 0, tagAll = 0, vel = 0, hierOn = 0, enTagOn = 0;
            for (const auto& [e, m] : m_model)
            {
                if (std::string why = CompareEntity(e, m); !why.empty())
                    return Fail("entity " + std::to_string(e.GetID()) + "v" + std::to_string(e.GetVersion()) + ": " + why);
                live += m.live.has_value();
                pos += m.pos.has_value();
                velPos += (m.vel && m.pos);
                velNoTag += (m.vel && !m.tag);   // Not<> tests PRESENCE (spec s6.3: Not<Sparse> adds `present` to exclude): a disabled tag is still present
                hsOn += (m.hs && m.hsEnabled);
                hsAll += m.hs.has_value();
                timerOn += m.timerEnabled;
                tagOn += (m.tag && m.tagEnabled);   // a required enableable argument excludes its disabled mirror
                tagAll += m.tag;
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
                && expectSize(m_reg->CreateView<ParityTagS>().Size(), tagOn, "View<TagS>")
                && expectSize(m_reg->CreateView<Astra::IncludeDisabled<ParityTagS>>().Size(), tagAll, "View<IncludeDisabled<TagS>>")
                && expectSize(m_reg->CreateView<Velocity, Astra::Optional<ParityLiveS>>().Size(), vel, "View<Velocity,Optional<LiveS>>")
                && expectSize(m_reg->CreateView<Hierarchy>().Size(), hierOn, "View<Hierarchy>")
                && expectSize(m_reg->CreateView<Velocity, EnTag>().Size(), enTagOn, "View<Velocity,EnTag>");
        }
    };

    // One seed end to end; false on the first failure (Run or teardown).
    bool RunSeed(uint64_t seed, size_t ops, FuzzStats* stats)
    {
        StorageFuzzer fuzzer(seed, stats);
        const bool ran = fuzzer.Run(ops);
        const bool finished = fuzzer.Finish();
        return ran && finished;
    }
} // namespace

TEST(StorageInvariantFuzz, FixedSeedsShortRun)
{
    constexpr size_t kOps = 1000;   // brief floor is 1000; measured 19.8 s Debug for the 4 seeds incl. bursts (2026-10-23, desk)
    FuzzStats stats;
    for (uint64_t seed : {1ull, 2ull, 3ull, 0xA57A5EEDull})
    {
        std::printf("[ fuzz ] seed=%" PRIu64 " ops=%zu\n", seed, kOps);
        std::fflush(stdout);
        if (!RunSeed(seed, kOps, &stats)) return;   // the failure message carries seed + op index
    }
    stats.Print();
    // The structural ops and the page-crossing burst must actually have run.
    for (const char* op : {"clear", "command-buffer", "defragment", "burst-create", "shrink", "save-load", "enable TagS"})
        EXPECT_GT(stats.CountPrefix(op), 0u) << "op never reached in the short run: " << op;
    EXPECT_GT(stats.maxIndex, 4096u) << "no entity index past the 4096 sparse page";
}

TEST(StorageInvariantFuzz, DISABLED_LongRun)
{
    const auto first = EnvU64("ASTRA_FUZZ_SEED", 1000, /*allowZero*/ true);
    const auto count = EnvU64("ASTRA_FUZZ_SEEDS", 64, false);
    const auto ops = EnvU64("ASTRA_FUZZ_OPS", 20000, false);
    if (!first || !count || !ops) return;   // EnvU64 recorded the failure
    ASSERT_LE(*count - 1, UINT64_MAX - *first) << "ASTRA_FUZZ_SEED + ASTRA_FUZZ_SEEDS overflows";
    FuzzStats stats;
    for (uint64_t i = 0; i < *count; ++i)
    {
        const uint64_t seed = *first + i;
        std::printf("[ fuzz ] seed=%" PRIu64 " ops=%" PRIu64 "\n", seed, *ops);
        std::fflush(stdout);
        if (!RunSeed(seed, static_cast<size_t>(*ops), &stats)) return;
    }
    stats.Print();
}
