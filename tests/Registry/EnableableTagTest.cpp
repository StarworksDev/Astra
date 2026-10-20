#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>

#include <bit>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "../TestComponents.hpp"

// Enableable Table tags (spec 2026-10-01 s3.2, second bullet): an enableable EMPTY type
// gets a bit-only column -- stride 0, no data region, a disabled-word region -- so
// SetEnabled / IsEnabled / view filtering / every bit carry behave exactly as for an
// enableable data component (parity). EnTag is the suite's one enableable tag.

using Astra::Test::EnTag;
using Astra::Test::Player;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace
{
    // disabledCount == popcount(words) and no bit at a slot >= count, on every enableable
    // column of every chunk (RegistryTest.cpp's invariant seam, restated for this suite).
    void ExpectBitInvariant(Astra::Registry& reg)
    {
        for (Astra::Archetype* arch : reg.GetArchetypeManager()->GetArchetypes())
        {
            const Astra::ArchetypeColumnMeta& meta = arch->GetColumnMeta();
            for (const auto& chunkPtr : arch->GetChunks())
            {
                Astra::ArchetypeChunk& chunk = *chunkPtr;
                const size_t words = (chunk.GetCapacity() + 63) / 64;
                for (uint16_t e = 0; e < meta.enableableColumnCount; ++e)
                {
                    const int col = static_cast<int>(meta.enableableColumns[e]);
                    const uint64_t* w = chunk.GetDisabledWords(col);
                    ASSERT_NE(w, nullptr) << "enableable column " << col << " must carry disabled words";
                    uint32_t pop = 0;
                    for (size_t i = 0; i < words; ++i)
                        pop += static_cast<uint32_t>(std::popcount(w[i]));
                    EXPECT_EQ(pop, chunk.GetDisabledCount(col));
                    for (size_t i = chunk.GetCount(); i < words * 64; ++i)
                        EXPECT_FALSE(chunk.IsDisabled(col, i)) << "tail bit at slot " << i;
                }
            }
        }
    }

    const Astra::EntityRecord* RecordOf(Astra::Registry& reg, Astra::Entity e)
    {
        return reg.GetArchetypeManager()->GetEntityRecord(e);
    }
}

TEST(EnableableTag, RegistersAndGetsABitOnlyColumn)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity<Position, EnTag, Player>();
    const Astra::ComponentID tagId = Astra::TypeID<EnTag>::Value();
    const auto* desc = reg.GetComponentRegistry()->GetComponentDescriptor(tagId);
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(desc->is_empty);
    EXPECT_EQ(desc->size, 0u);
    EXPECT_TRUE(desc->isEnableable);

    const Astra::EntityRecord* rec = RecordOf(reg, e);
    ASSERT_NE(rec, nullptr);
    const Astra::ArchetypeColumnMeta& meta = rec->archetype->GetColumnMeta();
    const int col = meta.idToColumn[tagId];
    ASSERT_GE(col, 0);                                            // the tag HAS a column...
    EXPECT_EQ(meta.columns[col].stride, 0u);                      // ...that carries no data
    EXPECT_EQ(meta.enableableColumnCount, 1u);
    EXPECT_EQ(meta.idToColumn[Astra::TypeID<Player>::Value()], -1);   // a non-enableable tag still has none
    EXPECT_EQ(rec->chunk->GetComponentArray<EnTag>(), nullptr);   // no data region
    EXPECT_NE(rec->chunk->GetComponentArrayByID(tagId), nullptr); // in-arena base, never dereferenced
    EXPECT_NE(rec->chunk->GetDisabledWords(col), nullptr);        // but a disabled-word region
}

TEST(EnableableTag, ToggleBehavesLikeAnEnableableDataComponent)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentEnabled);
    reg.EnableSignals(Astra::Signal::ComponentDisabled);
    int enabled = 0, disabled = 0;
    void* lastPayload = nullptr;
    reg.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const auto& ev) { ++disabled; lastPayload = ev.component; });
    reg.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const auto& ev) { ++enabled; lastPayload = ev.component; });

    const Astra::Entity e = reg.CreateEntity<Position, EnTag>();
    EXPECT_TRUE(reg.IsEnabled<EnTag>(e));                         // born enabled
    EXPECT_TRUE(reg.SetEnabled<EnTag>(e, false));
    EXPECT_FALSE(reg.IsEnabled<EnTag>(e));
    EXPECT_TRUE(reg.SetEnabled<EnTag>(e, false));                 // idempotent: true (present), silent
    EXPECT_TRUE(reg.HasComponent<EnTag>(e));                      // existence never lies
    EXPECT_NE(reg.GetComponent<EnTag>(e), nullptr);
    EXPECT_TRUE(reg.SetEnabled<EnTag>(e, true));
    EXPECT_EQ(disabled, 1);
    EXPECT_EQ(enabled, 1);
    EXPECT_EQ(lastPayload, Astra::EmptyComponentSentinel());      // by-ID tag payload convention

    const Astra::Entity bare = reg.CreateEntity<Position>();
    EXPECT_FALSE(reg.SetEnabled<EnTag>(bare, false));             // absent
    EXPECT_FALSE(reg.IsEnabled<EnTag>(bare));
    reg.DestroyEntity(e);
    EXPECT_FALSE(reg.SetEnabled<EnTag>(e, true));                 // stale handle
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BitSurvivesSwapRemoveIncludingTheTailItself)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i)
        es.push_back(reg.CreateEntity<Position, EnTag>());
    reg.SetEnabled<EnTag>(es[7], false);                          // the tail slot
    reg.DestroyEntity(es[2]);                                     // the disabled tail swaps into slot 2
    EXPECT_FALSE(reg.IsEnabled<EnTag>(es[7]));
    reg.SetEnabled<EnTag>(es[6], false);
    reg.DestroyEntity(es[6]);                                     // the removed slot IS the tail now
    EXPECT_FALSE(reg.IsEnabled<EnTag>(es[7]));
    for (int i : {0, 1, 3, 4, 5})
        EXPECT_TRUE(reg.IsEnabled<EnTag>(es[i])) << i;
    const Astra::EntityRecord* rec = RecordOf(reg, es[7]);
    const int col = rec->archetype->GetColumnMeta().idToColumn[Astra::TypeID<EnTag>::Value()];
    EXPECT_EQ(rec->chunk->GetDisabledCount(col), 1u);
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BitSurvivesArchetypeTransitionsAndAddsBornEnabled)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity<Position, EnTag>();
    reg.SetEnabled<EnTag>(e, false);
    reg.AddComponent(e, Velocity{1.0f, 2.0f, 3.0f});              // MoveAndAdd: shared bit-only column carries
    EXPECT_FALSE(reg.IsEnabled<EnTag>(e));
    reg.RemoveComponent<Velocity>(e);                             // MoveEntityFrom: carries back
    EXPECT_FALSE(reg.IsEnabled<EnTag>(e));
    EXPECT_TRUE(reg.RemoveComponent<EnTag>(e));
    EXPECT_FALSE(reg.HasComponent<EnTag>(e));
    EXPECT_TRUE(reg.AddComponent(e, EnTag{}));                    // MoveAndAdd<EnTag>: the new column, born enabled
    EXPECT_TRUE(reg.IsEnabled<EnTag>(e));

    const Astra::Entity f = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.AddComponentByID(f, Astra::TypeID<EnTag>::Value(), nullptr, 0));   // MoveAndAddByID, null data
    EXPECT_TRUE(reg.HasComponent<EnTag>(f));
    EXPECT_TRUE(reg.IsEnabled<EnTag>(f));
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BatchAddLeavesNeighbourDataIntact)
{
    Astra::Registry reg;
    constexpr size_t N = 200;
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg.CreateEntitiesWith<Position>(N, std::span{es}, [](size_t i)
        { return std::tuple{Position{static_cast<float>(i), static_cast<float>(i) + 0.5f, -static_cast<float>(i)}}; })), N);
    std::vector<Astra::Entity> odd;
    for (size_t i = 1; i < N; i += 2)
        odd.push_back(es[i]);
    // Archetype::SetComponents -> ArchetypeChunk::BatchConstructComponent<EnTag>: must not
    // write sizeof(EnTag) bytes through the bit-only column's base.
    reg.AddComponents<EnTag>(std::span<Astra::Entity>(odd.data(), odd.size()), EnTag{});
    for (size_t i = 0; i < N; ++i)
    {
        const Position* p = reg.GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->x, static_cast<float>(i));
        EXPECT_EQ(p->y, static_cast<float>(i) + 0.5f);
        EXPECT_EQ(p->z, -static_cast<float>(i));
        EXPECT_EQ(reg.HasComponent<EnTag>(es[i]), (i % 2) == 1) << i;
        if ((i % 2) == 1)
            EXPECT_TRUE(reg.IsEnabled<EnTag>(es[i])) << i;
    }
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, ViewsFilterDisabledTagsInMixedAndTagOnlyArchetypes)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 10; ++i)
        es.push_back(reg.CreateEntity<Position, EnTag>());
    for (int i : {0, 3, 9})
        reg.SetEnabled<EnTag>(es[i], false);

    size_t n = 0;
    reg.CreateView<Position, EnTag>().ForEach([&](Astra::Entity e, Position&, EnTag&)
    {
        ++n;
        EXPECT_TRUE(reg.IsEnabled<EnTag>(e));
    });
    EXPECT_EQ(n, 7u);
    EXPECT_EQ((reg.CreateView<Position, EnTag>().Size()), 7u);

    size_t all = 0;
    reg.CreateView<Position, Astra::IncludeDisabled<EnTag>>().ForEach([&](Astra::Entity, Position&, EnTag&) { ++all; });
    EXPECT_EQ(all, 10u);

    // Tag-only archetype (zero per-entity size: entities live in the chunk's side vector,
    // capacity chunkBytes/64) -- the bit-only column's words must still be carved.
    std::vector<Astra::Entity> tagOnly;
    for (int i = 0; i < 70; ++i)
        tagOnly.push_back(reg.CreateEntity<EnTag>());
    reg.SetEnabled<EnTag>(tagOnly[64], false);                    // second word
    size_t m = 0;
    reg.CreateView<EnTag>().ForEach([&](Astra::Entity, EnTag&) { ++m; });
    EXPECT_EQ(m, 7u + 69u);                                       // both archetypes, disabled excluded
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, BitSurvivesChurnAndDefragment)
{
    Astra::Registry reg;
    constexpr size_t N = 3000;                                    // spans chunks
    std::vector<Astra::Entity> es(N);
    ASSERT_EQ((reg.CreateEntitiesWith<Position, EnTag>(N, std::span{es},
        [](size_t) { return std::tuple{Position{}, EnTag{}}; })), N);
    std::vector<bool> expected(N, true);
    for (size_t i = 0; i < N; i += 3)
    {
        reg.SetEnabled<EnTag>(es[i], false);
        expected[i] = false;
    }
    for (size_t i = 1; i < N; i += 4)
        reg.DestroyEntity(es[i]);                                 // fragment: swap-removes across every chunk
    for (size_t i = 2; i < N; i += 5)
    {
        reg.AddComponent(es[i], Velocity{});                      // MoveAndAdd + MoveEntityFrom round trip
        reg.RemoveComponent<Velocity>(es[i]);
    }

    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;                        // CompactChunks on every multi-chunk archetype
    reg.Defragment(opts);

    for (size_t i = 0; i < N; ++i)
    {
        if ((i % 4) == 1)
            continue;                                             // destroyed
        EXPECT_EQ(reg.IsEnabled<EnTag>(es[i]), expected[i]) << i;
    }
    ExpectBitInvariant(reg);
}

TEST(EnableableTag, HasNoValueForChangeDetection)
{
    Astra::Registry reg;
    const Astra::Entity e = reg.CreateEntity<Position, EnTag>();
    EXPECT_FALSE(reg.IsChanged<EnTag>(e, 0));                     // "false for ... a tag" (unchanged contract)
    EXPECT_FALSE(reg.IsAdded<EnTag>(e, 0));
    EXPECT_FALSE(reg.Modified<EnTag>(e));
    EXPECT_FALSE(reg.Modified(e, Astra::TypeID<EnTag>::Value()));
    EXPECT_TRUE(reg.Modified<Position>(e));                       // the data column beside it still answers
}

TEST(EnableableTag, InspectorReadsTheBitOnlyColumnAsZeroBytes)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i)
        reg.CreateEntity<Position, EnTag>();
    const Astra::Debug::InspectorSnapshot snap = Astra::Debug::Capture(reg);
    bool sawBitOnly = false;
    for (const Astra::Debug::ArchetypeInfo& arch : snap.archetypes)
    {
        for (size_t c = 0; c < arch.columns.size(); ++c)
        {
            if (arch.columns[c].stride != 0)
                continue;
            sawBitOnly = true;
            for (const Astra::Debug::ChunkInfo& ch : arch.chunks)
            {
                EXPECT_EQ(ch.columns[c].bytes, 0u);
                EXPECT_NE(ch.columns[c].disabledOffset, SIZE_MAX);   // its words region is reported
            }
        }
        for (const Astra::Debug::ChunkInfo& ch : arch.chunks)
            EXPECT_LE(ch.padBytes, ch.chunkBytes);                // no unsigned underflow in the layout walk
    }
    EXPECT_TRUE(sawBitOnly);
}
