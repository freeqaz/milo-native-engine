// BinStream unit tests — pure in-memory, no engine init needed
#include "test_helpers.h"

// ============================================================================
// Big-endian reads (mLittleEndian=false → Xbox .milo format)
// On native x86_64, ReadEndian swaps BE file data to LE host order.
// ============================================================================

TEST(BinStreamEndian, ReadBE_Int32) {
    // 0x12345678 stored big-endian in file
    std::vector<uint8_t> buf;
    PutBE32(buf, 0x12345678);

    MemBinStream ms(buf.data(), buf.size(), /*littleEndian=*/false);
    int val;
    ms >> val;
    EXPECT_EQ(val, 0x12345678);
    EXPECT_EQ(ms.Tell(), 4);
    EXPECT_FALSE(ms.Fail());
}

TEST(BinStreamEndian, ReadBE_Int16) {
    std::vector<uint8_t> buf;
    PutBE16(buf, 0xABCD);

    MemBinStream ms(buf.data(), buf.size(), false);
    short val;
    ms >> val;
    EXPECT_EQ((unsigned short)val, 0xABCD);
    EXPECT_EQ(ms.Tell(), 2);
}

TEST(BinStreamEndian, ReadBE_Float) {
    float expected = 3.14f;
    std::vector<uint8_t> buf;
    PutBEFloat(buf, expected);

    MemBinStream ms(buf.data(), buf.size(), false);
    float val;
    ms >> val;
    EXPECT_FLOAT_EQ(val, expected);
}

TEST(BinStreamEndian, ReadBE_Double) {
    double expected = 2.718281828;
    std::vector<uint8_t> buf;
    PutBEDouble(buf, expected);

    MemBinStream ms(buf.data(), buf.size(), false);
    double val;
    ms >> val;
    EXPECT_DOUBLE_EQ(val, expected);
}

// ============================================================================
// Little-endian reads (mLittleEndian=true → PC format)
// On native x86_64, no swap needed since host and file are both LE.
// ============================================================================

TEST(BinStreamEndian, ReadLE_Int32) {
    std::vector<uint8_t> buf;
    PutLE32(buf, 0x12345678);

    MemBinStream ms(buf.data(), buf.size(), /*littleEndian=*/true);
    int val;
    ms >> val;
    EXPECT_EQ(val, 0x12345678);
}

// ============================================================================
// String/Symbol reading (requires Symbol::PreInit)
// ============================================================================

class BinStreamSymbolTest : public SymbolTestFixture {};

TEST_F(BinStreamSymbolTest, ReadBE_Symbol) {
    // Symbol format: BE length prefix + raw chars
    std::vector<uint8_t> buf;
    PutBEString(buf, "hello");

    MemBinStream ms(buf.data(), buf.size(), false);
    Symbol sym;
    ms >> sym;
    EXPECT_STREQ(sym.Str(), "hello");
    // 4 bytes length + 5 chars
    EXPECT_EQ(ms.Tell(), 9);
}

TEST_F(BinStreamSymbolTest, ReadBE_EmptySymbol) {
    std::vector<uint8_t> buf;
    PutBEString(buf, "");

    MemBinStream ms(buf.data(), buf.size(), false);
    Symbol sym;
    ms >> sym;
    EXPECT_STREQ(sym.Str(), "");
    EXPECT_EQ(ms.Tell(), 4);
}

// ============================================================================
// Sequential reads — verify Tell() tracking
// ============================================================================

TEST_F(BinStreamSymbolTest, SequentialReads) {
    std::vector<uint8_t> buf;
    PutBE32(buf, 42);          // int at offset 0
    PutBEString(buf, "test");  // symbol at offset 4 (4 + 4 = 8 bytes)
    PutBE32(buf, 99);          // int at offset 12

    MemBinStream ms(buf.data(), buf.size(), false);

    int v1;
    ms >> v1;
    EXPECT_EQ(v1, 42);
    EXPECT_EQ(ms.Tell(), 4);

    Symbol sym;
    ms >> sym;
    EXPECT_STREQ(sym.Str(), "test");
    EXPECT_EQ(ms.Tell(), 12);

    int v2;
    ms >> v2;
    EXPECT_EQ(v2, 99);
    EXPECT_EQ(ms.Tell(), 16);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(BinStreamEndian, ReadPastEof) {
    std::vector<uint8_t> buf;
    PutBE32(buf, 0x11111111);
    // Only 4 bytes available

    MemBinStream ms(buf.data(), buf.size(), false);
    int v1, v2;
    ms >> v1;
    EXPECT_EQ(v1, 0x11111111);
    EXPECT_FALSE(ms.Fail());

    // This should trigger EOF/fail
    ms >> v2;
    EXPECT_TRUE(ms.Fail());
}

TEST(BinStreamEndian, ReadBoolChar) {
    std::vector<uint8_t> buf;
    buf.push_back(1);    // bool = true
    buf.push_back(0);    // bool = false
    buf.push_back('A');  // char

    MemBinStream ms(buf.data(), buf.size(), false);

    bool b1, b2;
    char c;
    ms >> b1;
    EXPECT_TRUE(b1);
    ms >> b2;
    EXPECT_FALSE(b2);
    ms >> c;
    EXPECT_EQ(c, 'A');
    EXPECT_EQ(ms.Tell(), 3);
}

TEST(BinStreamEndian, ReadBE_UnsignedChar) {
    std::vector<uint8_t> buf;
    buf.push_back(0xFF);

    MemBinStream ms(buf.data(), buf.size(), false);
    unsigned char val;
    ms >> val;
    EXPECT_EQ(val, 0xFF);
}

// ============================================================================
// Write + read round-trip
// ============================================================================

TEST(BinStreamEndian, WriteReadRoundTrip_BE) {
    MemBinStream writer(/*littleEndian=*/false);

    int intVal = 0xDEADBEEF;
    float floatVal = 1.5f;
    writer << intVal;
    writer << floatVal;

    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    int readInt;
    float readFloat;
    reader >> readInt;
    reader >> readFloat;
    EXPECT_EQ(readInt, (int)0xDEADBEEF);
    EXPECT_FLOAT_EQ(readFloat, 1.5f);
}

TEST(BinStreamEndian, WriteReadRoundTrip_LE) {
    MemBinStream writer(/*littleEndian=*/true);

    int intVal = 0x12345678;
    writer << intVal;

    MemBinStream reader(writer.Buffer(), writer.Size(), true);
    int readInt;
    reader >> readInt;
    EXPECT_EQ(readInt, 0x12345678);
}

// ============================================================================
// SwapData correctness
// ============================================================================

extern void SwapData(const void *in, void *out, int size);

TEST(BinStreamEndian, SwapData_2Bytes) {
    unsigned short in = 0xABCD;
    unsigned short out;
    SwapData(&in, &out, 2);
    EXPECT_EQ(out, 0xCDAB);
}

TEST(BinStreamEndian, SwapData_4Bytes) {
    unsigned int in = 0x12345678;
    unsigned int out;
    SwapData(&in, &out, 4);
    EXPECT_EQ(out, 0x78563412u);
}

TEST(BinStreamEndian, SwapData_8Bytes) {
    unsigned long long in = 0x0102030405060708ULL;
    unsigned long long out;
    SwapData(&in, &out, 8);
    EXPECT_EQ(out, 0x0807060504030201ULL);
}

// ============================================================================
// Seek behavior
// ============================================================================

TEST(BinStreamEndian, SeekBeginCurEnd) {
    std::vector<uint8_t> buf;
    PutBE32(buf, 0xAAAAAAAA);
    PutBE32(buf, 0xBBBBBBBB);
    PutBE32(buf, 0xCCCCCCCC);

    MemBinStream ms(buf.data(), buf.size(), false);

    // Read first int
    int val;
    ms >> val;
    EXPECT_EQ(val, (int)0xAAAAAAAA);
    EXPECT_EQ(ms.Tell(), 4);

    // Seek to beginning, re-read
    ms.Seek(0, BinStream::kSeekBegin);
    ms >> val;
    EXPECT_EQ(val, (int)0xAAAAAAAA);

    // Seek relative +4, should skip to third int
    ms.Seek(4, BinStream::kSeekCur);
    ms >> val;
    EXPECT_EQ(val, (int)0xCCCCCCCC);

    // Seek from end -4, should read last int
    ms.Seek(-4, BinStream::kSeekEnd);
    ms >> val;
    EXPECT_EQ(val, (int)0xCCCCCCCC);
}

// ============================================================================
// Vector deserialization
// ============================================================================

TEST(BinStreamEndian, ReadBE_VectorOfInts) {
    std::vector<uint8_t> buf;
    PutBE32(buf, 3);           // count = 3
    PutBE32(buf, 10);          // [0] = 10
    PutBE32(buf, 20);          // [1] = 20
    PutBE32(buf, 30);          // [2] = 30

    MemBinStream ms(buf.data(), buf.size(), false);
    std::vector<int> vec;
    ms >> vec;
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 10);
    EXPECT_EQ(vec[1], 20);
    EXPECT_EQ(vec[2], 30);
    EXPECT_EQ(ms.Tell(), 16);
}
