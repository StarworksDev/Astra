#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>

#include "Base.hpp"   // ASTRA_NODISCARD

namespace Astra
{
    /**
     * Change-detection time (spec 2026-09-10 §3.1). A Tick is a process-relative,
     * monotonically increasing counter owned by the Registry's ArchetypeManager
     * and advanced once per system group by the scheduler (or by hand via
     * Registry::AdvanceTick for unscheduled use). Tick 0 means "never": a fresh
     * chunk or entity has never been stamped. Nothing here is serialized.
     */
    using Tick = uint32_t;

    /**
     * Two ticks compared by SIGNED difference so the counter may wrap: `a` is
     * newer than `b` iff a - b, read as int32_t, is positive. Ages beyond 2^31
     * system runs misread (documented bound; a periodic clamp is a follow-up).
     * Stamped(>=1) vs never(0) is always newer; never vs never is not.
     */
    ASTRA_NODISCARD constexpr bool IsNewer(Tick a, Tick b) noexcept
    {
        return static_cast<int32_t>(a - b) > 0;
    }

    /**
     * Shared-location tick accesses for the PARALLEL view drivers (RULINGS Task 28).
     * A location several workers can reach in one walk (the untracked-Sparse coarse
     * version slot, a chunk's column version written from a dense non-chunk-partitioned
     * range, a Universal page version) is written with StoreTickRelaxed and read with
     * LoadTickRelaxed: all workers store the same `now`, so relaxed ordering is enough
     * and the formal data race is gone. Per-element / per-slot ticks of disjoint
     * entities stay plain stores. Serial paths keep plain accesses (the `Par=false`
     * arms of the stores' templated accessors never call these).
     */
    static_assert(std::atomic_ref<Tick>::is_always_lock_free, "Tick must be lock-free atomically accessible");

    inline void StoreTickRelaxed(Tick& slot, Tick value) noexcept
    {
        std::atomic_ref<Tick>(slot).store(value, std::memory_order_relaxed);
    }

    ASTRA_NODISCARD inline Tick LoadTickRelaxed(const Tick& slot) noexcept
    {
        return std::atomic_ref<Tick>(const_cast<Tick&>(slot)).load(std::memory_order_relaxed);
    }

    /**
     * Check-before-store for a shared stamp slot a parallel walk hits once per ENTITY: after
     * the first worker stores `now`, the line stays shared and read-only instead of bouncing
     * in exclusive state between workers on every entity.
     */
    inline void StampTickShared(Tick& slot, Tick now) noexcept
    {
        if (LoadTickRelaxed(slot) != now)
            StoreTickRelaxed(slot, now);
    }

    /**
     * Per-entity ticks for a change-tracked column (spec §3.2, opt-in tier):
     * `added` = the tick the component was added to this entity, `changed` = the
     * tick of the last mark. 8 bytes per entity per tracked column, carved into
     * the chunk arena beside the enableable disabled words.
     */
    struct EntityTicks
    {
        Tick added   = 0;
        Tick changed = 0;
    };

    /**
     * Anything a tick-aware view can take its "since" from: SystemContext, or a
     * test stand-in. A concept (not the concrete SystemContext) so View.hpp does
     * not have to include System/SystemContext.hpp (which includes Registry.hpp,
     * which includes View.hpp).
     */
    template<typename C>
    concept TickContext = requires(const C& c)
    {
        { c.LastRun() } -> std::convertible_to<Tick>;
    };
}
