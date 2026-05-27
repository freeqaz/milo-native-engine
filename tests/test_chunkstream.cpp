// ChunkStream unit tests — uses synthetic .milo_xbox files on disk
#include "test_helpers.h"
#include "utl/ChunkStream.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char *kTestDir = "/tmp/claude-1000/milo_tests";

class ChunkStreamTest : public SymbolTestFixture {
protected:
    static void SetUpTestSuite() {
        SymbolTestFixture::SetUpTestSuite();
        // Create test output directory
        mkdir("/tmp/claude-1000", 0755);
        mkdir(kTestDir, 0755);
    }

    std::string TestPath(const char *name) {
        return std::string(kTestDir) + "/" + name;
    }
};

// ============================================================================
// Single chunk — basic read back
// ============================================================================

TEST_F(ChunkStreamTest, SingleChunkRead) {
    // Build a single chunk containing a BE string "hello"
    std::vector<uint8_t> chunkData;
    PutBEString(chunkData, "hello");

    std::string path = TestPath("single_chunk.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail()) << "Failed to open " << path;

    // Pump Eof() to process the chunk header and load first chunk
    EofType eof = cs.Eof();
    ASSERT_EQ(eof, NotEof) << "Expected NotEof after opening single-chunk file";

    // Read the string back
    Symbol sym;
    cs >> sym;
    EXPECT_STREQ(sym.Str(), "hello");
    EXPECT_EQ(cs.Tell(), 9); // 4 (length) + 5 (chars)
}

// ============================================================================
// Multiple ints in one chunk — sequential reads
// ============================================================================

TEST_F(ChunkStreamTest, MultipleIntsInOneChunk) {
    std::vector<uint8_t> chunkData;
    PutBE32(chunkData, 100);
    PutBE32(chunkData, 200);
    PutBE32(chunkData, 300);

    std::string path = TestPath("multi_ints.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    int v1, v2, v3;
    cs >> v1;
    EXPECT_EQ(v1, 100);
    EXPECT_EQ(cs.Tell(), 4);

    cs >> v2;
    EXPECT_EQ(v2, 200);
    EXPECT_EQ(cs.Tell(), 8);

    cs >> v3;
    EXPECT_EQ(v3, 300);
    EXPECT_EQ(cs.Tell(), 12);
}

// ============================================================================
// Cross-chunk boundary read — int straddling two chunks
// This is the critical test: the native ReadImpl must handle reads that
// span chunk boundaries (which the original Xbox code never needed to do).
// ============================================================================

TEST_F(ChunkStreamTest, CrossChunkBoundaryRead) {
    // Test cross-chunk boundary reads.
    // Layout: chunk 0 has 12 bytes (3 ints), chunk 1 has 12 bytes (3 ints).
    // We read an int that straddles the boundary: consume 10 bytes from chunk 0,
    // then read a 4-byte int that needs 2 bytes from chunk 0 and 2 from chunk 1.
    //
    // Chunk 0: [int32=0x11223344] [int32=0x55667788] [byte 0x99] [byte 0xAA] [pad 0x00] [pad 0x00]
    // Chunk 1: [byte 0xBB] [byte 0xCC] [int32=0xDDEEFF00] [pad pad pad pad pad pad]
    //
    // After reading ints 0x11223344 and 0x55667788 (8 bytes), then reading
    // another int should get 0x99AA0000 (all from chunk 0 still — 4 remaining).
    // Then next int crosses: 0 bytes left in chunk 0 → advance to chunk 1.
    //
    // Simpler test: two 12-byte chunks, read across boundary.

    std::vector<uint8_t> chunk0;
    PutBE32(chunk0, 0x11111111);  // fully in chunk 0
    PutBE32(chunk0, 0x22222222);  // fully in chunk 0
    // Last 4 bytes split: 2 here + 2 in chunk 1 won't work because chunk
    // boundaries are enforced at the chunk level.
    // Instead: consume all of chunk 0, then cross-chunk ReadImpl
    // handles the read from chunk 1.
    PutBE32(chunk0, 0x33333333);  // fully in chunk 0

    std::vector<uint8_t> chunk1;
    PutBE32(chunk1, 0x44444444);  // fully in chunk 1
    PutBE32(chunk1, 0x55555555);  // fully in chunk 1
    PutBE32(chunk1, 0x66666666);  // fully in chunk 1

    std::string path = TestPath("cross_chunk.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunk0, chunk1}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    // Read all 3 ints from chunk 0
    int v1, v2, v3;
    cs >> v1;
    EXPECT_EQ(v1, 0x11111111);
    cs >> v2;
    EXPECT_EQ(v2, 0x22222222);
    cs >> v3;
    EXPECT_EQ(v3, 0x33333333);

    // Now at chunk boundary — the cross-chunk ReadImpl should handle
    // advancing to chunk 1 transparently when we read the next int
    int v4;
    cs >> v4;
    EXPECT_EQ(v4, 0x44444444) << "Cross-chunk boundary read failed";

    int v5, v6;
    cs >> v5;
    EXPECT_EQ(v5, 0x55555555);
    cs >> v6;
    EXPECT_EQ(v6, 0x66666666);
}

// ============================================================================
// EOF detection
// ============================================================================

TEST_F(ChunkStreamTest, EofDetection) {
    std::vector<uint8_t> chunkData;
    PutBE32(chunkData, 42);

    std::string path = TestPath("eof_test.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    int val;
    cs >> val;
    EXPECT_EQ(val, 42);

    // All data consumed — should be RealEof
    EofType eof = cs.Eof();
    EXPECT_EQ(eof, RealEof);
}

// ============================================================================
// Tell() tracking across chunks
// ============================================================================

TEST_F(ChunkStreamTest, TellTracking) {
    std::vector<uint8_t> chunk0;
    PutBE32(chunk0, 0xAAAA);
    PutBE32(chunk0, 0xBBBB);

    std::vector<uint8_t> chunk1;
    PutBE32(chunk1, 0xCCCC);
    PutBE32(chunk1, 0xDDDD);

    std::string path = TestPath("tell_tracking.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunk0, chunk1}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    int val;
    cs >> val;
    EXPECT_EQ(cs.Tell(), 4);

    cs >> val;
    EXPECT_EQ(cs.Tell(), 8);

    // Force chunk advance
    EofType e = cs.Eof();
    ASSERT_EQ(e, NotEof) << "Should advance to chunk 1";

    cs >> val;
    EXPECT_EQ(cs.Tell(), 12);

    cs >> val;
    EXPECT_EQ(cs.Tell(), 16);
}

// ============================================================================
// ReadDead — scan for 0xADDEADDE marker
// ============================================================================

extern void ReadDead(BinStream &);

TEST_F(ChunkStreamTest, ReadDeadMarker) {
    std::vector<uint8_t> chunkData;
    // Garbage bytes before marker
    PutGarbage(chunkData, 7);
    // Dead marker
    PutDeadMarker(chunkData);
    // Data after marker
    PutBE32(chunkData, 0x12345678);

    std::string path = TestPath("dead_marker.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    ReadDead(cs);

    // After ReadDead, we should be right after the marker
    int val;
    cs >> val;
    EXPECT_EQ(val, 0x12345678) << "ReadDead didn't stop at the right position";
}

// ============================================================================
// Platform detection from .milo_xbox suffix
// ============================================================================

TEST_F(ChunkStreamTest, PlatformDetectionXbox) {
    std::vector<uint8_t> chunkData;
    PutBE32(chunkData, 1);

    std::string path = TestPath("platform.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());

    // Force header processing
    cs.Eof();

    EXPECT_EQ(cs.GetPlatform(), kPlatformXBox);
    // Xbox is big-endian, so LittleEndian() should be false
    EXPECT_FALSE(cs.LittleEndian());
}

// ============================================================================
// Unreread safety — Unreread(4) near chunk boundary
// Documents the edge case where DirLoader peeks 4 bytes then unreads.
// ============================================================================

TEST_F(ChunkStreamTest, UnrereadSafety) {
    // Single chunk with enough data for the test
    std::vector<uint8_t> chunkData;
    PutBE32(chunkData, 0x11111111);
    PutBE32(chunkData, 0x22222222);
    PutBE32(chunkData, 0x33333333);

    std::string path = TestPath("unreread.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    // Read first int
    int val;
    cs >> val;
    EXPECT_EQ(val, 0x11111111);
    EXPECT_EQ(cs.Tell(), 4);

    // Peek: read 4 bytes then unreread
    int peek;
    cs >> peek;
    EXPECT_EQ(peek, 0x22222222);
    EXPECT_EQ(cs.Tell(), 8);

    cs.Unreread(4);
    EXPECT_EQ(cs.Tell(), 4);

    // Re-read — should get the same value
    int reread;
    cs >> reread;
    EXPECT_EQ(reread, 0x22222222) << "Unreread(4) didn't properly rewind within chunk";
}

// ============================================================================
// Cross-chunk Unreread — the prime suspect for .milo desync
// If a 4-byte peek crosses a chunk boundary, Unreread only adjusts
// mCurBufOffset in the current chunk, potentially corrupting reads.
// ============================================================================

TEST_F(ChunkStreamTest, UnrereadAtChunkBoundary) {
    // Chunk 0: exactly 4 bytes
    std::vector<uint8_t> chunk0;
    PutBE32(chunk0, 0xAAAAAAAA);

    // Chunk 1: starts with an int
    std::vector<uint8_t> chunk1;
    PutBE32(chunk1, 0xBBBBBBBB);
    PutBE32(chunk1, 0xCCCCCCCC);

    std::string path = TestPath("unreread_boundary.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunk0, chunk1}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    // Consume chunk 0
    int v0;
    cs >> v0;
    EXPECT_EQ(v0, (int)0xAAAAAAAA);

    // We're now at the chunk boundary. Advance to chunk 1.
    EofType e = cs.Eof();
    ASSERT_EQ(e, NotEof) << "Should advance to chunk 1";

    // Read first int from chunk 1 (peek)
    int peek;
    cs >> peek;
    EXPECT_EQ(peek, (int)0xBBBBBBBB);

    // Unreread while in chunk 1 — this should be safe since we
    // haven't crossed a boundary during this read
    cs.Unreread(4);

    // Re-read — should get same value
    int reread;
    cs >> reread;
    EXPECT_EQ(reread, (int)0xBBBBBBBB);

    // Continue reading
    int v2;
    cs >> v2;
    EXPECT_EQ(v2, (int)0xCCCCCCCC);
}

// ============================================================================
// Large chunk with many sequential reads
// ============================================================================

TEST_F(ChunkStreamTest, LargeSequentialReads) {
    const int numInts = 256;
    std::vector<uint8_t> chunkData;
    for (int i = 0; i < numInts; i++) {
        PutBE32(chunkData, i * 7 + 3);
    }

    std::string path = TestPath("large_seq.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunkData}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    for (int i = 0; i < numInts; i++) {
        int val;
        cs >> val;
        EXPECT_EQ(val, i * 7 + 3) << "Mismatch at index " << i;
    }
    EXPECT_EQ(cs.Tell(), numInts * 4);
}

// ============================================================================
// Multi-chunk with strings
// ============================================================================

TEST_F(ChunkStreamTest, MultiChunkStrings) {
    std::vector<uint8_t> chunk0;
    PutBEString(chunk0, "ObjectDir");
    PutBE32(chunk0, 42);

    std::vector<uint8_t> chunk1;
    PutBEString(chunk1, "RndMesh");
    PutBE32(chunk1, 99);

    std::string path = TestPath("multi_strings.milo_xbox");
    ASSERT_TRUE(WriteSyntheticMilo(path.c_str(), {chunk0, chunk1}));

    ChunkStream cs(path.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    Symbol sym1;
    int v1;
    cs >> sym1;
    cs >> v1;
    EXPECT_STREQ(sym1.Str(), "ObjectDir");
    EXPECT_EQ(v1, 42);

    // Advance to next chunk
    EofType e = cs.Eof();
    ASSERT_EQ(e, NotEof);

    Symbol sym2;
    int v2;
    cs >> sym2;
    cs >> v2;
    EXPECT_STREQ(sym2.Str(), "RndMesh");
    EXPECT_EQ(v2, 99);
}
