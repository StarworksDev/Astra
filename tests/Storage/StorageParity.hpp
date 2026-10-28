#pragma once

// Storage-parity support (spec 2026-10-01 s9.1). The SAME behavioural test bodies run
// over three variants of one component that differ only in storage:
//   Table     -- the Table-trait Astra::Test type in a plain registry;
//   Sparse    -- a field-identical twin declaring `AstraStorage = Sparse`;
//   Universal -- the Table-trait type in a registry whose Config declares it universal.
//
// TypeID BUDGET (see tests/TestComponents.hpp and premake5.lua's ASTRA_MAX_COMPONENTS=192
// note): the AstraTest process mints ComponentIDs process-wide across every suite. The
// Table and Universal variants REUSE existing Astra::Test types (no new id), and so do two of
// the Sparse twins (PosS from Task 3, SpTag from chunk C). Only THREE types below are new
// (ParityHierS, ParityTrackedS, ParityLiveS), and they are shared by the parity suites, the invariant
// fuzzer and the Inspector tests. Do NOT add a type here without re-running the full suite
// (Task 37 Step 6): an id >= ASTRA_MAX_COMPONENTS dies in Bitmap's "index < Bits" guard.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <Astra/Astra.hpp>
#include "../Registry/SparseStructureTestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"
#include "../TestComponents.hpp"

namespace Astra::Test::Parity
{
    // Sparse twin of Astra::Test::Position (same fields, ctor and equality): REUSED from
    // Task 3 (tests/TestComponents.hpp), no new id.
    using ParityPosS = Astra::Test::PosS;

    // Sparse twin of Astra::Test::Hierarchy (enableable, same fields).
    struct ParityHierS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraEnableable = true;
        uint32_t parent = 0;
        uint32_t firstChild = 0;
        uint32_t nextSibling = 0;
        uint32_t childCount = 0;
        ParityHierS() = default;
    };

    // Sparse ENABLEABLE tag (presence only, no data buffer; s3.2): REUSED from chunk C
    // (tests/Registry/SparseStructureTestComponents.hpp), no new id. Its Table twin is
    // Astra::Test::EnTag (Task 6), the enableable Table tag with a bit-only column.
    using ParityTagS = Astra::Test::SparseC::SpTag;

    // Sparse twin of Astra::Test::TrackedPos (change-tracked: per-element ticks in the store).
    struct ParityTrackedS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraChangeTracked = true;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        ParityTrackedS() = default;
        ParityTrackedS(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
        bool operator==(const ParityTrackedS&) const = default;
    };

    // Sparse, non-trivial, lifetime-counted (fuzzer leak / double-destroy accounting):
    // s_live == number of live instances anywhere in the process.
    struct ParityLiveS
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static inline int s_live = 0;
        int value = 0;

        ParityLiveS() { ++s_live; }
        explicit ParityLiveS(int v) : value(v) { ++s_live; }
        ParityLiveS(const ParityLiveS& o) : value(o.value) { ++s_live; }
        ParityLiveS(ParityLiveS&& o) noexcept : value(o.value) { ++s_live; }
        ParityLiveS& operator=(const ParityLiveS& o) { value = o.value; return *this; }
        ParityLiveS& operator=(ParityLiveS&& o) noexcept { value = o.value; return *this; }
        ~ParityLiveS() { --s_live; }

        // Required so RegisterComponents<ParityLiveS> compiles (see Tracked's note in
        // TestComponents.hpp: non-trivially-copyable types need a Serialize).
        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(value);
        }
    };

    static_assert(Astra::StorageOfV<ParityPosS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<ParityHierS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<ParityTagS> == Astra::Storage::Sparse && std::is_empty_v<ParityTagS> && Astra::IsEnableableV<ParityTagS>);
    static_assert(Astra::StorageOfV<Astra::Test::EnTag> == Astra::Storage::Table && Astra::IsEnableableV<Astra::Test::EnTag>);
    static_assert(Astra::StorageOfV<ParityTrackedS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<ParityLiveS> == Astra::Storage::Sparse);
    static_assert(Astra::StorageOfV<Astra::Test::Position> == Astra::Storage::Table);

    // ---- Variants ------------------------------------------------------------------

    template<class C, Astra::StorageKind K>
    struct Variant
    {
        using Comp = C;
        static constexpr Astra::StorageKind kind = K;
        static constexpr bool kUniversal = (K == Astra::StorageKind::Universal);
        static constexpr bool kSparse = (K == Astra::StorageKind::Sparse);
        static constexpr const char* kName = kUniversal ? "Universal" : (kSparse ? "Sparse" : "Table");

        // The trait must agree with the variant: Sparse twins declare Sparse; the Table and
        // Universal variants use a Table-trait type (Universal is a per-registry override).
        static_assert(kSparse == (Astra::StorageOfV<C> == Astra::Storage::Sparse),
                      "variant kind and the component's AstraStorage trait disagree");

        static Astra::Registry::Config MakeConfig(Astra::Registry::Config config = {})
        {
            if constexpr (kUniversal)
                config.AddUniversal<C>();
            return config;
        }

        // A FRESH ComponentRegistry with C and the suites' Table companions registered, so
        // a universal declaration always finds its descriptor and a Load always resolves.
        static std::shared_ptr<Astra::ComponentRegistry> MakeComponentRegistry()
        {
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<C, Astra::Test::Velocity, Astra::Test::Health>();
            return creg;
        }

        static std::unique_ptr<Astra::Registry> Make(Astra::Registry::Config config = {})
        {
            return std::make_unique<Astra::Registry>(MakeComponentRegistry(), MakeConfig(std::move(config)));
        }
    };

    template<class C> using TableV     = Variant<C, Astra::StorageKind::Table>;
    template<class C> using SparseV    = Variant<C, Astra::StorageKind::Sparse>;
    template<class C> using UniversalV = Variant<C, Astra::StorageKind::Universal>;

    // gtest typed-suite name generator: StorageParityPos/Table.*, /Sparse.*, /Universal.*
    struct VariantNames
    {
        template<class V>
        static std::string GetName(int) { return V::kName; }
    };

    // ---- Values --------------------------------------------------------------------

    // Position-shaped (Position, ParityPosS, TrackedPos, ParityTrackedS).
    template<class C>
    C MakePos(float v) { return C{v, v + 1.0f, v + 2.0f}; }

    // Hierarchy-shaped (Hierarchy, ParityHierS). Not aggregates (user-declared ctor).
    template<class C>
    C MakeHier(uint32_t v)
    {
        C c;
        c.parent = v;
        c.childCount = v + 1u;
        return c;
    }

    // "Give" C to an entity. Table/Sparse: AddComponent. Universal: the component is
    // already present (Add would be a no-op returning true, s3.3), so giving = writing.
    template<class V, class C>
    void Give(Astra::Registry& reg, Astra::Entity e, const C& value)
    {
        if constexpr (V::kUniversal)
        {
            C* slot = reg.GetComponent<C>(e);
            ASSERT_NE(slot, nullptr) << "a live entity must hold its universal component";
            *slot = value;
        }
        else
        {
            ASSERT_TRUE(reg.AddComponent(e, value));
        }
    }

    // Creates entities through `make` until one lands on `dead`'s index (ids are recycled
    // at version+1 since T1-C2), destroys the spill, and returns the new occupant (or
    // Invalid if the index never came back within 4096 creates).
    template<class MakeFn>
    Astra::Entity RecycleIndex(Astra::Registry& reg, Astra::Entity dead, MakeFn&& make)
    {
        std::vector<Astra::Entity> spill;
        for (int i = 0; i < 4096; ++i)
        {
            Astra::Entity e = make(reg);
            if (e.GetID() == dead.GetID())
            {
                for (Astra::Entity s : spill) reg.DestroyEntity(s);
                return e;
            }
            spill.push_back(e);
        }
        for (Astra::Entity s : spill) reg.DestroyEntity(s);
        return Astra::Entity::Invalid();
    }

    // ---- Assert capture (Universal refusal, iteration-safety guard) -----------------

    struct AssertCount
    {
        int count = 0;
        std::string lastMessage;
    };

    // Records and CONTINUES, so an ASTRA_ASSERT returns to the call site (which then
    // takes its Release path, e.g. `return false`). Install with
    // Astra::Testing::ScopedAssertHandler for one scope only: a globally installed
    // continue-handler would hide genuine failures elsewhere.
    inline Astra::AssertAction CountAndContinue(const Astra::AssertContext& ctx, void* user) noexcept
    {
        auto* c = static_cast<AssertCount*>(user);
        ++c->count;
        c->lastMessage = ctx.message ? ctx.message : "";
        return Astra::AssertAction::Continue;
    }

    // ASTRA_ASSERT is live exactly when MOSAIC_ASSERTS_ACTIVE (NDEBUG undefined, or a
    // checked-Release build via MOSAIC_ENABLE_ASSERTS / ASTRA_ENABLE_ASSERTS; RULINGS Task 37).
    inline constexpr bool kAssertsActive = (MOSAIC_ASSERTS_ACTIVE != 0);
} // namespace Astra::Test::Parity
