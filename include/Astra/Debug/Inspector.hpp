#pragma once

// Read-only Registry introspection facade (AstraStudio MVP, spec 2026-07-27).
// Opt-in: NOT included by Astra.hpp. Snapshots internals into POD structs so
// tools never hold pointers into live engine state. This header is the single
// sanctioned place that walks ArchetypeManager/Archetype/ColumnMeta for
// inspection; keep tool code out of engine internals.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../Registry/Registry.hpp"

namespace Astra::Debug
{
    struct ColumnInfo
    {
        std::string name;
        ComponentID id{};
        size_t size = 0;
        size_t alignment = 0;
        uint32_t stride = 0;
        bool isEnableable = false;
    };

    // Physical placement of one storage column inside one chunk's arena.
    struct ChunkColumnLayout
    {
        size_t offset = 0;                    // base offset (cache-line-aligned; a bit-only column's is the unaligned running offset)
        size_t bytes = 0;                     // stride * capacity
        size_t disabledOffset = SIZE_MAX;     // disabled-bit words region, or SIZE_MAX
        size_t disabledBytes = 0;
        uint32_t disabledCount = 0;
    };

    // One sparse presence block of one chunk (spec 2026-10-01 s5.1). Side allocation
    // from the chunk pool's word freelist -- NOT part of the chunk arena (chunkBytes).
    struct SparseBlockInfo
    {
        uint16_t slot = 0;
        ComponentID componentId{};
        uint32_t presentCount = 0;
        uint32_t disabledCount = 0;
        bool hasPresentWords = false;    // false for an enableable Universal ("all present")
        bool hasDisabledWords = false;
    };

    struct ChunkInfo
    {
        size_t count = 0;
        size_t capacity = 0;
        size_t chunkBytes = 0;                       // arena size
        std::vector<ChunkColumnLayout> columns;      // parallel to ArchetypeInfo::columns
        // Accounting; invariant: the four sum exactly to chunkBytes.
        size_t columnBytes = 0, padBytes = 0, bitsBytes = 0, slackBytes = 0;
        std::vector<SparseBlockInfo> sparseBlocks;   // ascending slot; empty = no sparse member here
        size_t sparseBlockBytes = 0;                 // side allocations; NOT in the chunkBytes sum
    };

    struct ArchetypeInfo
    {
        std::string signature;              // all mask components (tags included), " + "-joined; "(empty)" for the root
        size_t componentCount = 0;          // mask popcount
        size_t tagCount = 0;                // empty (tag) components in the mask
        std::vector<ColumnInfo> columns;    // storage-bearing columns + an enableable tag's bit-only column (stride 0)
        std::vector<ChunkInfo> chunks;
        size_t entityCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;                // layout-derived (unchanged semantics)
        size_t bytesAllocated = 0;           // layout-derived (unchanged semantics)
        size_t bytesReserved = 0;            // true arena footprint: sum of chunkBytes
    };

    // One component's storage, as the Studio storage panel shows it (s9.4). Table entries
    // are listed when the component is held or churned; Sparse/Universal ones always.
    struct StoreInfo
    {
        std::string name;
        ComponentID id{};
        StorageKind kind = StorageKind::Table;   // effective kind in THIS registry
        size_t size = 0;
        bool isEnableable = false;
        bool isChangeTracked = false;
        int sparseSlot = -1;                     // Sparse, or enableable Universal (mirror); else -1
        size_t population = 0;                   // live holders
        size_t pageCount = 0;                    // Sparse: index pages; Universal: column pages; Table: 0
        size_t archetypeCount = 0;               // Table: archetypes whose mask holds it
        size_t pairedArchetypeCount = 0;         // Table: archetypes whose mask-minus-it also exists
        size_t presenceBlockCount = 0;           // chunks with a SparseBlock for sparseSlot
        size_t presenceBitsSet = 0;              // sum of presentCount (== population for Sparse: P1)
        size_t disabledCount = 0;                // Sparse/Universal: sum of the disabled mirror (P3)
        uint64_t adds = 0;                       // ChurnTracker counts (0 without a tracker)
        uint64_t removes = 0;
        bool looksSparseShaped = false;          // ADVISORY only; nothing is promoted automatically
        std::string advisory;                    // the reason, or empty
    };

    struct RegistrySnapshot
    {
        size_t entityCount = 0;
        size_t archetypeCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;
        size_t bytesAllocated = 0;
        size_t bytesReserved = 0;
        size_t sparseSlotsUsed = 0;          // ComponentStorage::UsedSparseSlotCount() (cap ASTRA_MAX_SPARSE_COMPONENTS)
    };

    struct InspectorSnapshot
    {
        RegistrySnapshot registry;
        std::vector<ArchetypeInfo> archetypes;
        std::vector<StoreInfo> components;   // one entry per listed component, ascending id
        size_t cacheLineBytes = 0;           // so panels never include engine internals
    };

    // Opt-in per-component churn counter for the "sparse-shaped" advisory. Attaching one
    // enables ComponentAdded/ComponentRemoved on the registry (a registry without a
    // tracker pays nothing); the previous signal mask is restored on destruction. Must not
    // outlive the registry, and follows only that registry (a Load builds a new one).
    class ChurnTracker
    {
    public:
        explicit ChurnTracker(Registry& registry)
            : m_registry(&registry), m_previous(registry.GetEnabledSignals())
        {
            registry.EnableSignals(Signal::ComponentAdded | Signal::ComponentRemoved);
            SignalManager* signals = registry.GetSignalManager();
            m_addedId = signals->On<Events::ComponentAdded>().Register(
                [this](const Events::ComponentAdded& ev) { if (ev.componentId < MAX_COMPONENTS) ++m_adds[ev.componentId]; });
            m_removedId = signals->On<Events::ComponentRemoved>().Register(
                [this](const Events::ComponentRemoved& ev) { if (ev.componentId < MAX_COMPONENTS) ++m_removes[ev.componentId]; });
        }

        ~ChurnTracker()
        {
            SignalManager* signals = m_registry->GetSignalManager();
            signals->On<Events::ComponentAdded>().Unregister(m_addedId);
            signals->On<Events::ComponentRemoved>().Unregister(m_removedId);
            m_registry->SetEnabledSignals(m_previous);
        }

        ChurnTracker(const ChurnTracker&) = delete;
        ChurnTracker& operator=(const ChurnTracker&) = delete;

        ASTRA_NODISCARD uint64_t Adds(ComponentID id) const noexcept { return id < MAX_COMPONENTS ? m_adds[id] : 0; }
        ASTRA_NODISCARD uint64_t Removes(ComponentID id) const noexcept { return id < MAX_COMPONENTS ? m_removes[id] : 0; }
        void Reset() noexcept { m_adds.fill(0); m_removes.fill(0); }

    private:
        Registry* m_registry;
        Signal m_previous;
        std::array<uint64_t, MAX_COMPONENTS> m_adds{};
        std::array<uint64_t, MAX_COMPONENTS> m_removes{};
        std::size_t m_addedId = 0;
        std::size_t m_removedId = 0;
    };

    // Advisory thresholds (s9.4). Heuristics, deliberately conservative.
    inline constexpr uint64_t kAdvisoryMinRemoves = 64;          // toggled: at least this many removes...
    inline constexpr size_t kAdvisoryMinPairedArchetypes = 4;    // rare: splits at least this many archetypes...
    inline constexpr size_t kAdvisoryRareDivisor = 10;           // ...while held by < 1/10 of live entities

    inline void Advise(StoreInfo& s, size_t liveEntities)
    {
        if (s.kind != StorageKind::Table)
            return;   // already out of the archetype; nothing to suggest
        if (s.removes >= kAdvisoryMinRemoves && s.removes >= s.population)
        {
            s.looksSparseShaped = true;
            s.advisory += "toggled: " + std::to_string(s.removes) + " removes vs population " +
                          std::to_string(s.population) + "; every add/remove moves the whole entity. ";
        }
        if (s.pairedArchetypeCount >= kAdvisoryMinPairedArchetypes &&
            s.population * kAdvisoryRareDivisor < liveEntities)
        {
            s.looksSparseShaped = true;
            s.advisory += "rare: held by " + std::to_string(s.population) + " of " + std::to_string(liveEntities) +
                          " entities but splits " + std::to_string(s.pairedArchetypeCount) + " archetypes. ";
        }
        if (s.looksSparseShaped)
            s.advisory += "Consider `static constexpr auto AstraStorage = Astra::Storage::Sparse;`.";
    }

    // Capture-into: clears and refills `out`, retaining outer vector capacity.
    // Runs every studio frame -- POD work proportional to chunks x columns,
    // never to entity count; per-entity copies live in CaptureChunkDetail
    // (selected chunk only). Read-only: it never creates a store, a column or a
    // sparse slot (FindSparse/FindUniversal only), so a suspended Universal (module
    // unloaded, spec s4.4) is reported as declared-Universal with no column.
    inline void Capture(Registry& registry, InspectorSnapshot& out, const ChurnTracker* churn = nullptr)
    {
        out.registry = RegistrySnapshot{};
        out.archetypes.clear();
        out.components.clear();
        out.cacheLineBytes = CACHE_LINE_SIZE;

        ArchetypeManager* manager = registry.GetArchetypeManager();
        const ComponentRegistry* components = registry.GetComponentRegistry();
        if (!manager || !components)
            return;

        ComponentStorage* storage = registry.GetComponentStorage();
        std::array<size_t, MAX_COMPONENTS> tablePopulation{};
        std::array<size_t, MAX_COMPONENTS> tableArchetypes{};
        std::array<size_t, MAX_COMPONENTS> tablePaired{};
        std::array<size_t, ASTRA_MAX_SPARSE_COMPONENTS> slotBlocks{};
        std::array<size_t, ASTRA_MAX_SPARSE_COMPONENTS> slotBits{};
        std::array<size_t, ASTRA_MAX_SPARSE_COMPONENTS> slotDisabled{};
        out.registry.sparseSlotsUsed = storage ? storage->UsedSparseSlotCount() : 0;

        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;

            ArchetypeInfo info;
            const ComponentMask& mask = archetype->GetMask();
            info.componentCount = mask.Count();

            // Signature from mask bits (tags included).
            for (size_t bit = 0; bit < MAX_COMPONENTS; ++bit)
            {
                if (!mask.Test(bit))
                    continue;
                // Each mask bit is a Table component (Sparse and Universal never enter masks, s3.1).
                tablePopulation[bit] += archetype->GetEntityCount();
                ++tableArchetypes[bit];
                ComponentMask without = mask;
                without.Reset(bit);
                if (manager->FindArchetype(without) != nullptr)
                    ++tablePaired[bit];   // this archetype exists only because of `bit`
                const ComponentDescriptor* desc =
                    components->GetComponentDescriptor(static_cast<ComponentID>(bit));
                const char* name = (desc && desc->name) ? desc->name : "?";
                if (!info.signature.empty())
                    info.signature += " + ";
                info.signature += name;
                if (desc && desc->is_empty)
                    ++info.tagCount;
            }
            if (info.signature.empty())
                info.signature = "(empty)";

            // Storage-bearing columns from the shared per-archetype metadata.
            const ArchetypeColumnMeta& meta = archetype->GetColumnMeta();
            size_t rowStride = 0;
            info.columns.reserve(meta.columnCount);
            for (uint16_t c = 0; c < meta.columnCount; ++c)
            {
                const auto& col = meta.columns[c];
                ColumnInfo ci;
                ci.id = col.id;
                ci.stride = col.stride;
                if (col.descriptor)
                {
                    ci.name = col.descriptor->name ? col.descriptor->name : "?";
                    ci.size = col.descriptor->size;
                    ci.alignment = col.descriptor->alignment;
                    ci.isEnableable = col.descriptor->isEnableable;
                }
                rowStride += col.stride;
                info.columns.push_back(std::move(ci));
            }

            info.entityCount = archetype->GetEntityCount();
            info.chunkCount = archetype->GetChunkCount();
            info.chunks.reserve(info.chunkCount);
            for (const auto& chunk : archetype->GetChunks())
            {
                if (!chunk)
                    continue;
                ChunkInfo ch;
                ch.count = chunk->GetCount();
                ch.capacity = chunk->GetCapacity();
                ch.chunkBytes = chunk->GetChunkBytes();

                // Columns first (ascending offsets), then their disabled-bit
                // regions (carved after all columns, in enableable-ordinal order,
                // which is also ascending) -- cursor walk yields pad as the gaps.
                size_t cursor = 0;
                ch.columns.reserve(meta.columnCount);
                for (uint16_t c = 0; c < meta.columnCount; ++c)
                {
                    ChunkColumnLayout cl;
                    cl.offset = chunk->GetColumnOffset(c);
                    cl.bytes = size_t(meta.columns[c].stride) * ch.capacity;
                    cl.disabledOffset = chunk->GetDisabledWordsOffset(c);
                    if (cl.disabledOffset != SIZE_MAX)
                    {
                        // ArchetypeChunk::WordsForCapacity is private; mirror its formula
                        // inline, matching the existing duplication in Archetype.hpp's
                        // own capacity math (ComputeLayoutBytesForCapacity et al.).
                        cl.disabledBytes = ((ch.capacity + 63) / 64) * 8;
                        cl.disabledCount = chunk->GetDisabledCount(static_cast<int>(c));
                    }
                    ch.columnBytes += cl.bytes;
                    ch.padBytes += cl.offset - cursor;
                    cursor = cl.offset + cl.bytes;
                    ch.columns.push_back(cl);
                }
                for (const ChunkColumnLayout& cl : ch.columns)
                {
                    if (cl.disabledOffset == SIZE_MAX)
                        continue;
                    ch.padBytes += cl.disabledOffset - cursor;
                    ch.bitsBytes += cl.disabledBytes;
                    cursor = cl.disabledOffset + cl.disabledBytes;
                }
                ch.slackBytes = ch.chunkBytes - cursor;

                // Sparse presence blocks (s5.1): side allocations, reported separately.
                const size_t blockWordBytes = ((ch.capacity + 63) / 64) * 8;
                for (const ArchetypeChunk::SparseBlock& block : chunk->GetSparseBlocks())   // sorted by slot
                {
                    if (block.slot >= ASTRA_MAX_SPARSE_COMPONENTS)
                        continue;
                    SparseBlockInfo bi;
                    bi.slot = block.slot;
                    bi.componentId = storage ? storage->ComponentOfSlot(block.slot) : INVALID_COMPONENT;
                    bi.presentCount = block.presentCount;
                    bi.disabledCount = block.disabledCount;
                    bi.hasPresentWords = block.present != nullptr;
                    bi.hasDisabledWords = block.disabled != nullptr;
                    ch.sparseBlockBytes += (bi.hasPresentWords ? blockWordBytes : 0) + (bi.hasDisabledWords ? blockWordBytes : 0);
                    ++slotBlocks[block.slot];
                    slotBits[block.slot] += block.presentCount;
                    slotDisabled[block.slot] += block.disabledCount;
                    ch.sparseBlocks.push_back(bi);
                }

                info.bytesUsed += ch.count * rowStride;
                info.bytesAllocated += ch.capacity * rowStride;
                info.bytesReserved += ch.chunkBytes;
                info.chunks.push_back(std::move(ch));
            }

            out.registry.entityCount += info.entityCount;
            out.registry.chunkCount += info.chunkCount;
            out.registry.bytesUsed += info.bytesUsed;
            out.registry.bytesAllocated += info.bytesAllocated;
            out.registry.bytesReserved += info.bytesReserved;
            out.archetypes.push_back(std::move(info));
        }
        out.registry.archetypeCount = out.archetypes.size();

        // ---- Per-component storage (s9.4) ------------------------------------------
        std::vector<ComponentDescriptor> descriptors;
        components->GetAllDescriptors(descriptors);   // ascending id
        for (const ComponentDescriptor& d : descriptors)
        {
            if (d.id >= MAX_COMPONENTS)
                continue;
            StoreInfo s;
            s.name = d.name ? d.name : "?";
            s.id = d.id;
            s.size = d.size;
            s.isEnableable = d.isEnableable;
            s.isChangeTracked = d.isChangeTracked;
            s.kind = storage ? storage->KindOf(d.id) : StorageKind::Table;
            s.sparseSlot = storage ? storage->SparseSlotOf(d.id) : -1;
            switch (s.kind)
            {
            case StorageKind::Table:
                s.population = tablePopulation[d.id];
                s.archetypeCount = tableArchetypes[d.id];
                s.pairedArchetypeCount = tablePaired[d.id];
                break;
            case StorageKind::Sparse:
                // Non-creating lookup: a Sparse kind with no set yet reports empty.
                if (const SparseSet* set = storage->FindSparse(d.id))
                {
                    s.population = set->Size();
                    s.pageCount = set->PageCount();
                }
                break;
            case StorageKind::Universal:
                // Declared but empty, or suspended after a module unload: no column, nothing to report.
                if (const UniversalColumn* column = storage->FindUniversal(d.id))
                {
                    s.population = column->LiveCount();
                    s.pageCount = column->PageCount();
                }
                break;
            }
            if (s.sparseSlot >= 0 && s.sparseSlot < static_cast<int>(ASTRA_MAX_SPARSE_COMPONENTS))
            {
                s.presenceBlockCount = slotBlocks[static_cast<size_t>(s.sparseSlot)];
                s.presenceBitsSet = slotBits[static_cast<size_t>(s.sparseSlot)];
                s.disabledCount = slotDisabled[static_cast<size_t>(s.sparseSlot)];
            }
            if (churn)
            {
                s.adds = churn->Adds(d.id);
                s.removes = churn->Removes(d.id);
            }
            // Unused Table components (and resource descriptors sharing the id space) are noise.
            if (s.kind == StorageKind::Table && s.population == 0 && s.adds == 0 && s.removes == 0)
                continue;
            Advise(s, out.registry.entityCount);
            out.components.push_back(std::move(s));
        }
    }

    ASTRA_NODISCARD inline InspectorSnapshot Capture(Registry& registry, const ChurnTracker* churn = nullptr)
    {
        InspectorSnapshot snap;
        Capture(registry, snap, churn);
        return snap;
    }

    // Per-entity data for ONE chunk -- the panel's selected chunk only, so the
    // per-frame cost is O(one chunk's capacity), never O(total entities).
    struct ChunkDetail
    {
        std::vector<Entity> entities;                       // row -> Entity
        std::vector<std::vector<uint64_t>> disabledWords;   // by column ordinal; empty = not enableable
    };

    // Same-thread, same-frame as Capture, so snapshot indices stay consistent.
    // archetypeIndex counts non-null archetypes exactly as Capture does.
    ASTRA_NODISCARD inline bool CaptureChunkDetail(Registry& registry, size_t archetypeIndex,
                                   size_t chunkIndex, ChunkDetail& out)
    {
        out.entities.clear();
        out.disabledWords.clear();

        ArchetypeManager* manager = registry.GetArchetypeManager();
        if (!manager)
            return false;

        Archetype* target = nullptr;
        size_t index = 0;
        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;
            if (index++ == archetypeIndex)
            {
                target = archetype;
                break;
            }
        }
        if (!target)
            return false;

        const auto& chunks = target->GetChunks();
        if (chunkIndex >= chunks.size() || !chunks[chunkIndex])
            return false;
        const auto& chunk = chunks[chunkIndex];

        out.entities = chunk->GetEntities();

        const ArchetypeColumnMeta& meta = target->GetColumnMeta();
        out.disabledWords.resize(meta.columnCount);
        // ArchetypeChunk::WordsForCapacity is private; mirror its formula inline,
        // matching the existing duplication in Capture() above.
        const size_t words = (chunk->GetCapacity() + 63) / 64;
        for (uint16_t e = 0; e < meta.enableableColumnCount; ++e)
        {
            const uint16_t c = meta.enableableColumns[e];
            const uint64_t* w = chunk->GetDisabledWords(static_cast<int>(c));
            // An enableable column always carves a word region; a null here
            // would leave this column indistinguishable from non-enableable.
            ASTRA_ASSERT(w != nullptr, "Enableable column has no disabled-word region");
            if (w)
                out.disabledWords[c].assign(w, w + words);
        }
        return true;
    }
} // namespace Astra::Debug
