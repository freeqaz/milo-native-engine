// A movie opened from memory (FFmpegMovieImpl::BeginFromMemory).
//
// Retail opens a PRELOADED movie from its buffer: RB3 TU5's Movie::Impl
// CheckOpen keeps the FileLoader's buffer and calls MovieOpen(buffer,
// BINKFROMMEMORY | BINKNOFRAMEBUFFERS).  A cached .milo can embed a movie
// (rb3's ui/startup/gen/startup_movie_keep.milo_xbox: splash.tmov carries
// startup.bik, in the 360's word-swapped order "iKIB"), and there is no file to
// open.  These tests open the same Bink from a file and from memory, in both
// byte orders, and check the memory reader's seek and bounds.
//
// Sabotage record (each revert re-run, test watched FAIL, fix restored):
//   * Read ignoring the word swap (plain memcpy)
//       -> UnswapsXboxWordOrder, WordSwappedBinkDecodesTheSame FAIL
//   * Seek accepting a target past the end
//       -> ReadsAndSeeksInRange FAIL
//   * Close() not deleting the reader (OpenFromMemory stays true after End)
//       -> OpensBinkFromMemoryLikeTheFile FAIL

#include <gtest/gtest.h>

#include "test_helpers.h"
#include "os/File.h"
#ifdef HX_FFMPEG // FFmpegMovieImpl is in the engine only when FFmpeg was found
#include "platform/FFmpegMovieImpl.h"
#endif

#ifdef MILO_MOVIE_MEMORY

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

const char *kBik = "songs/ymca/ymca_prev.bik";

std::vector<uint8_t> ReadGameFile(const char *path) {
    std::vector<uint8_t> buf;
    File *f = NewFile(path, 2);
    if (!f)
        return buf;
    buf.resize(f->Size());
    int got = f->Read(buf.data(), (int)buf.size());
    delete f;
    if (got != (int)buf.size())
        buf.clear();
    return buf;
}

// The 360's order: every 32-bit word byte-reversed (EndianSwapBuffer).
std::vector<uint8_t> WordSwap(std::vector<uint8_t> v) {
    v.resize((v.size() + 3) & ~size_t(3), 0);
    for (size_t i = 0; i < v.size(); i += 4) {
        std::swap(v[i], v[i + 3]);
        std::swap(v[i + 1], v[i + 2]);
    }
    return v;
}

// Decode up to `ms` of movie time and return the last decoded picture.
std::vector<uint8_t> PictureAt(FFmpegMovieImpl &m, float ms) {
    m.SetVirtualTime(ms);
    m.Poll();
    const uint8_t *p = m.GetRGBABuffer();
    return std::vector<uint8_t>(p, p + (size_t)m.GetDecodedWidth() * m.GetDecodedHeight() * 4);
}

uint64_t Fnv(const std::vector<uint8_t> &v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v)
        h = (h ^ b) * 1099511628211ull;
    return h;
}

} // namespace

TEST(MovieMemoryReader, ReadsAndSeeksInRange) {
    std::vector<uint8_t> buf(100);
    for (int i = 0; i < 100; i++)
        buf[i] = (uint8_t)i;
    MovieMemoryReader r(buf.data(), (int64_t)buf.size());
    ASSERT_TRUE(r.Valid());
    EXPECT_FALSE(r.WordSwapped());
    uint8_t out[16];
    ASSERT_EQ(r.Read(out, 10), 10);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[9], 9);
    EXPECT_EQ(r.Seek(5, SEEK_CUR), 15);
    ASSERT_EQ(r.Read(out, 1), 1);
    EXPECT_EQ(out[0], 15);
    EXPECT_EQ(r.Seek(-4, SEEK_END), 96);
    ASSERT_EQ(r.Read(out, 16), 4); // short read at the end
    EXPECT_EQ(out[3], 99);
    EXPECT_EQ(r.Read(out, 16), AVERROR_EOF);
    EXPECT_EQ(r.Seek(0, AVSEEK_SIZE), 100);
    EXPECT_EQ(r.Seek(7, SEEK_SET | AVSEEK_FORCE), 7);
    // Out of range: refused, position unchanged.
    EXPECT_LT(r.Seek(-1, SEEK_SET), 0);
    EXPECT_LT(r.Seek(101, SEEK_SET), 0);
    EXPECT_LT(r.Seek(1, SEEK_END), 0);
    EXPECT_EQ(r.Pos(), 7);
    EXPECT_EQ(r.Seek(100, SEEK_SET), 100); // the end itself is a valid position
    EXPECT_EQ(r.Read(out, 1), AVERROR_EOF);
}

TEST(MovieMemoryReader, UnswapsXboxWordOrder) {
    // PC order "BIKi" + 0x01020304 + "abcd"; the 360 stores each word reversed.
    const uint8_t pc[12] = {'B', 'I', 'K', 'i', 4, 3, 2, 1, 'a', 'b', 'c', 'd'};
    const uint8_t xbox[12] = {'i', 'K', 'I', 'B', 1, 2, 3, 4, 'd', 'c', 'b', 'a'};
    EXPECT_TRUE(MovieMemoryReader::IsWordSwappedBink(xbox, 12));
    EXPECT_FALSE(MovieMemoryReader::IsWordSwappedBink(pc, 12));
    const uint8_t kb2[4] = {'j', '2', 'B', 'K'}; // Bink 2 "KB2j", swapped
    EXPECT_TRUE(MovieMemoryReader::IsWordSwappedBink(kb2, 4));

    MovieMemoryReader r(xbox, 12);
    ASSERT_TRUE(r.Valid());
    EXPECT_TRUE(r.WordSwapped());
    uint8_t out[12];
    ASSERT_EQ(r.Read(out, 12), 12);
    EXPECT_EQ(0, memcmp(out, pc, 12));
    // Unaligned reads after a seek see the same PC-order bytes.
    ASSERT_EQ(r.Seek(3, SEEK_SET), 3);
    ASSERT_EQ(r.Read(out, 6), 6);
    EXPECT_EQ(0, memcmp(out, pc + 3, 6));
    // The caller's bytes are never modified.
    EXPECT_EQ(xbox[0], 'i');

    // A swapped buffer that is not whole words is refused (EndianSwapBuffer's
    // size % 4 assert); a PC-order one of any size is fine.
    EXPECT_FALSE(MovieMemoryReader(xbox, 11).Valid());
    EXPECT_TRUE(MovieMemoryReader(pc, 11).Valid());
    EXPECT_FALSE(MovieMemoryReader(nullptr, 12).Valid());
    EXPECT_FALSE(MovieMemoryReader(pc, 0).Valid());
}

class FFmpegMovieMemory : public EngineTestFixture {};

TEST_F(FFmpegMovieMemory, OpensBinkFromMemoryLikeTheFile) {
    std::vector<uint8_t> bytes = ReadGameFile(kBik);
    if (bytes.empty())
        GTEST_SKIP() << kBik << " is not in the archive";
    FFmpegMovieImpl file, mem;
    ASSERT_TRUE(file.BeginFromFile(kBik, 0.0f, true, false, true, false, 0, nullptr, kLoadFront));
    ASSERT_TRUE(mem.BeginFromMemory(bytes.data(), (int)bytes.size(), kBik, true, false, 0));
    EXPECT_TRUE(mem.IsOpen());
    EXPECT_TRUE(mem.OpenFromMemory());
    EXPECT_FALSE(file.OpenFromMemory());
    EXPECT_EQ(mem.GetDecodedWidth(), file.GetDecodedWidth());
    EXPECT_EQ(mem.GetDecodedHeight(), file.GetDecodedHeight());
    EXPECT_EQ(mem.NumFrames(), file.NumFrames());
    EXPECT_GT(mem.NumFrames(), 10);
    EXPECT_FLOAT_EQ(mem.MsPerFrame(), file.MsPerFrame());
    // Two times: the pictures change over time and match the file's.
    float t1 = mem.MsPerFrame() * 2.5f, t2 = mem.MsPerFrame() * 9.5f;
    std::vector<uint8_t> f1 = PictureAt(file, t1), m1 = PictureAt(mem, t1);
    EXPECT_EQ(mem.GetFrame(), file.GetFrame());
    std::vector<uint8_t> f2 = PictureAt(file, t2), m2 = PictureAt(mem, t2);
    EXPECT_EQ(mem.GetFrame(), file.GetFrame());
    ASSERT_FALSE(m1.empty());
    EXPECT_EQ(Fnv(m1), Fnv(f1));
    EXPECT_EQ(Fnv(m2), Fnv(f2));
    EXPECT_NE(Fnv(m1), Fnv(m2));
    mem.End();
    EXPECT_FALSE(mem.IsOpen());
    EXPECT_FALSE(mem.OpenFromMemory());
    EXPECT_EQ(mem.MemoryReader(), nullptr);
}

TEST_F(FFmpegMovieMemory, WordSwappedBinkDecodesTheSame) {
    std::vector<uint8_t> bytes = ReadGameFile(kBik);
    if (bytes.empty())
        GTEST_SKIP() << kBik << " is not in the archive";
    std::vector<uint8_t> swapped = WordSwap(bytes);
    ASSERT_TRUE(MovieMemoryReader::IsWordSwappedBink(swapped.data(), (int64_t)swapped.size()));
    uint64_t before = Fnv(swapped);
    FFmpegMovieImpl pc, xbox;
    ASSERT_TRUE(pc.BeginFromMemory(bytes.data(), (int)bytes.size(), kBik, true, false, 0));
    ASSERT_TRUE(xbox.BeginFromMemory(swapped.data(), (int)swapped.size(), kBik, true, false, 0));
    ASSERT_NE(xbox.MemoryReader(), nullptr);
    EXPECT_TRUE(xbox.MemoryReader()->WordSwapped());
    EXPECT_EQ(xbox.NumFrames(), pc.NumFrames());
    float t = pc.MsPerFrame() * 6.5f;
    EXPECT_EQ(Fnv(PictureAt(xbox, t)), Fnv(PictureAt(pc, t)));
    EXPECT_EQ(xbox.GetFrame(), pc.GetFrame());
    EXPECT_EQ(Fnv(swapped), before); // read in place, never swapped in place
}

// A looping movie seeks the demuxer back to the start through the reader.
TEST_F(FFmpegMovieMemory, LoopSeeksBackInMemory) {
    std::vector<uint8_t> bytes = ReadGameFile(kBik);
    if (bytes.empty())
        GTEST_SKIP() << kBik << " is not in the archive";
    FFmpegMovieImpl m;
    ASSERT_TRUE(m.BeginFromMemory(bytes.data(), (int)bytes.size(), kBik, true, true, 0));
    float per = m.MsPerFrame();
    int frames = m.NumFrames();
    std::vector<uint8_t> first = PictureAt(m, per * 0.5f);
    float end = per * (frames + 2);
    m.SetVirtualTime(end);
    EXPECT_TRUE(m.Poll()); // past the end: restarts, still playing
    EXPECT_LT(m.GetFrame(), 3);
    // The next pass starts at `end`: its first picture is the first frame again.
    std::vector<uint8_t> again = PictureAt(m, end + per * 0.5f);
    EXPECT_EQ(Fnv(again), Fnv(first));
    EXPECT_TRUE(m.MemoryReader()->Pos() > 0);
}

TEST_F(FFmpegMovieMemory, TruncatedBufferFailsCleanly) {
    std::vector<uint8_t> bytes = ReadGameFile(kBik);
    if (bytes.empty())
        GTEST_SKIP() << kBik << " is not in the archive";
    FFmpegMovieImpl m;
    EXPECT_FALSE(m.BeginFromMemory(nullptr, 100, "null", true, false, 0));
    EXPECT_FALSE(m.IsOpen());
    EXPECT_FALSE(m.BeginFromMemory(bytes.data(), 0, "empty", true, false, 0));
    EXPECT_FALSE(m.BeginFromMemory(bytes.data(), 16, "header-cut", true, false, 0));
    EXPECT_FALSE(m.IsOpen());
    EXPECT_FALSE(m.OpenFromMemory());
    EXPECT_FALSE(m.Poll()); // not open = done, as BinkMovieImpl::Poll with no HBINK
    // Cut mid-stream: it may open (the header and index are whole) but it must
    // end, never read past the buffer, and never loop forever.
    int half = (int)bytes.size() / 2;
    std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + half);
    if (m.BeginFromMemory(cut.data(), half, "half", true, false, 0)) {
        float per = m.MsPerFrame();
        bool playing = true;
        for (int i = 1; i <= m.NumFrames() + 4 && playing; i++) {
            m.SetVirtualTime(per * i);
            playing = m.Poll();
        }
        EXPECT_FALSE(playing);
        EXPECT_LE(m.MemoryReader()->Pos(), (int64_t)half);
    }
    m.End();
    EXPECT_FALSE(m.IsOpen());
}

#endif // MILO_MOVIE_MEMORY
