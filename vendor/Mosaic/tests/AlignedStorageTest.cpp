#include <catch2/catch_test_macros.hpp>

#include <Mosaic/AlignedStorage.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

// Ported from Astra's tests/Container/AlignedStorageTest.cpp when the type moved
// here. Its BytePreservation case (write a uint32_t, read a float) is not carried:
// that reads one object through another type, which C++ leaves undefined.

TEST_CASE("Mosaic::AlignedStorage stores either type", "[mosaic][alignedstorage]")
{
    Mosaic::AlignedStorage<int, double> storage;
    *storage.As<int>() = 42;
    CHECK(*storage.As<int>() == 42);
    *storage.As<double>() = 3.5;
    CHECK(*storage.As<double>() == 3.5);

    const auto& constStorage = storage;
    CHECK(*constStorage.As<double>() == 3.5);
}

TEST_CASE("Mosaic::AlignedStorage takes the larger size and the stricter alignment", "[mosaic][alignedstorage]")
{
    struct Aligned16 { alignas(16) char data[16]; };
    struct Aligned8 { alignas(8) char data[8]; };
    using Storage = Mosaic::AlignedStorage<Aligned16, Aligned8>;
    STATIC_CHECK(sizeof(Storage::data) == std::max(sizeof(Aligned16), sizeof(Aligned8)));
    STATIC_CHECK(Storage::alignment == 16u);

    Storage storage;
    CHECK(reinterpret_cast<std::uintptr_t>(&storage.data) % 16 == 0u);

    STATIC_CHECK(Mosaic::AlignedStorage<char, int>::size == sizeof(int));
    STATIC_CHECK(Mosaic::AlignedStorage<char, int>::alignment == alignof(int));
    STATIC_CHECK(Mosaic::AlignedStorage<double, char>::size == sizeof(double));
    struct Large { char data[100]; };
    struct Small { char data[10]; };
    STATIC_CHECK(Mosaic::AlignedStorage<Large, Small>::size == sizeof(Large));
}

TEST_CASE("Mosaic::AlignedStorage holds structs and arrays", "[mosaic][alignedstorage]")
{
    struct SmallStruct { int a; char b; };
    struct LargeStruct { double values[4]; int count; };
    Mosaic::AlignedStorage<SmallStruct, LargeStruct> storage;
    storage.As<SmallStruct>()->a = 123;
    storage.As<SmallStruct>()->b = 'X';
    CHECK(storage.As<SmallStruct>()->a == 123);
    CHECK(storage.As<SmallStruct>()->b == 'X');

    auto* large = storage.As<LargeStruct>();
    large->values[0] = 1.0;
    large->values[3] = 4.0;
    large->count = 4;
    CHECK(storage.As<LargeStruct>()->values[0] == 1.0);
    CHECK(storage.As<LargeStruct>()->values[3] == 4.0);
    CHECK(storage.As<LargeStruct>()->count == 4);

    Mosaic::AlignedStorage<int[4], double[2]> arrays;
    (*arrays.As<int[4]>())[3] = 40;
    CHECK((*arrays.As<int[4]>())[3] == 40);
    (*arrays.As<double[2]>())[1] = 2.5;
    CHECK((*arrays.As<double[2]>())[1] == 2.5);
}

TEST_CASE("Mosaic::AlignedStorage<void, E> holds only E, at E's size and alignment", "[mosaic][alignedstorage]")
{
    struct ErrorType { int code; char message[64]; };
    Mosaic::AlignedStorage<void, ErrorType> storage;
    storage.As<ErrorType>()->code = 404;
    std::memcpy(storage.As<ErrorType>()->message, "Not Found", sizeof("Not Found"));
    CHECK(storage.As<ErrorType>()->code == 404);
    CHECK(std::strcmp(storage.As<ErrorType>()->message, "Not Found") == 0);
    STATIC_CHECK(sizeof(storage.data) == sizeof(ErrorType));
    STATIC_CHECK(alignof(decltype(storage)) == alignof(ErrorType));
}
