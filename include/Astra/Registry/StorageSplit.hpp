#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../Component/Component.hpp"

namespace Astra::Detail
{
    // Compile-time partition of a component pack by its declared storage TRAIT (spec
    // 2026-10-01 s7.1). This is pure type-level recursion: no constexpr arrays, so an
    // empty partition needs no zero-length array. Universal is a per-registry runtime
    // fact, and the Registry resolves it per member.

    template<Storage S, std::size_t I, typename Out, typename... Cs>
    struct StorageIndexSeqImpl
    {
        using type = Out;
    };

    template<Storage S, std::size_t I, std::size_t... Out, typename C, typename... Rest>
    struct StorageIndexSeqImpl<S, I, std::index_sequence<Out...>, C, Rest...>
    {
        using type = typename StorageIndexSeqImpl<S, I + 1,
            std::conditional_t<StorageOfV<std::decay_t<C>> == S,
                               std::index_sequence<Out..., I>,
                               std::index_sequence<Out...>>,
            Rest...>::type;
    };

    // Positions (into Cs...) of the members whose decayed trait is S, ascending.
    template<Storage S, typename... Cs>
    using StorageIndexSeq = typename StorageIndexSeqImpl<S, 0, std::index_sequence<>, Cs...>::type;

    template<typename Tuple, typename Seq>
    struct SelectDecayed;

    template<typename Tuple, std::size_t... I>
    struct SelectDecayed<Tuple, std::index_sequence<I...>>
    {
        using type = std::tuple<std::decay_t<std::tuple_element_t<I, Tuple>>...>;
    };

    // The members whose decayed trait is S, decayed, in pack order, as a std::tuple type
    // (consumed through a std::type_identity<std::tuple<Ts...>> template lambda).
    template<Storage S, typename... Cs>
    using TypesWithStorage = typename SelectDecayed<std::tuple<Cs...>, StorageIndexSeq<S, Cs...>>::type;
}
