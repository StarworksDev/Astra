#include <cstddef>
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
    constexpr int kEntities = 128;

    std::string LongName(int i) { return "entity-name-with-a-long-and-very-repetitive-suffix-" + std::to_string(i); }

    void MintName(Astra::Entity e, void* component, void* user)
    {
        static_cast<Name*>(component)->value = "minted-" + std::to_string(e.GetID());
        ++*static_cast<int*>(user);
    }

    // Save side, all Table: Position everywhere, Name on even entities.
    struct NamedWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg;
        std::vector<Astra::Entity> entities;
        NamedWorld()
        {
            creg->RegisterComponents<Position, Name>();
            reg = std::make_unique<Astra::Registry>(creg);
            for (int i = 0; i < kEntities; ++i)
            {
                const Astra::Entity e = reg->CreateEntity<Position>();
                if (i % 2 == 0) reg->AddComponent<Name>(e, LongName(i));
                entities.push_back(e);
            }
        }
    };

    std::vector<std::byte> SaveWith(const Astra::Registry& reg, Astra::CompressionMode mode)
    {
        Astra::Registry::SaveConfig sc;
        sc.compressionMode = mode;
        auto saved = reg.Save(sc);
        EXPECT_TRUE(saved.IsOk());
        return saved.IsOk() ? std::move(*saved.GetValue()) : std::vector<std::byte>{};
    }

    // Writes `reg` exactly as a format-v5 Astra did: header stamped 5, entity
    // manager, archetypes, relationship graph, empty resource block, and NO
    // ComponentStorage block. Valid for registries whose components are all
    // Table (v5 had no other storage) and that hold no resources.
    std::vector<std::byte> SaveAsV5(const Astra::Registry& reg, Astra::CompressionMode mode)
    {
        std::vector<std::byte> buf;
        {
            Astra::BinaryWriter::Config wc;
            wc.compressionMode = mode;
            Astra::BinaryWriter w(buf, wc);
            Astra::BinaryHeader header;
            header.version = 5;
            header.entityCount = static_cast<uint32_t>(reg.Size());
            header.archetypeCount = static_cast<uint32_t>(reg.GetArchetypeManager()->GetArchetypeCount());
            w.WriteHeader(header);
            reg.GetEntityManager().Serialize(w);
            reg.GetArchetypeManager()->Serialize(w);
            reg.GetRelationshipGraph().Serialize(w);
            w(uint32_t{0});   // ResourceStorage::Serialize of an empty store
            w.FinalizeHeader();
            EXPECT_FALSE(w.HasError());
        }
        return buf;
    }

    size_t ArchetypesWithMask(Astra::Registry& reg, const Astra::ComponentMask& mask)
    {
        size_t n = 0;
        for (Astra::Archetype* a : reg.GetArchetypeManager()->GetArchetypes())
            if (a->GetMask() == mask) ++n;
        return n;
    }

    Astra::ComponentMask MaskOf(std::initializer_list<Astra::ComponentID> ids)
    {
        Astra::ComponentMask m;
        for (const auto id : ids) m.Set(id);
        return m;
    }

    const Name* NameByHash(Astra::Registry& reg, Astra::Entity e)
    {
        return static_cast<const Name*>(reg.GetComponentByHash(e, Astra::TypeID<Name>::Hash()));
    }
}

// Review Focus 4 (PINNED): an LZ4 archive in which Name was a Table column loads
// into a registry where Name is Sparse. The values must be read out of the
// column's compressed block into the sparse set. The {Position, Name} archetype
// must collapse onto {Position} without a duplicate, and P1-P3 must hold.
TEST(StorageMigration, Lz4TableColumnIsDivertedIntoTheSparseSet)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    const auto lz4 = SaveWith(*world.reg, Astra::CompressionMode::LZ4);
    const auto none = SaveWith(*world.reg, Astra::CompressionMode::None);
    ASSERT_EQ(static_cast<uint8_t>(lz4[offsetof(Astra::BinaryHeader, compressionMode)]),
              static_cast<uint8_t>(Astra::CompressionMode::LZ4));
    ASSERT_LT(lz4.size(), none.size()) << "precondition: the Name column must really be compressed";

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(lz4, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(out.Size(), static_cast<size_t>(kEntities));
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
    EXPECT_EQ(ArchetypesWithMask(out, MaskOf({Astra::TypeID<Position>::Value()})), 1u)
        << "the diverted {Position, Name} archetype must merge into {Position}";
    for (int i = 0; i < kEntities; ++i)
    {
        const Astra::Entity e = world.entities[i];
        const Astra::EntityRecord* rec = out.GetArchetypeManager()->GetEntityRecord(e);
        ASSERT_NE(rec, nullptr);
        EXPECT_FALSE(rec->archetype->GetMask().Test(nameId)) << "entity " << i;
        ASSERT_EQ(out.HasComponentByHash(e, Astra::TypeID<Name>::Hash()), i % 2 == 0) << "entity " << i;
        if (i % 2 == 0)
        {
            const Name* n = NameByHash(out, e);
            ASSERT_NE(n, nullptr);
            EXPECT_EQ(n->value, LongName(i));
        }
    }

    // The migrated set behaves like any other sparse set afterwards.
    Name extra("added-after-load");
    ASSERT_TRUE(out.AddComponentByID(world.entities[1], nameId, &extra, sizeof(extra)));
    ASSERT_TRUE(out.RemoveComponentByID(world.entities[0], nameId));
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}

TEST(StorageMigration, InlineEnableableTableColumnKeepsItsDisabledStateAsSparse)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Hierarchy>();
    Astra::Registry reg(saveCreg);
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 70; ++i)
    {
        const Astra::Entity e = reg.CreateEntity<Position, Hierarchy>();
        reg.GetComponent<Hierarchy>(e)->parent = static_cast<uint32_t>(i);
        if (i % 4 == 1) ASSERT_TRUE(reg.SetEnabled<Hierarchy>(e, false));
        es.push_back(e);
    }
    const auto bytes = SaveWith(reg, Astra::CompressionMode::None);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID hierId = Astra::TestSupport::InstallWithStorage<Hierarchy>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(bytes, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(out.ValidateStorageInvariants(), "");
    for (int i = 0; i < 70; ++i)
    {
        const auto* h = static_cast<const Hierarchy*>(out.GetComponentByHash(es[i], Astra::TypeID<Hierarchy>::Hash()));
        ASSERT_NE(h, nullptr) << "entity " << i;
        EXPECT_EQ(h->parent, static_cast<uint32_t>(i));
        EXPECT_EQ(out.IsEnabledByID(es[i], hierId), i % 4 != 1) << "entity " << i;
    }
}

TEST(StorageMigration, TableColumnBecomesUniversalAndOnlyEntitiesWithoutItAreInitialised)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        const auto bytes = SaveWith(*world.reg, mode);
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponents<Position, Name>();
        int inits = 0;
        Astra::Registry::Config cfg;
        cfg.AddUniversal<Name>(&MintName, &inits);
        auto loaded = Astra::Registry::Load(bytes, creg, cfg);
        ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
        Astra::Registry& out = **loaded.GetValue();

        EXPECT_EQ(inits, kEntities / 2) << "init runs only where no value was archived (s4.2, s8.3)";
        EXPECT_EQ(ArchetypesWithMask(out, MaskOf({Astra::TypeID<Position>::Value()})), 1u);
        for (int i = 0; i < kEntities; ++i)
        {
            const Astra::Entity e = world.entities[i];
            const Name* n = out.GetComponent<Name>(e);
            ASSERT_NE(n, nullptr) << "entity " << i;
            EXPECT_EQ(n->value, i % 2 == 0 ? LongName(i) : "minted-" + std::to_string(e.GetID()));
        }
        EXPECT_EQ(out.ValidateStorageInvariants(), "");
    }
}

// An archetype whose only component is diverted collapses onto the root.
TEST(StorageMigration, ArchetypeLeftWithNoTableComponentsMergesIntoTheRoot)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    auto saveCreg = std::make_shared<Astra::ComponentRegistry>();
    saveCreg->RegisterComponents<Position, Name>();
    Astra::Registry reg(saveCreg);
    const Astra::Entity bare = reg.CreateEntity();
    const Astra::Entity named = reg.CreateEntity<Name>();
    reg.GetComponent<Name>(named)->value = "only-a-name";
    const auto bytes = SaveWith(reg, Astra::CompressionMode::None);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(bytes, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_EQ(ArchetypesWithMask(out, Astra::ComponentMask{}), 1u);
    EXPECT_TRUE(out.GetArchetypeManager()->GetEntityRecord(named)->archetype->GetMask().None());
    EXPECT_FALSE(out.HasComponentByHash(bare, Astra::TypeID<Name>::Hash()));
    ASSERT_NE(NameByHash(out, named), nullptr);
    EXPECT_EQ(NameByHash(out, named)->value, "only-a-name");
    EXPECT_EQ(out.ValidateStorageInvariants(), "");
}

// s8.3: "v5 and older archives load through the same path". All of a v5
// archive's components are Table columns.
TEST(StorageMigration, FormatV5ArchivesLoadIntoUniversalSparseAndTableRegistries)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    for (const auto mode : {Astra::CompressionMode::None, Astra::CompressionMode::LZ4})
    {
        const auto v5 = SaveAsV5(*world.reg, mode);
        uint16_t version = 0;
        std::memcpy(&version, v5.data() + 5, sizeof(version));
        ASSERT_EQ(version, 5u);

        {   // Name declared Universal
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<Position, Name>();
            int inits = 0;
            Astra::Registry::Config cfg;
            cfg.AddUniversal<Name>(&MintName, &inits);
            auto loaded = Astra::Registry::Load(v5, creg, cfg);
            ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
            Astra::Registry& out = **loaded.GetValue();
            EXPECT_EQ(inits, kEntities / 2);
            for (int i = 0; i < kEntities; i += 2)
                EXPECT_EQ(out.GetComponent<Name>(world.entities[i])->value, LongName(i));
            EXPECT_EQ(out.ValidateStorageInvariants(), "");
        }
        {   // Name Sparse
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<Position>();
            Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
            auto loaded = Astra::Registry::Load(v5, creg);
            ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
            Astra::Registry& out = **loaded.GetValue();
            for (int i = 0; i < kEntities; ++i)
                EXPECT_EQ(out.HasComponentByHash(world.entities[i], Astra::TypeID<Name>::Hash()), i % 2 == 0);
            EXPECT_EQ(out.ValidateStorageInvariants(), "");
        }
        {   // Name still Table
            auto creg = std::make_shared<Astra::ComponentRegistry>();
            creg->RegisterComponents<Position, Name>();
            auto loaded = Astra::Registry::Load(v5, creg);
            ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
            Astra::Registry& out = **loaded.GetValue();
            for (int i = 0; i < kEntities; ++i)
                EXPECT_EQ(out.HasComponent<Name>(world.entities[i]), i % 2 == 0);
        }
    }
}

// Diverted values get the same coarse-version stamp a block-loaded Sparse set does
// (Task 32): IsAdded/IsChanged since a pre-load tick answers like a Table column.
TEST(StorageMigration, DivertedSparseValuesStampTheCoarseVersionAtLoad)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    const auto bytes = SaveWith(*world.reg, Astra::CompressionMode::None);
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    const Astra::ComponentID nameId = Astra::TestSupport::InstallWithStorage<Name>(*creg, Astra::Storage::Sparse);
    auto loaded = Astra::Registry::Load(bytes, creg);
    ASSERT_TRUE(loaded.IsOk()) << "error " << static_cast<int>(*loaded.GetError());
    Astra::Registry& out = **loaded.GetValue();
    EXPECT_NE(out.GetComponentStorage()->CoarseVersion(nameId), Astra::Tick{0});
    EXPECT_EQ(out.GetComponentStorage()->CoarseVersion(nameId), out.CurrentTick());
}

// IM-9: the archive says the column carries values; a type that is a tag in the
// loading build cannot decode them, so the load is refused, not desynced.
TEST(StorageMigration, ArchivedValuesForATypeThatIsATagHereAreRefused)
{
    Astra::TestSupport::ScopedDefaultTypeContext ctx;
    NamedWorld world;
    const auto bytes = SaveWith(*world.reg, Astra::CompressionMode::None);
    const Astra::ComponentID nameId = Astra::TypeID<Name>::Value();

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Position>();
    // A tag stand-in carrying Name's hash: install Name's descriptor, emptied, as Sparse.
    Astra::ComponentDescriptor desc = Astra::ComponentRegistry::MakeDescriptor<Name>(nameId, nullptr);
    desc.storage = Astra::Storage::Sparse;
    desc.size = 0;
    desc.deserializeVersioned = nullptr;
    desc.deserialize = nullptr;
    static int s_image = 0;
    const std::string nm(Astra::TypeID<Name>::Name());
    desc.name = nm.c_str();
    const uint32_t owner = creg->OpenModuleId("TagStandIn");
    ASSERT_NE(creg->InstallOwned(nameId, owner, desc, &s_image), Astra::InstallResult::Refused);
    auto loaded = Astra::Registry::Load(bytes, creg);
    EXPECT_TRUE(loaded.IsErr());
}
