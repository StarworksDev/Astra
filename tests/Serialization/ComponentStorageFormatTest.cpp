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

namespace
{
    void MintName(Astra::Entity e, void* component, void* user)
    {
        static_cast<Name*>(component)->value = "minted-" + std::to_string(e.GetID());
        ++*static_cast<int*>(user);
    }

    // Position + Name registered; Hierarchy (enableable) and Player (tag) Sparse.
    struct StorageWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::ComponentID hierId = 0;
        Astra::ComponentID playerId = 0;
        StorageWorld()
        {
            creg->RegisterComponents<Position, Name>();
            hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
            playerId = Astra::TestSupport::InstallWithStorage<Player>(*creg, Astra::Storage::Sparse);
        }
    };

    Astra::Registry::Config UniversalNameConfig()
    {
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>();
        return cfg;
    }

    void WriteBlockHeader(Astra::BinaryWriter& w, uint64_t hash, uint8_t kind, uint8_t flags, uint32_t count)
    {
        w(hash);
        w(uint32_t{1});   // descriptor version
        w(kind);
        w(flags);
        w(count);
    }
}

TEST(ComponentStorageFormat, SparseUniversalAndSparseTagRoundTripInBothCompressionModes)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        StorageWorld save;
        Astra::Registry reg(save.creg, UniversalNameConfig());
        std::vector<Astra::Entity> es;
        for (int i = 0; i < 70; ++i)   // > 64: the disabled bitmap spans two words
        {
            const Astra::Entity e = reg.CreateEntity<Position>();
            reg.GetComponent<Name>(e)->value = "name-" + std::to_string(i);
            if (i % 3 == 0)
            {
                Hierarchy h{};
                h.parent = static_cast<uint32_t>(i);
                ASSERT_TRUE(reg.AddComponentByID(e, save.hierId, &h, sizeof(h)));
            }
            if (i % 6 == 0) ASSERT_TRUE(reg.SetEnabledByID(e, save.hierId, false));
            if (i % 5 == 0)
            {
                Player tag{};
                ASSERT_TRUE(reg.AddComponentByID(e, save.playerId, &tag, sizeof(tag)));
            }
            es.push_back(e);
        }
        Astra::Registry::SaveConfig sc;
        sc.compressionMode = mode;
        auto saved = reg.Save(sc);
        ASSERT_TRUE(saved.IsOk());

        StorageWorld load;
        auto loaded = Astra::Registry::Load(*saved.GetValue(), load.creg, UniversalNameConfig());
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& out = **loaded.GetValue();
        EXPECT_EQ(out.ValidateStorageInvariants(), "");

        const uint64_t hierHash = Astra::TypeID<Hierarchy>::Hash();
        const uint64_t playerHash = Astra::TypeID<Player>::Hash();
        for (int i = 0; i < 70; ++i)
        {
            const Astra::Entity e = es[i];
            ASSERT_TRUE(out.IsValid(e));
            ASSERT_NE(out.GetComponent<Name>(e), nullptr);
            EXPECT_EQ(out.GetComponent<Name>(e)->value, "name-" + std::to_string(i));
            ASSERT_EQ(out.HasComponentByHash(e, hierHash), i % 3 == 0) << "entity " << i;
            if (i % 3 == 0)
            {
                const auto* h = static_cast<const Hierarchy*>(out.GetComponentByHash(e, hierHash));
                ASSERT_NE(h, nullptr);
                EXPECT_EQ(h->parent, static_cast<uint32_t>(i));
                EXPECT_EQ(out.IsEnabledByID(e, load.hierId), i % 6 != 0) << "entity " << i;
            }
            EXPECT_EQ(out.HasComponentByHash(e, playerHash), i % 5 == 0) << "entity " << i;
        }
    }
}

// s8.3 bullet 3: Name is declared Universal by the loading registry, and the archive
// never held it. Every entity gets default construction and then the registry's init.
TEST(ComponentStorageFormat, DeclaredUniversalAbsentFromTheArchiveIsDefaultedThenInitialised)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Name>();
    Astra::Registry reg(saveCreg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 10; ++i) es.push_back(reg.CreateEntity<Position>());
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loadCreg = std::make_shared<Astra::ComponentRegistry>();
    loadCreg->RegisterComponents<Position, Name>();
    int inits = 0;
    Astra::Registry::Config cfg;
    cfg.AddUniversal<Name>(&MintName, &inits);
    auto loaded = Astra::Registry::Load(*saved.GetValue(), loadCreg, cfg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(inits, 10);
    for (const Astra::Entity e : es)
    {
        ASSERT_NE(out.GetComponent<Name>(e), nullptr);
        EXPECT_EQ(out.GetComponent<Name>(e)->value, "minted-" + std::to_string(e.GetID()));
    }
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}

// An archived universal value is never re-initialised: only the entities the archive
// lacked get default + init.
TEST(ComponentStorageFormat, ArchivedUniversalValuesAreNotReinitialised)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position, Name>();
    int saveInits = 0;
    Astra::Registry::Config saveCfg;
    saveCfg.AddUniversal<Name>(&MintName, &saveInits);
    Astra::Registry reg(creg, saveCfg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Name>(e)->value = "kept";
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    int loadInits = 0;
    Astra::Registry::Config loadCfg;
    loadCfg.AddUniversal<Name>(&MintName, &loadInits);
    auto loaded = Astra::Registry::Load(*saved.GetValue(), creg, loadCfg);
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ(loadInits, 0);
    EXPECT_EQ((**loaded.GetValue()).GetComponent<Name>(e)->value, "kept");
}

TEST(ComponentStorageFormat, BlockForAnUnregisteredComponentIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld save;
    Astra::Registry reg(save.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Hierarchy h{};
    ASSERT_TRUE(reg.AddComponentByID(e, save.hierId, &h, sizeof(h)));
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loadCreg = std::make_shared<Astra::ComponentRegistry>();
    loadCreg->RegisterComponents<Position, Name>();   // no Hierarchy
    auto loaded = Astra::Registry::Load(*saved.GetValue(), loadCreg);
    ASSERT_TRUE(loaded.IsErr());
    EXPECT_EQ(*loaded.GetError(), Astra::SerializationError::UnknownComponent);   // unknown hash stays UnknownComponent
}

TEST(ComponentStorageFormat, BlockNamingADeadEntityIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity live = reg.CreateEntity<Position>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        w(uint32_t{1});
        WriteBlockHeader(w, Astra::TypeID<Player>::Hash(), Astra::StorageFormat::KIND_SPARSE, 0, 1);
        w(Astra::Entity(live.GetID() + 1000, 1));   // never created in `reg`
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    Astra::SerializationError error = Astra::SerializationError::None;
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg, &error));
    EXPECT_EQ(error, Astra::SerializationError::CorruptedData);
}

// The refusal tests below call Deserialize on a LIVE registry: a refused block may leave
// partially inserted sparse elements behind (without presence bits). Harmless, because
// Load discards the whole registry on any refusal.
TEST(ComponentStorageFormat, BlocksOutOfHashOrderAreRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    const uint64_t hierHash = Astra::TypeID<Hierarchy>::Hash();
    const uint64_t playerHash = Astra::TypeID<Player>::Hash();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        auto writeOne = [&](uint64_t hash)
        {
            if (hash == hierHash)
            {
                WriteBlockHeader(w, hash, Astra::StorageFormat::KIND_SPARSE,
                                 Astra::StorageFormat::FLAG_HAS_VALUES | Astra::StorageFormat::FLAG_HAS_DISABLED, 1);
                w(e);
                Hierarchy h{};
                w.WriteVersionedComponent(h);
                Astra::StorageFormat::WriteDisabledBitmap(w, {0});
            }
            else
            {
                WriteBlockHeader(w, hash, Astra::StorageFormat::KIND_SPARSE, 0, 1);
                w(e);
            }
        };
        w(uint32_t{2});
        writeOne(std::max(hierHash, playerHash));   // descending: invalid
        writeOne(std::min(hierHash, playerHash));
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    Astra::SerializationError error = Astra::SerializationError::None;
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg, &error));
    EXPECT_EQ(error, Astra::SerializationError::CorruptedData);
}

TEST(ComponentStorageFormat, DisabledBitmapPopcountMismatchIsRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        w(uint32_t{1});
        WriteBlockHeader(w, Astra::TypeID<Hierarchy>::Hash(), Astra::StorageFormat::KIND_SPARSE,
                         Astra::StorageFormat::FLAG_HAS_VALUES | Astra::StorageFormat::FLAG_HAS_DISABLED, 1);
        w(e);
        Hierarchy h{};
        w.WriteVersionedComponent(h);
        w(uint32_t{2});     // claims two disabled elements...
        w(uint64_t{1});     // ...but one bit is set
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    Astra::SerializationError error = Astra::SerializationError::None;
    EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg, &error));
    EXPECT_EQ(error, Astra::SerializationError::CorruptedData);
}

// A block with an unknown flag bit, or an empty block, is something the writer never
// emits: CorruptedData, not UnknownComponent.
TEST(ComponentStorageFormat, UnknownFlagsAndEmptyBlocksAreCorrupted)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    for (const auto& [flags, count] : {std::pair<uint8_t, uint32_t>{0x80, 1}, std::pair<uint8_t, uint32_t>{0, 0}})
    {
        std::vector<std::byte> buf;
        {
            Astra::BinaryWriter w(buf);
            w(uint32_t{1});
            WriteBlockHeader(w, Astra::TypeID<Player>::Hash(), Astra::StorageFormat::KIND_SPARSE, flags, count);
            if (count) w(e);
        }
        Astra::BinaryReader r{std::span<const std::byte>(buf)};
        Astra::SerializationError error = Astra::SerializationError::None;
        EXPECT_FALSE(reg.GetComponentStorage()->Deserialize(r, reg, &error));
        EXPECT_EQ(error, Astra::SerializationError::CorruptedData);
    }
}

// Table-now stays decode-and-discard until Task 34 stages it (RULINGS Task 31/32): a
// Sparse block for a component this build keeps in the archetype is consumed exactly.
TEST(ComponentStorageFormat, BlockForATableNowComponentIsConsumedAndDiscarded)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter w(buf);
        w(uint32_t{1});
        WriteBlockHeader(w, Astra::TypeID<Position>::Hash(), Astra::StorageFormat::KIND_SPARSE,
                         Astra::StorageFormat::FLAG_HAS_VALUES, 1);
        w(e);
        Position p{};
        w.WriteVersionedComponent(p);
    }
    Astra::BinaryReader r{std::span<const std::byte>(buf)};
    EXPECT_TRUE(reg.GetComponentStorage()->Deserialize(r, reg));
    EXPECT_EQ(r.GetRemaining(), 0u);
    EXPECT_EQ(reg.GetComponentStorage()->FindSparse(Astra::TypeID<Position>::Value()), nullptr);
}

// RULINGS Task 32 (Task 20 review): a loaded Sparse component reads like a loaded
// Table column, stamped at the loaded registry's current tick (never 0 = "never").
TEST(ComponentStorageFormat, LoadedSparseComponentsStampTheCoarseVersion)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld save;
    Astra::Registry reg(save.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Player tag{};
    ASSERT_TRUE(reg.AddComponentByID(e, save.playerId, &tag, sizeof(tag)));
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    StorageWorld load;
    auto loaded = Astra::Registry::Load(*saved.GetValue(), load.creg);
    ASSERT_TRUE(loaded.IsOk());
    Astra::Registry& out = **loaded.GetValue();
    EXPECT_NE(out.GetComponentStorage()->CoarseVersion(load.playerId), Astra::Tick{0});
    EXPECT_EQ(out.GetComponentStorage()->CoarseVersion(load.playerId), out.CurrentTick());
}

// RULINGS Task 32 (Task 31 review minor 2): Save selects stores by the stores that
// exist, so a held Sparse set whose live descriptor no longer says Sparse is still written.
TEST(ComponentStorageFormat, HeldSparseSetIsSavedEvenWhenItsDescriptorKindChanged)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    StorageWorld world;
    Astra::Registry reg(world.creg);
    const Astra::Entity e = reg.CreateEntity<Position>();
    Hierarchy h{};
    h.parent = 41;
    ASSERT_TRUE(reg.AddComponentByID(e, world.hierId, &h, sizeof(h)));

    Astra::TestSupport::InstallWithStorage<Hierarchy>(*world.creg, Astra::Storage::Table);
    ASSERT_EQ(reg.GetComponentStorage()->KindOf(world.hierId), Astra::StorageKind::Table);

    std::vector<std::byte> bytes;
    {
        Astra::BinaryWriter w(bytes);
        reg.GetComponentStorage()->Serialize(w);
    }
    Astra::BinaryReader r{std::span<const std::byte>(bytes)};
    uint32_t stored = 0;
    r(stored);
    EXPECT_EQ(stored, 1u);
}

// Task 32 review (a): an ENABLEABLE Universal with disabled entities survives Save/Load.
// Exercises the Universal arms of SetLoadedDisabled and DeriveStoragePresence.
TEST(ComponentStorageFormat, EnableableUniversalKeepsDisabledEntitiesAcrossSaveLoad)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    constexpr int kCount = 70;
    Astra::Registry::Config cfg;
    cfg.AddUniversal<Hierarchy>();

    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position>();
    Astra::Registry reg(saveCreg, cfg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < kCount; ++i)
    {
        const Astra::Entity e = reg.CreateEntity<Position>();
        reg.GetComponent<Hierarchy>(e)->parent = static_cast<uint32_t>(i);
        if (i % 3 == 0) ASSERT_TRUE(reg.SetEnabled<Hierarchy>(e, false));
        es.push_back(e);
    }
    ASSERT_EQ(reg.ValidateStorageInvariants(), "");
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loadCreg = std::make_shared<Astra::ComponentRegistry>();
    loadCreg->RegisterComponents<Position>();
    auto loaded = Astra::Registry::Load(*saved.GetValue(), loadCreg, cfg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(out.ValidateStorageInvariants(), "");
    const Astra::ComponentID hierId = Astra::TypeID<Hierarchy>::Value();
    for (int i = 0; i < kCount; ++i)
    {
        const auto* h = static_cast<const Hierarchy*>(out.GetComponentByHash(es[i], Astra::TypeID<Hierarchy>::Hash()));
        ASSERT_NE(h, nullptr) << "entity " << i;
        EXPECT_EQ(h->parent, static_cast<uint32_t>(i));
        EXPECT_EQ(out.IsEnabledByID(es[i], hierId), i % 3 != 0) << "entity " << i;
    }
}

// Task 32 review (b): an archived ENABLEABLE Universal loads as Sparse-now (cross-kind).
TEST(ComponentStorageFormat, ArchivedUniversalLoadsAsSparseWithValuesPresenceAndDisabledState)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    constexpr int kCount = 9;
    Astra::Registry::Config saveCfg;
    saveCfg.AddUniversal<Hierarchy>();
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position>();
    Astra::Registry reg(saveCreg, saveCfg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < kCount; ++i)
    {
        const Astra::Entity e = reg.CreateEntity<Position>();
        reg.GetComponent<Hierarchy>(e)->parent = 40u + static_cast<uint32_t>(i);
        if (i == 2 || i == 5) ASSERT_TRUE(reg.SetEnabled<Hierarchy>(e, false));
        es.push_back(e);
    }
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loadCreg = std::make_shared<Astra::ComponentRegistry>();
    loadCreg->RegisterComponents<Position>();
    const Astra::ComponentID hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*loadCreg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(*saved.GetValue(), loadCreg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(out.ValidateStorageInvariants(), "");
    EXPECT_EQ(out.GetComponentStorage()->KindOf(hierId), Astra::StorageKind::Sparse);
    for (int i = 0; i < kCount; ++i)
    {
        ASSERT_TRUE(out.HasComponentByHash(es[i], Astra::TypeID<Hierarchy>::Hash())) << "entity " << i;
        const auto* h = static_cast<const Hierarchy*>(out.GetComponentByHash(es[i], Astra::TypeID<Hierarchy>::Hash()));
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(h->parent, 40u + static_cast<uint32_t>(i));
        EXPECT_EQ(out.IsEnabledByID(es[i], hierId), i != 2 && i != 5) << "entity " << i;
    }
    // The migrated set is an ordinary sparse set afterwards.
    ASSERT_TRUE(out.RemoveComponentByID(es[0], hierId));
    EXPECT_FALSE(out.HasComponentByHash(es[0], Astra::TypeID<Hierarchy>::Hash()));
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}
