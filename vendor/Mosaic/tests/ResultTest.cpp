#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Result.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace
{
    enum class Error { NotFound, Invalid };

    // Counts live instances, so every construct/assign path is checked for a
    // matching destroy (no leak, no double destroy).
    struct Tracked
    {
        static inline int live = 0;
        int value = 0;
        bool movedFrom = false;
        explicit Tracked(int v) : value(v) { ++live; }
        Tracked(const Tracked& o) : value(o.value) { ++live; }
        Tracked(Tracked&& o) noexcept : value(o.value) { o.movedFrom = true; ++live; }
        Tracked& operator=(const Tracked& o) { value = o.value; return *this; }
        Tracked& operator=(Tracked&& o) noexcept { value = o.value; o.movedFrom = true; return *this; }
        ~Tracked() { --live; }
    };

    using R = Mosaic::Result<int, Error>;
}

TEST_CASE("Mosaic::Result Ok and Err report their state and expose one side", "[mosaic][result]")
{
    auto ok = R::Ok(7);
    CHECK(ok.IsOk());
    CHECK_FALSE(ok.IsErr());
    REQUIRE(ok.GetValue() != nullptr);
    CHECK(*ok.GetValue() == 7);
    CHECK(ok.GetError() == nullptr);
    CHECK(*ok == 7);

    auto err = R::Err(Error::Invalid);
    CHECK(err.IsErr());
    CHECK(err.GetValue() == nullptr);
    REQUIRE(err.GetError() != nullptr);
    CHECK(*err.GetError() == Error::Invalid);
}

TEST_CASE("Mosaic::Result operator-> and rvalue operator* reach the value", "[mosaic][result]")
{
    auto r = Mosaic::Result<std::string, Error>::Ok(std::string("hello"));
    CHECK(r->size() == 5u);
    std::string taken = *std::move(r);
    CHECK(taken == "hello");
}

TEST_CASE("Mosaic::Result copies, moves and assigns across states without leaking", "[mosaic][result]")
{
    using T = Mosaic::Result<Tracked, Tracked>;
    Tracked::live = 0;
    {
        auto a = T::Ok(Tracked(1));
        auto b = T::Err(Tracked(2));
        CHECK(Tracked::live == 2);

        T c = a;                                   // copy a value
        CHECK(c.IsOk());
        CHECK(c->value == 1);
        T d = std::move(b);                        // move an error
        CHECK(d.IsErr());
        CHECK(d.GetError()->value == 2);
        CHECK(b.GetError()->movedFrom);
        CHECK(Tracked::live == 4);

        c = d;                                     // value <- error (copy)
        CHECK(c.IsErr());
        CHECK(c.GetError()->value == 2);
        c = std::move(a);                          // error <- value (move)
        CHECK(c.IsOk());
        CHECK(c->value == 1);
        c = T::Ok(Tracked(9));                     // value <- value
        CHECK(c->value == 9);
        CHECK(Tracked::live == 4);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("Mosaic::Result Map, MapError and AndThen take only their branch", "[mosaic][result]")
{
    auto doubled = R::Ok(4).Map([](int v) { return v * 2; });
    CHECK(*doubled == 8);
    auto stillErr = R::Err(Error::NotFound).Map([](int v) { return v * 2; });
    CHECK(*stillErr.GetError() == Error::NotFound);

    auto named = R::Err(Error::Invalid).MapError([](Error) { return std::string("invalid"); });
    STATIC_CHECK(std::is_same_v<decltype(named), Mosaic::Result<int, std::string>>);
    CHECK(*named.GetError() == "invalid");
    CHECK(*R::Ok(3).MapError([](Error) { return 0; }) == 3);

    auto half = [](int v) { return v % 2 == 0 ? R::Ok(v / 2) : R::Err(Error::Invalid); };
    CHECK(*R::Ok(10).AndThen(half) == 5);
    CHECK(*R::Ok(3).AndThen(half).GetError() == Error::Invalid);
    CHECK(*R::Err(Error::NotFound).AndThen(half).GetError() == Error::NotFound);
}

TEST_CASE("Mosaic::Result ValueOr falls back only on an error", "[mosaic][result]")
{
    const auto ok = R::Ok(5);
    CHECK(ok.ValueOr(-1) == 5);
    CHECK(R::Err(Error::Invalid).ValueOr(-1) == -1);
    CHECK(Mosaic::Result<std::string, Error>::Err(Error::Invalid).ValueOr("fallback") == "fallback");
}

TEST_CASE("Mosaic::Result<void, E> carries success or an error", "[mosaic][result]")
{
    using V = Mosaic::Result<void, Error>;
    auto ok = V::Ok();
    CHECK(ok.IsOk());
    CHECK(ok.GetError() == nullptr);
    auto err = V::Err(Error::NotFound);
    CHECK(err.IsErr());
    CHECK(*err.GetError() == Error::NotFound);

    V copy = err;
    CHECK(*copy.GetError() == Error::NotFound);
    copy = ok;
    CHECK(copy.IsOk());

    CHECK(*err.MapError([](Error) { return 42; }).GetError() == 42);
    CHECK(*ok.AndThen([] { return V::Err(Error::Invalid); }).GetError() == Error::Invalid);
    CHECK(err.AndThen([] { return V::Ok(); }).IsErr());
}

TEST_CASE("Mosaic::Result stays a tagged union: noexcept moves and tight size", "[mosaic][result]")
{
    STATIC_CHECK(std::is_nothrow_move_constructible_v<R>);
    STATIC_CHECK(std::is_nothrow_move_assignable_v<R>);
    STATIC_CHECK(sizeof(Mosaic::Result<int, char>) <= 2 * sizeof(int));
    STATIC_CHECK(sizeof(Mosaic::Result<void, int>) <= 2 * sizeof(int));
}
