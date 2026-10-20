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
