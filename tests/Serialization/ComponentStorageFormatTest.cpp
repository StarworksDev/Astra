#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../TestComponents.hpp"
#include "../Support/StorageOverride.hpp"
#include "Astra/Registry/Registry.hpp"

namespace
{
    using namespace Astra::Test;

    struct BlockHeader { uint64_t hash; uint32_t version; uint8_t kind; uint8_t flags; uint32_t count; };

    BlockHeader ReadBlockHeader(Astra::BinaryReader& r)
    {
        BlockHeader h{};
        r(h.hash)(h.version)(h.kind)(h.flags)(h.count);
        return h;
    }

    // Five entities with Position; Health universal (registry Config); Hierarchy
    // Sparse through a storage override, added out of id order so the dense order
    // differs from the ascending order the block must use.
    struct FormatWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::ComponentID hierId = 0;
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> e;

        FormatWorld()
        {
            creg->RegisterComponents<Position, Health>();
            hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
            Astra::Registry::Config cfg;
            cfg.AddUniversal<Health>();
            reg = std::make_unique<Astra::Registry>(creg, cfg);
            for (int i = 0; i < 5; ++i) e.push_back(reg->CreateEntity<Position>());
            reg->GetComponent<Health>(e[2])->current = 7;
            reg->GetComponent<Health>(e[2])->max = 9;
            for (int i : {4, 1, 3})
            {
                Hierarchy h{};
                h.parent = 100u + static_cast<uint32_t>(i);
                EXPECT_TRUE(reg->AddComponentByID(e[i], hierId, &h, sizeof(h)));
            }
            EXPECT_TRUE(reg->SetEnabledByID(e[3], hierId, false));
        }
    };

    std::vector<Astra::Entity> AscendingById(std::vector<Astra::Entity> v)
    {
        std::sort(v.begin(), v.end(), [](Astra::Entity a, Astra::Entity b) { return a.GetID() < b.GetID(); });
        return v;
    }
}

TEST(ComponentStorageFormat, BlockIsSortedByHashWithAscendingEntitiesValuesAndDisabledBitmap)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    FormatWorld world;

    std::vector<std::byte> bytes;
    {
        Astra::BinaryWriter w(bytes);   // memory mode, CompressionMode::None
        world.reg->GetComponentStorage()->Serialize(w);
        ASSERT_FALSE(w.HasError());
    }

    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    uint32_t stored = 0;
    r(stored);
    ASSERT_EQ(stored, 2u);

    const uint64_t hierHash = Astra::TypeID<Hierarchy>::Hash();
    const uint64_t healthHash = Astra::TypeID<Health>::Hash();
    uint64_t previous = 0;
    for (uint32_t b = 0; b < stored; ++b)
    {
        const BlockHeader h = ReadBlockHeader(r);
        ASSERT_FALSE(r.HasError());
        if (b > 0) EXPECT_GT(h.hash, previous) << "blocks must be sorted by hash";
        previous = h.hash;
        EXPECT_EQ(h.version, 1u);

        if (h.hash == hierHash)
        {
            EXPECT_EQ(h.kind, Astra::StorageFormat::KIND_SPARSE);
            EXPECT_EQ(h.flags, Astra::StorageFormat::FLAG_HAS_VALUES | Astra::StorageFormat::FLAG_HAS_DISABLED);
            ASSERT_EQ(h.count, 3u);
            const auto expected = AscendingById({world.e[4], world.e[1], world.e[3]});
            for (uint32_t i = 0; i < 3; ++i) { Astra::Entity got; r(got); EXPECT_EQ(got, expected[i]); }
            for (uint32_t i = 0; i < 3; ++i)
            {
                Hierarchy v{};
                ASSERT_TRUE(r.ReadVersionedComponent(v).IsOk());
                const uint32_t ordinal = (expected[i] == world.e[1]) ? 1u : (expected[i] == world.e[3]) ? 3u : 4u;
                EXPECT_EQ(v.parent, 100u + ordinal);
            }
            uint32_t disabledCount = 0;
            uint64_t word = 0;
            r(disabledCount)(word);
            EXPECT_EQ(disabledCount, 1u);
            const size_t e3 = static_cast<size_t>(std::find(expected.begin(), expected.end(), world.e[3]) - expected.begin());
            EXPECT_EQ(word, uint64_t{1} << e3);
        }
        else
        {
            ASSERT_EQ(h.hash, healthHash);
            EXPECT_EQ(h.kind, Astra::StorageFormat::KIND_UNIVERSAL);
            EXPECT_EQ(h.flags, Astra::StorageFormat::FLAG_HAS_VALUES);   // Health is not enableable
            ASSERT_EQ(h.count, 5u);
            const auto expected = AscendingById(world.e);
            for (uint32_t i = 0; i < 5; ++i) { Astra::Entity got; r(got); EXPECT_EQ(got, expected[i]); }
            for (uint32_t i = 0; i < 5; ++i)
            {
                Health v{};
                ASSERT_TRUE(r.ReadVersionedComponent(v).IsOk());
                EXPECT_EQ(v.current, expected[i] == world.e[2] ? 7 : 100);
            }
        }
    }
    EXPECT_EQ(r.GetRemaining(), 0u);
}

// s8.1: header, EntityManager, ArchetypeManager, ComponentStorage, RelationshipGraph,
// ResourceStorage, checksum. Parse a None-mode Save up to the block and check it
// sits right after the archetypes.
TEST(ComponentStorageFormat, SaveWritesTheBlockBetweenArchetypesAndRelationsAtVersion6)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    FormatWorld world;
    Astra::Registry::SaveConfig sc;
    sc.compressionMode = Astra::CompressionMode::None;
    auto saved = world.reg->Save(sc);
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte>& bytes = *saved.GetValue();

    uint16_t version = 0;
    std::memcpy(&version, bytes.data() + 5, sizeof(version));   // BinaryHeader: magic[5], then version
    EXPECT_EQ(version, 6u);
    EXPECT_EQ(Astra::BINARY_FORMAT_VERSION, 6u);

    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    ASSERT_TRUE(r.ReadHeader().IsOk());
    auto em = Astra::EntityManager::Deserialize(r);
    ASSERT_TRUE(em.IsOk());
    Astra::ArchetypeManager scratch(world.creg, Astra::ArchetypeChunkPool::Config{}, &(*em.GetValue())->GetRecordTable());
    ASSERT_TRUE(scratch.Deserialize(r));

    uint32_t stored = 0;
    r(stored);
    EXPECT_EQ(stored, 2u);
    uint64_t firstHash = 0;
    r(firstHash);
    EXPECT_EQ(firstHash, std::min(Astra::TypeID<Hierarchy>::Hash(), Astra::TypeID<Health>::Hash()));
}

// Fix round 1: the reader branches on the ARCHIVE's flag, never on its own build's
// descriptor. A values-flagged block for a component this build registers as a tag
// (size 0) cannot be decoded; it must be refused, not left to desync the stream.
TEST(ComponentStorageFormat, ValuesFlaggedBlockForATagTypeIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<EnTag>();
    Astra::Registry reg(creg);

    std::vector<std::byte> bytes;
    {
        Astra::BinaryWriter w(bytes);
        uint32_t stored = 1;
        uint64_t hash = Astra::TypeID<EnTag>::Hash();
        uint32_t version = 1, count = 1;
        uint8_t kind = Astra::StorageFormat::KIND_UNIVERSAL;
        uint8_t flags = Astra::StorageFormat::FLAG_HAS_VALUES;
        Astra::Entity e = reg.CreateEntity<EnTag>();
        w(stored)(hash)(version)(kind)(flags)(count)(e);
        uint64_t valueHash = hash; uint32_t valueVersion = 1, payload = 0xABCDu;   // per-element versioned bytes the stub cannot size
        w(valueHash)(valueVersion)(payload);
        ASSERT_FALSE(w.HasError());
    }
    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg));
}

// The stub must consume exactly what Serialize wrote (Sparse values + disabled
// bitmap, uncompressed), so the sections after the block stay aligned.
TEST(ComponentStorageFormat, StubConsumesSparseBlockWithDisabledBitmapExactly)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    FormatWorld world;
    std::vector<std::byte> bytes;
    {
        Astra::BinaryWriter w(bytes);
        world.reg->GetComponentStorage()->Serialize(w);
        ASSERT_FALSE(w.HasError());
    }
    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    EXPECT_TRUE(world.reg->GetComponentStorage()->Deserialize(r, *world.reg));
    EXPECT_EQ(r.GetRemaining(), 0u);
}
