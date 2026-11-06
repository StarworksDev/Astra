#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <memory>

using Astra::ArchetypeChunk;
using Astra::ArchetypeChunkPool;
using Astra::ArchetypeColumnMeta;
using Astra::Entity;

namespace
{
    using ChunkPtr = std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>;

    ArchetypeChunkPool::Config SmallPoolConfig()
    {
        ArchetypeChunkPool::Config c;
        c.useHugePages = false;
        c.chunksPerBlock = 4;
        return c;
    }

    // Column-less chunks (the root archetype's shape): presence blocks only need
    // the entity slots, not any column storage.
    class SparseBlockTest : public ::testing::Test
    {
    protected:
        ArchetypeChunkPool pool{SmallPoolConfig()};
        ArchetypeColumnMeta meta;

        ChunkPtr Make(size_t capacity, size_t count)
        {
            ChunkPtr chunk = pool.CreateChunk(capacity, ArchetypeChunkPool::MIN_CHUNK_SIZE, &meta);
            for (size_t i = 0; i < count; ++i)
                chunk->AddEntity(Entity(static_cast<Entity::StorageType>(i + 1), 1), Astra::Tick{1});
            return chunk;
        }
    };
}

TEST_F(SparseBlockTest, MissingBlockMeansAbsent)
{
    ChunkPtr chunk = Make(64, 4);
    EXPECT_EQ(chunk->FindSparseBlock(3), nullptr);
    EXPECT_FALSE(chunk->IsSparsePresent(3, 1));
    EXPECT_FALSE(chunk->IsSparseDisabled(3, 1));
    EXPECT_FALSE(chunk->HasSparseBlocks());
}

TEST_F(SparseBlockTest, SetPresentCreatesBlockLazilyAndCounts)
{
    ChunkPtr chunk = Make(64, 8);
    EXPECT_TRUE(chunk->SetSparsePresent(3, 5, true));
    EXPECT_FALSE(chunk->SetSparsePresent(3, 5, true));   // idempotent: no change
    const ArchetypeChunk::SparseBlock* b = chunk->FindSparseBlock(3);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(b->present, nullptr);
    EXPECT_EQ(b->disabled, nullptr);                      // disabled words are lazy too
    EXPECT_EQ(b->presentCount, 1u);
    EXPECT_TRUE(chunk->IsSparsePresent(3, 5));
    EXPECT_FALSE(chunk->IsSparsePresent(3, 4));
    EXPECT_TRUE(chunk->SetSparsePresent(3, 5, false));
    EXPECT_EQ(chunk->FindSparseBlock(3)->presentCount, 0u);
}

TEST_F(SparseBlockTest, BlocksStaySortedBySlot)
{
    ChunkPtr chunk = Make(64, 2);
    chunk->SetSparsePresent(7, 0, true);
    chunk->SetSparsePresent(2, 1, true);
    chunk->SetSparsePresent(5, 0, true);
    const auto blocks = chunk->GetSparseBlocks();
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].slot, 2u);
    EXPECT_EQ(blocks[1].slot, 5u);
    EXPECT_EQ(blocks[2].slot, 7u);
}

TEST_F(SparseBlockTest, ClearingPresentClearsDisabled)
{
    ChunkPtr chunk = Make(64, 4);
    chunk->SetSparsePresent(1, 2, true);
    EXPECT_TRUE(chunk->SetSparseDisabled(1, 2, true));
    EXPECT_EQ(chunk->FindSparseBlock(1)->disabledCount, 1u);
    chunk->SetSparsePresent(1, 2, false);
    EXPECT_FALSE(chunk->IsSparseDisabled(1, 2));
    EXPECT_EQ(chunk->FindSparseBlock(1)->disabledCount, 0u);
}

TEST_F(SparseBlockTest, UniversalMirrorBlockHasNoPresentWords)
{
    ChunkPtr chunk = Make(64, 6);
    EXPECT_TRUE(chunk->SetSparseDisabled(9, 4, true));
    const ArchetypeChunk::SparseBlock* b = chunk->FindSparseBlock(9);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->present, nullptr);
    EXPECT_EQ(b->disabledCount, 1u);
    EXPECT_TRUE(chunk->IsSparseDisabled(9, 4));
    EXPECT_FALSE(chunk->IsSparsePresent(9, 4));   // no present words: the caller knows it is Universal
}

TEST_F(SparseBlockTest, SwapRemoveMovesTailBitsAndClearsTail)
{
    ChunkPtr chunk = Make(64, 4);
    chunk->SetSparsePresent(1, 3, true);    // tail (index 3) is a member of slot 1
    chunk->SetSparseDisabled(1, 3, true);
    chunk->SetSparsePresent(2, 1, true);    // removed slot (index 1) is a member of slot 2 only
    chunk->SwapRemoveSparseBits(1, 3);
    EXPECT_TRUE(chunk->IsSparsePresent(1, 1));
    EXPECT_TRUE(chunk->IsSparseDisabled(1, 1));
    EXPECT_FALSE(chunk->IsSparsePresent(1, 3));
    EXPECT_FALSE(chunk->IsSparseDisabled(1, 3));
    EXPECT_FALSE(chunk->IsSparsePresent(2, 1));   // the removed entity's membership is gone
    EXPECT_EQ(chunk->FindSparseBlock(1)->presentCount, 1u);
    EXPECT_EQ(chunk->FindSparseBlock(1)->disabledCount, 1u);
    EXPECT_EQ(chunk->FindSparseBlock(2)->presentCount, 0u);
}

TEST_F(SparseBlockTest, SwapRemoveOfTheTailClearsOnlyTheTail)
{
    ChunkPtr chunk = Make(64, 4);
    chunk->SetSparsePresent(1, 3, true);
    chunk->SetSparsePresent(1, 0, true);
    chunk->SwapRemoveSparseBits(3, 3);
    EXPECT_FALSE(chunk->IsSparsePresent(1, 3));
    EXPECT_TRUE(chunk->IsSparsePresent(1, 0));
    EXPECT_EQ(chunk->FindSparseBlock(1)->presentCount, 1u);
}

TEST_F(SparseBlockTest, CarryCopiesEverySetBitAndAllocatesLazily)
{
    ChunkPtr src = Make(64, 4);
    ChunkPtr dst = Make(64, 1);
    src->SetSparsePresent(1, 2, true);
    src->SetSparseDisabled(1, 2, true);
    src->SetSparsePresent(4, 2, true);
    src->SetSparseDisabled(6, 2, true);      // Universal mirror block (no present words)
    src->SetSparsePresent(8, 0, true);       // a different slot index: not carried
    dst->CarrySparseBits(*src, 2, 0);
    EXPECT_TRUE(dst->IsSparsePresent(1, 0));
    EXPECT_TRUE(dst->IsSparseDisabled(1, 0));
    EXPECT_TRUE(dst->IsSparsePresent(4, 0));
    EXPECT_FALSE(dst->IsSparseDisabled(4, 0));
    ASSERT_NE(dst->FindSparseBlock(6), nullptr);
    EXPECT_EQ(dst->FindSparseBlock(6)->present, nullptr);   // mirror stays a mirror
    EXPECT_TRUE(dst->IsSparseDisabled(6, 0));
    EXPECT_EQ(dst->FindSparseBlock(8), nullptr);            // never created for an unset bit
    EXPECT_EQ(dst->GetSparseBlocks().size(), 3u);
}

TEST_F(SparseBlockTest, CountsMatchPopcountAcrossWords)
{
    ChunkPtr chunk = Make(200, 200);    // 4 words per block
    for (size_t i : {size_t(0), size_t(63), size_t(64), size_t(199)})
        chunk->SetSparsePresent(0, i, true);
    EXPECT_EQ(chunk->FindSparseBlock(0)->presentCount, 4u);
    EXPECT_TRUE(chunk->IsSparsePresent(0, 199));
    EXPECT_FALSE(chunk->IsSparsePresent(0, 198));
}

TEST_F(SparseBlockTest, WordsReturnToThePoolWithTheChunkAndAreReused)
{
    const auto before = pool.GetSparseWordStats();
    {
        ChunkPtr chunk = Make(64, 4);
        chunk->SetSparsePresent(1, 0, true);
        chunk->SetSparsePresent(2, 1, true);
        chunk->SetSparseDisabled(2, 1, true);                // 3 word blocks live
        EXPECT_EQ(pool.GetSparseWordStats().liveBlocks, before.liveBlocks + 3);
    }   // chunk freed: its blocks go back to the freelist
    const auto after = pool.GetSparseWordStats();
    EXPECT_EQ(after.liveBlocks, before.liveBlocks);
    EXPECT_EQ(after.cachedBlocks, before.cachedBlocks + 3);

    ChunkPtr again = Make(64, 1);
    again->SetSparsePresent(5, 0, true);                     // reuses a cached block
    EXPECT_EQ(pool.GetSparseWordStats().cachedBlocks, after.cachedBlocks - 1);
}

TEST_F(SparseBlockTest, ReleaseSparseBlockDropsOneSlot)
{
    ChunkPtr chunk = Make(64, 2);
    chunk->SetSparsePresent(1, 0, true);
    chunk->SetSparsePresent(3, 1, true);
    const size_t live = pool.GetSparseWordStats().liveBlocks;
    chunk->ReleaseSparseBlock(1);
    EXPECT_EQ(chunk->FindSparseBlock(1), nullptr);
    EXPECT_NE(chunk->FindSparseBlock(3), nullptr);
    EXPECT_EQ(pool.GetSparseWordStats().liveBlocks, live - 1);
}

TEST_F(SparseBlockTest, PoolDefragmentTrimsCachedWords)
{
    {
        ChunkPtr chunk = Make(64, 1);
        chunk->SetSparsePresent(1, 0, true);
    }
    EXPECT_GT(pool.GetSparseWordStats().cachedBlocks, 0u);
    pool.Defragment();
    EXPECT_EQ(pool.GetSparseWordStats().cachedBlocks, 0u);
}
