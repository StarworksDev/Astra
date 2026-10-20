#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

// Storage-policies declaration surface (spec 2026-10-01 s3.2, s4): the Storage trait,
// the descriptor snapshot, the universal Config, and ComponentStorage's kind and
// sparse-slot tables. Registered types: Astra::Test::PosS (the one Sparse test type)
// plus existing Table types. The StorageTraitTestDetail types are compile-time only
// and mint NO ComponentID (MakeDescriptor takes the id as an argument).

namespace StorageTraitTestDetail
{
    struct MemberSparse { static constexpr auto AstraStorage = Astra::Storage::Sparse; int v; };
    struct MemberTable  { static constexpr auto AstraStorage = Astra::Storage::Table; int v; };
    struct Plain        { int v; };
    struct SpecSparse   { int v; };
    struct SparseTag    { static constexpr auto AstraStorage = Astra::Storage::Sparse; };
}
template<> struct Astra::StorageTraits<StorageTraitTestDetail::SpecSparse>
{
    static constexpr Astra::Storage value = Astra::Storage::Sparse;
};

namespace
{
    // ComponentModule::Open requires an EXPLICITLY installed TypeContext slot; the
    // module-default context keeps every id identical to TypeID<T>::Value().
    struct InstalledContext
    {
        InstalledContext()  { Astra::SetTypeContext(&Astra::DefaultTypeContext()); }
        ~InstalledContext() { Astra::SetTypeContext(nullptr); }
    };
}

TEST(StorageTrait, DetectionCoversBothSpellingsDefaultsAndConst)
{
    using namespace Astra;
    using namespace StorageTraitTestDetail;
    static_assert(StorageOfV<MemberSparse> == Storage::Sparse);
    static_assert(StorageOfV<MemberTable> == Storage::Table);
    static_assert(StorageOfV<SpecSparse> == Storage::Sparse);          // specialization escape hatch
    static_assert(StorageOfV<Plain> == Storage::Table);                // default
    static_assert(StorageOfV<int> == Storage::Table);                  // non-class T: no hard error
    static_assert(StorageOfV<const MemberSparse> == Storage::Sparse);  // const-stripped like IsEnableableV
    static_assert(StorageOfV<SparseTag> == Storage::Sparse && std::is_empty_v<SparseTag>);   // sparse tags are legal
    static_assert(StorageOfV<Astra::Test::PosS> == Storage::Sparse);
    static_assert(StorageOfV<Astra::Test::Position> == Storage::Table);
    static_assert(StorageKind::Table != StorageKind::Sparse && StorageKind::Sparse != StorageKind::Universal);
    SUCCEED();
}

TEST(StorageTrait, ConstantsMatchTheSpec)
{
    static_assert(Astra::MAX_SPARSE_COMPONENTS == 32);
    static_assert(Astra::SPARSE_PAGE_SIZE == 4096);
    static_assert(Astra::UNIVERSAL_PAGE_SLOTS == 1024);
    SUCCEED();
}

TEST(StorageTrait, DescriptorSnapshotsTheTraitOnTheAnonymousPath)
{
    // MakeDescriptor is the one builder BOTH registration paths share; a hand-built
    // descriptor defaults to Table (the safe default, like isEnableable).
    EXPECT_EQ(Astra::ComponentRegistry::MakeDescriptor<StorageTraitTestDetail::MemberSparse>(0, nullptr).storage,
              Astra::Storage::Sparse);
    EXPECT_EQ(Astra::ComponentDescriptor{}.storage, Astra::Storage::Table);

    Astra::ComponentRegistry registry;
    registry.RegisterComponents<Astra::Test::PosS, Astra::Test::Position>();
    const auto* sparse = registry.GetComponentDescriptor(Astra::TypeID<Astra::Test::PosS>::Value());
    const auto* table  = registry.GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value());
    ASSERT_NE(sparse, nullptr);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(sparse->storage, Astra::Storage::Sparse);
    EXPECT_EQ(table->storage, Astra::Storage::Table);
}

TEST(StorageTrait, DescriptorSnapshotsTheTraitOnTheModulePath)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "StorageTraitModule");
    ASSERT_TRUE(static_cast<bool>(mod));
    mod.Register<Astra::Test::PosS>();
    const auto* desc = creg->GetComponentDescriptor(Astra::TypeID<Astra::Test::PosS>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->storage, Astra::Storage::Sparse);
}

// ---------------- ComponentStorage (spec s4): kinds + sparse slots ----------------

namespace
{
    // Records and CONTINUES, so a deliberately failing ASTRA_ASSERT / ASTRA_ENSURE returns
    // into the refusal path instead of breaking or aborting the test process.
    Astra::AssertAction CountingContinue(const Astra::AssertContext&, void* user) noexcept
    {
        ++*static_cast<int*>(user);
        return Astra::AssertAction::Continue;
    }
}

TEST(ComponentStorage, KindOfReadsTheTraitAndTheUniversalList)
{
    using Astra::StorageKind;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponents<Astra::Test::Position, Astra::Test::PosS>();
    const Astra::ComponentID pos  = Astra::TypeID<Astra::Test::Position>::Value();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    const Astra::ComponentID vel  = Astra::TypeID<Astra::Test::Velocity>::Value();   // NOT registered in creg

    Astra::ComponentStorage plain(creg, {});
    EXPECT_EQ(plain.KindOf(pos), StorageKind::Table);
    EXPECT_EQ(plain.KindOf(posS), StorageKind::Sparse);
    EXPECT_EQ(plain.KindOf(vel), StorageKind::Table);                     // unregistered: Table
    EXPECT_EQ(plain.KindOf(Astra::INVALID_COMPONENT), StorageKind::Table);
    EXPECT_TRUE(plain.UniversalSpecs().empty());

    const std::vector<Astra::UniversalSpec> specs{ Astra::UniversalSpec{pos, nullptr, nullptr},
                                                   Astra::UniversalSpec{posS, nullptr, nullptr} };
    Astra::ComponentStorage universal(creg, specs);
    EXPECT_EQ(universal.KindOf(pos), StorageKind::Universal);
    EXPECT_EQ(universal.KindOf(posS), StorageKind::Universal);            // Universal overrides the Sparse trait
    ASSERT_NE(universal.FindUniversalSpec(pos), nullptr);
    EXPECT_EQ(universal.FindUniversalSpec(vel), nullptr);
}

TEST(ComponentStorage, UniversalSpecRegistersItsTypeAndLastDeclarationWins)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const Astra::ComponentID name = Astra::TypeID<Astra::Test::Name>::Value();
    ASSERT_EQ(creg->GetComponentDescriptor(name), nullptr);
    int first = 0, second = 0;
    const auto registerName = +[](Astra::ComponentRegistry& r) { r.RegisterComponent<Astra::Test::Name>(); };
    const std::vector<Astra::UniversalSpec> specs{ Astra::UniversalSpec{name, nullptr, &first, registerName},
                                                   Astra::UniversalSpec{name, nullptr, &second, registerName} };
    Astra::ComponentStorage storage(creg, specs);
    EXPECT_NE(creg->GetComponentDescriptor(name), nullptr);      // registered through the spec's thunk
    EXPECT_EQ(storage.KindOf(name), Astra::StorageKind::Universal);
    ASSERT_EQ(storage.UniversalSpecs().size(), 1u);              // deduplicated by id
    EXPECT_EQ(storage.FindUniversalSpec(name)->user, static_cast<void*>(&second));   // last declaration wins
}

TEST(ComponentStorage, InvalidUniversalIdIsIgnored)
{
    // The handler keeps the ASTRA_ENSURE from breaking into a debugger. Its counter is
    // deliberately NOT asserted: ASTRA_ENSURE fires once per call site per process, so a
    // count would depend on whether another test already tripped this site.
    int calls = 0;
    Astra::Testing::ScopedAssertHandler guard(&CountingContinue, &calls);
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentStorage storage(creg, { Astra::UniversalSpec{Astra::INVALID_COMPONENT, nullptr, nullptr} });
    EXPECT_TRUE(storage.UniversalSpecs().empty());
    EXPECT_EQ(storage.KindOf(Astra::INVALID_COMPONENT), Astra::StorageKind::Table);
}

TEST(ComponentStorage, SparseSlotsAreDenseIdempotentAndPerStorage)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const Astra::ComponentID posS = Astra::TypeID<Astra::Test::PosS>::Value();
    const Astra::ComponentID pos  = Astra::TypeID<Astra::Test::Position>::Value();
    Astra::ComponentStorage a(creg, {});
    Astra::ComponentStorage b(creg, {});

    EXPECT_EQ(a.SparseSlotOf(posS), -1);
    EXPECT_EQ(a.AcquireSparseSlot(posS), 0);
    EXPECT_EQ(a.AcquireSparseSlot(posS), 0);                      // idempotent
    EXPECT_EQ(a.AcquireSparseSlot(pos), 1);                       // kind not checked: enableable Universal ids take slots too (s5.1)
    EXPECT_EQ(a.SparseSlotOf(posS), 0);
    EXPECT_EQ(a.ComponentOfSlot(0), posS);
    EXPECT_EQ(a.ComponentOfSlot(1), pos);
    EXPECT_EQ(a.ComponentOfSlot(2), Astra::INVALID_COMPONENT);
    EXPECT_EQ(a.ComponentOfSlot(static_cast<uint16_t>(Astra::MAX_SPARSE_COMPONENTS)), Astra::INVALID_COMPONENT);
    EXPECT_EQ(a.UsedSparseSlotCount(), 2u);

    // A shared ComponentRegistry does not couple slot spaces (spec s3.2).
    EXPECT_EQ(b.AcquireSparseSlot(pos), 0);
    EXPECT_EQ(b.AcquireSparseSlot(posS), 1);
    EXPECT_EQ(a.SparseSlotOf(posS), 0);

    a.Clear();                                                    // slot assignments survive Clear (s7.3)
    EXPECT_EQ(a.SparseSlotOf(posS), 0);
    EXPECT_EQ(a.SparseSlotOf(pos), 1);
}

TEST(ComponentStorage, SparseSlotCapRefusesBeyondMaxSparseComponents)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentStorage storage(creg, {});
    // Raw ids: the slot table never consults a descriptor, so no TypeID is minted.
    for (size_t i = 0; i < Astra::MAX_SPARSE_COMPONENTS; ++i)
        ASSERT_EQ(storage.AcquireSparseSlot(static_cast<Astra::ComponentID>(i)), static_cast<int>(i));
    ASSERT_EQ(storage.UsedSparseSlotCount(), Astra::MAX_SPARSE_COMPONENTS);

    const auto overflow = static_cast<Astra::ComponentID>(Astra::MAX_SPARSE_COMPONENTS);   // 32 < MAX_COMPONENTS
    int calls = 0;
    {
        Astra::Testing::ScopedAssertHandler guard(&CountingContinue, &calls);
        EXPECT_EQ(storage.AcquireSparseSlot(overflow), -1);
    }
#if MOSAIC_ASSERTS_ACTIVE
    EXPECT_EQ(calls, 1);                                          // Debug: assert, then the refusal
#endif
    EXPECT_EQ(storage.SparseSlotOf(overflow), -1);
    EXPECT_EQ(storage.UsedSparseSlotCount(), Astra::MAX_SPARSE_COMPONENTS);
    EXPECT_EQ(storage.AcquireSparseSlot(5), 5);                   // existing assignments still answer
}
