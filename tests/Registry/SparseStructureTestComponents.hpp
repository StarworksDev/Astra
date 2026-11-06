#pragma once

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <cstdint>
#include <string>
#include <utility>

// Storage-policies Tasks 16-21: test components + helpers. Six types only: every
// test in the binary shares one TypeID space of ASTRA_MAX_COMPONENTS ids (see the
// budget note in tests/TestComponents.hpp). Table parts use Astra::Test::Position/Velocity.
namespace Astra::Test::SparseC
{
    struct SpA
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        float v = 0.0f;
        SpA() = default;
        explicit SpA(float x) : v(x) {}
    };

    struct SpB
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        int v = 0;
        SpB() = default;
        explicit SpB(int x) : v(x) {}
        bool operator==(const SpB&) const = default;   // Chunk D's query tests compare values
    };

    // Sparse + enableable + change-tracked (Tasks 19-20).
    struct SpE
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraEnableable = true;
        static constexpr bool AstraChangeTracked = true;
        float v = 0.0f;
        SpE() = default;
        explicit SpE(float x) : v(x) {}
        bool operator==(const SpE&) const = default;
    };

    // Sparse tag (presence only); enableable (spec s3.2: every tag may be).
    struct SpTag
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        static constexpr bool AstraEnableable = true;
    };

    // Non-trivially-copyable sparse value: MoveConstruct/Destruct paths.
    struct SpStr
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
        std::string s;
        SpStr() = default;
        explicit SpStr(std::string v) : s(std::move(v)) {}

        template<typename Archive>
        void Serialize(Archive& ar) { ar(s); }   // ComponentRegistry erases Serialize for every registered type
    };

    // Table by trait; tests make it Universal with Config::AddUniversal<UnE>().
    struct UnE
    {
        static constexpr bool AstraEnableable = true;
        float v = 0.0f;
    };

    inline std::pair<Astra::ArchetypeChunk*, size_t> Locate(Astra::Registry& reg, Astra::Entity e)
    {
        const Astra::EntityRecord* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        if (!rec || !rec->chunk) return {nullptr, size_t(0)};
        return {rec->chunk, rec->location.GetEntityIndex()};
    }

    template<typename T>
    inline int SlotOf(Astra::Registry& reg)
    {
        return reg.GetComponentStorage()->SparseSlotOf(Astra::TypeID<T>::Value());
    }

    // T's presence bit for e, read straight from e's chunk (P1 observation).
    template<typename T>
    inline bool PresentBit(Astra::Registry& reg, Astra::Entity e)
    {
        auto [chunk, idx] = Locate(reg, e);
        const int slot = SlotOf<T>(reg);
        return chunk && slot >= 0 && chunk->IsSparsePresent(static_cast<uint16_t>(slot), idx);
    }

    // T's disabled-mirror bit for e (P3 observation).
    template<typename T>
    inline bool DisabledBit(Astra::Registry& reg, Astra::Entity e)
    {
        auto [chunk, idx] = Locate(reg, e);
        const int slot = SlotOf<T>(reg);
        return chunk && slot >= 0 && chunk->IsSparseDisabled(static_cast<uint16_t>(slot), idx);
    }

    // P1-P3 must hold: the validator's report is printed on failure.
    inline void ExpectInvariants(const Astra::Registry& reg)
    {
        const std::string report = reg.ValidateStorageInvariants();
        EXPECT_TRUE(report.empty()) << report;
    }
}
