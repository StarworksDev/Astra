#pragma once

// Storage-policies query suite (Tasks 22-30): test components + helpers. ONE new
// TypeID only (SpTag; the binary shares one ASTRA_MAX_COMPONENTS id space -- see the
// budget note in tests/TestComponents.hpp). SpB and SpE are chunk C's types (Task 16's
// SparseStructureTestComponents.hpp), brought in by using-declarations, so they mint no
// id. The Sparse data type is Astra::Test::PosS (Task 3);
// Universal variants reuse Table types (Health, Timer, TrackedPos, TrackedVel)
// declared universal per registry with Config::AddUniversal<T>().

#include <Astra/Astra.hpp>
#include <memory>
#include "../TestComponents.hpp"
#include "SparseStructureTestComponents.hpp"   // Task 16: SpB, SpE (shared, no new TypeIDs)

namespace Astra::Test::StorageQ
{
    // Second Sparse data type (Any/OneOf groups, the smallest-set driver): chunk C's.
    using Astra::Test::SparseC::SpB;

    // Sparse tag: presence only. Deliberately NOT enableable, unlike SparseC::SpTag:
    // a required enableable argument makes View::begin() refuse (HasRequiredFilter),
    // and Task 29's range-for tests need a Sparse tag they can iterate.
    struct SpTag
    {
        static constexpr auto AstraStorage = Astra::Storage::Sparse;
    };

    // Sparse + enableable + change-tracked: disabled mirror, Mut<T>, Changed/Added: chunk C's.
    using Astra::Test::SparseC::SpE;

    static_assert(Component<SpB> && Component<SpTag> && Component<SpE>);

    // A registry declaring every Us... universal, optionally with a work scheduler.
    template<typename... Us>
    std::unique_ptr<Astra::Registry> MakeRegistry(std::shared_ptr<Astra::IWorkScheduler> scheduler = nullptr)
    {
        Astra::Registry::Config cfg;
        (cfg.AddUniversal<Us>(), ...);
        cfg.workScheduler = std::move(scheduler);
        return std::make_unique<Astra::Registry>(cfg);
    }
}
