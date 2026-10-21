#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include "StorageQueryComponents.hpp"

namespace
{
    template<typename T>
    bool ListsComponent(const std::vector<const Astra::ComponentDescriptor*>& descs)
    {
        return std::any_of(descs.begin(), descs.end(), [](const Astra::ComponentDescriptor* d) { return d->id == Astra::TypeID<T>::Value(); });
    }

    template<typename T, typename Infos>
    const void* InspectedData(const Infos& infos, bool& found)
    {
        for (const auto& info : infos)
            if (info.descriptor->id == Astra::TypeID<T>::Value()) { found = true; return info.data; }
        found = false;
        return nullptr;
    }
}

TEST(StorageReflection, SparseMembersAreVisibleToEveryReflectionApi)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Astra::Test::Position>();
    reg.AddComponent(e, Astra::Test::PosS{1.0f, 2.0f, 3.0f});
    reg.AddComponent(e, Astra::Test::StorageQ::SpTag{});
    Astra::Entity f = reg.CreateEntity<Astra::Test::Position>();
    const void* sparse = std::as_const(reg).GetComponent<Astra::Test::PosS>(e);
    ASSERT_NE(sparse, nullptr);

    EXPECT_EQ(reg.GetComponentByHash(e, Astra::TypeID<Astra::Test::PosS>::Hash()), sparse);
    EXPECT_EQ(reg.GetComponentByName(e, Astra::TypeID<Astra::Test::PosS>::Name()), sparse);
    EXPECT_EQ(reg.GetComponentByHash(e, Astra::TypeID<Astra::Test::StorageQ::SpTag>::Hash()), Astra::EmptyComponentSentinel());
    EXPECT_TRUE(reg.HasComponentByHash(e, Astra::TypeID<Astra::Test::PosS>::Hash()));
    EXPECT_TRUE(reg.HasComponentByHash(e, Astra::TypeID<Astra::Test::StorageQ::SpTag>::Hash()));
    EXPECT_FALSE(reg.HasComponentByHash(f, Astra::TypeID<Astra::Test::PosS>::Hash()));
    EXPECT_EQ(reg.GetComponentByHash(f, Astra::TypeID<Astra::Test::PosS>::Hash()), nullptr);

    const auto descs = reg.GetEntityComponents(e);
    EXPECT_TRUE(ListsComponent<Astra::Test::Position>(descs));
    EXPECT_TRUE(ListsComponent<Astra::Test::PosS>(descs));
    EXPECT_TRUE(ListsComponent<Astra::Test::StorageQ::SpTag>(descs));
    EXPECT_FALSE(ListsComponent<Astra::Test::PosS>(reg.GetEntityComponents(f)));

    bool found = false;
    const auto infos = reg.InspectEntity(e);
    EXPECT_EQ(InspectedData<Astra::Test::PosS>(infos, found), sparse);
    EXPECT_TRUE(found);
    EXPECT_EQ(InspectedData<Astra::Test::StorageQ::SpTag>(infos, found), nullptr);   // a tag has no data
    EXPECT_TRUE(found);

    ASSERT_TRUE(reg.RemoveComponent<Astra::Test::PosS>(e));
    EXPECT_EQ(reg.GetComponentByHash(e, Astra::TypeID<Astra::Test::PosS>::Hash()), nullptr);
    EXPECT_FALSE(reg.HasComponentByHash(e, Astra::TypeID<Astra::Test::PosS>::Hash()));
}

TEST(StorageReflection, UniversalMembersAreVisibleAndStaleHandlesMiss)
{
    auto reg = Astra::Test::StorageQ::MakeRegistry<Astra::Test::Health>();
    Astra::Entity e = reg->CreateEntity<Astra::Test::Position>();
    const void* h = std::as_const(*reg).GetComponent<Astra::Test::Health>(e);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(reg->GetComponentByHash(e, Astra::TypeID<Astra::Test::Health>::Hash()), h);
    EXPECT_TRUE(reg->HasComponentByHash(e, Astra::TypeID<Astra::Test::Health>::Hash()));
    EXPECT_TRUE(ListsComponent<Astra::Test::Health>(reg->GetEntityComponents(e)));
    bool found = false;
    EXPECT_EQ(InspectedData<Astra::Test::Health>(reg->InspectEntity(e), found), h);
    EXPECT_TRUE(found);

    reg->DestroyEntity(e);
    Astra::Entity recycled = reg->CreateEntity<Astra::Test::Position>();   // same id, version + 1 (T1-C2)
    EXPECT_EQ(reg->GetComponentByHash(e, Astra::TypeID<Astra::Test::Health>::Hash()), nullptr);
    EXPECT_FALSE(reg->HasComponentByHash(e, Astra::TypeID<Astra::Test::Health>::Hash()));
    EXPECT_NE(reg->GetComponentByHash(recycled, Astra::TypeID<Astra::Test::Health>::Hash()), nullptr);
}

TEST(StorageReflection, RelationsFiltersAndYieldsSeeSparseMembers)
{
    Astra::Registry reg;
    Astra::Entity parent = reg.CreateEntity<Astra::Test::Velocity>();
    std::vector<Astra::Entity> kids;
    for (int i = 0; i < 6; ++i)
    {
        Astra::Entity kid = reg.CreateEntity<Astra::Test::Velocity>();
        if (i % 2 == 0) reg.AddComponent(kid, Astra::Test::PosS{float(i), 0.0f, 0.0f});
        if (i == 1) reg.AddComponent(kid, Astra::Test::StorageQ::SpTag{});
        reg.SetParent(kid, parent);
        kids.push_back(kid);
    }
    EXPECT_EQ(reg.GetRelations<Astra::Test::PosS>(parent).GetChildren().size(), 3u);
    EXPECT_EQ(reg.GetRelations<Astra::Not<Astra::Test::PosS>>(parent).GetChildren().size(), 3u);
    EXPECT_EQ((reg.GetRelations<Astra::Any<Astra::Test::PosS, Astra::Test::StorageQ::SpTag>>(parent).GetChildren().size()), 4u);
    EXPECT_EQ((reg.GetRelations<Astra::OneOf<Astra::Test::PosS, Astra::Test::Velocity>>(parent).GetChildren().size()), 3u);

    const Astra::Tick since = reg.CurrentTick();
    reg.AdvanceTick();
    float sum = 0.0f;
    reg.GetRelations<Astra::Test::PosS>(parent).ForEachChild([&](Astra::Entity, Astra::Test::PosS& s) { sum += s.x; s.y = 9.0f; });
    EXPECT_FLOAT_EQ(sum, 0.0f + 2.0f + 4.0f);
    ASSERT_NE(std::as_const(reg).GetComponent<Astra::Test::PosS>(kids[2]), nullptr);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Astra::Test::PosS>(kids[2])->y, 9.0f);
    EXPECT_TRUE(reg.IsChanged<Astra::Test::PosS>(kids[2], since));          // a non-const yield is a write
}

TEST(StorageReflection, RelationsSeeUniversalMembers)
{
    auto reg = Astra::Test::StorageQ::MakeRegistry<Astra::Test::Health>();
    Astra::Entity parent = reg->CreateEntity<Astra::Test::Velocity>();
    for (int i = 0; i < 4; ++i)
    {
        Astra::Entity kid = reg->CreateEntity<Astra::Test::Velocity>();
        reg->SetParent(kid, parent);
    }
    EXPECT_EQ(reg->GetRelations<Astra::Test::Health>(parent).GetChildren().size(), 4u);
    EXPECT_EQ(reg->GetRelations<Astra::Not<Astra::Test::Health>>(parent).GetChildren().size(), 0u);
    int visits = 0;
    reg->GetRelations<Astra::Test::Health>(parent).ForEachChild([&](Astra::Entity, Astra::Test::Health& h) { h.current = 1; ++visits; });
    EXPECT_EQ(visits, 4);
}
