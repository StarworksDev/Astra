#pragma once
// Mosaic::AlignedStorage<T, E>: raw storage sized and aligned for either T or
// E (only E for <void, E>), the backing of Mosaic::Result. Moved from Astra,
// which now forwards to it.

#include <algorithm>
#include <cstddef>
#include <new>
#include <type_traits>

namespace Mosaic
{
    // Type-erased aligned storage for union-like behavior without UB
    template<typename T, typename E>
    struct AlignedStorage
    {
        static constexpr std::size_t size = std::max(sizeof(T), sizeof(E));
        static constexpr std::size_t alignment = std::max(alignof(T), alignof(E));

        alignas(alignment) std::byte data[size];

        template<typename U>
        U* As() noexcept
        {
            static_assert(std::is_same_v<U, T> || std::is_same_v<U, E>, "AlignedStorage::As<U>() - U must be either T or E");
            return std::launder(reinterpret_cast<U*>(&data));
        }

        template<typename U>
        const U* As() const noexcept
        {
            static_assert(std::is_same_v<U, T> || std::is_same_v<U, E>, "AlignedStorage::As<U>() - U must be either T or E");
            return std::launder(reinterpret_cast<const U*>(&data));
        }
    };

    // Specialization for single type (used in Result<void, E>)
    template<typename E>
    struct AlignedStorage<void, E>
    {
        alignas(E) std::byte data[sizeof(E)];

        template<typename U>
        U* As() noexcept
        {
            static_assert(std::is_same_v<U, E>, "AlignedStorage<void, E>::As<U>() - U must be E");
            return std::launder(reinterpret_cast<U*>(&data));
        }

        template<typename U>
        const U* As() const noexcept
        {
            static_assert(std::is_same_v<U, E>, "AlignedStorage<void, E>::As<U>() - U must be E");
            return std::launder(reinterpret_cast<const U*>(&data));
        }
    };
}
