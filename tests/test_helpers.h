#pragma once

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <cstdint>

// ============================================================================
// MemBinStream — in-memory BinStream for unit testing without files
// ============================================================================
// Wraps a byte buffer and implements ReadImpl/WriteImpl/SeekImpl directly.
// Unlike MemStream (which lives in the engine), this is standalone and
// doesn't depend on engine initialization.

#include "utl/BinStream.h"

class MemBinStream : public BinStream {
public:
    // Construct from existing data (for reading). littleEndian controls endian swap.
    MemBinStream(const void *data, int size, bool littleEndian = false)
        : BinStream(littleEndian), mTell(0), mFail(false) {
        mBuffer.resize(size);
        if (size > 0)
            memcpy(mBuffer.data(), data, size);
    }

    // Construct empty (for writing)
    explicit MemBinStream(bool littleEndian = false)
        : BinStream(littleEndian), mTell(0), mFail(false) {
        mBuffer.reserve(4096);
    }

    virtual ~MemBinStream() {}
    virtual void Flush() {}
    virtual int Tell() { return mTell; }
    virtual EofType Eof() {
        return (mTell >= (int)mBuffer.size()) ? RealEof : NotEof;
    }
    virtual bool Fail() { return mFail; }

    int Size() const { return (int)mBuffer.size(); }
    const char *Buffer() const { return mBuffer.data(); }

private:
    virtual void ReadImpl(void *data, int bytes) {
        if (mTell + bytes > (int)mBuffer.size()) {
            mFail = true;
            int avail = (int)mBuffer.size() - mTell;
            if (avail > 0) {
                memcpy(data, mBuffer.data() + mTell, avail);
                memset((char *)data + avail, 0, bytes - avail);
            } else {
                memset(data, 0, bytes);
            }
            mTell = (int)mBuffer.size();
            return;
        }
        memcpy(data, mBuffer.data() + mTell, bytes);
        mTell += bytes;
    }

    virtual void WriteImpl(const void *data, int bytes) {
        if (mTell + bytes > (int)mBuffer.size())
            mBuffer.resize(mTell + bytes);
        memcpy(mBuffer.data() + mTell, data, bytes);
        mTell += bytes;
    }

    virtual void SeekImpl(int offset, SeekType type) {
        int pos;
        switch (type) {
        case kSeekBegin: pos = offset; break;
        case kSeekCur: pos = mTell + offset; break;
        case kSeekEnd: pos = (int)mBuffer.size() + offset; break;
        default: return;
        }
        if (pos < 0 || pos > (int)mBuffer.size()) {
            mFail = true;
        } else {
            mTell = pos;
        }
    }

    int mTell;
    bool mFail;
    std::vector<char> mBuffer;
};

// ============================================================================
// Byte buffer helpers — build synthetic binary test data
// ============================================================================

// Write a 32-bit big-endian value into a buffer
inline void PutBE32(std::vector<uint8_t> &buf, uint32_t val) {
    buf.push_back((val >> 24) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

// Write a 32-bit little-endian value into a buffer
inline void PutLE32(std::vector<uint8_t> &buf, uint32_t val) {
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}

// Write a 16-bit big-endian value into a buffer
inline void PutBE16(std::vector<uint8_t> &buf, uint16_t val) {
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

// Write a 16-bit little-endian value into a buffer
inline void PutLE16(std::vector<uint8_t> &buf, uint16_t val) {
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
}

// Write a length-prefixed string in big-endian format (length as BE32 + raw chars)
inline void PutBEString(std::vector<uint8_t> &buf, const char *str) {
    uint32_t len = (uint32_t)strlen(str);
    PutBE32(buf, len);
    for (uint32_t i = 0; i < len; i++)
        buf.push_back((uint8_t)str[i]);
}

// Write a length-prefixed string in little-endian format
inline void PutLEString(std::vector<uint8_t> &buf, const char *str) {
    uint32_t len = (uint32_t)strlen(str);
    PutLE32(buf, len);
    for (uint32_t i = 0; i < len; i++)
        buf.push_back((uint8_t)str[i]);
}

// Write the dead marker bytes (0xADDEADDE)
inline void PutDeadMarker(std::vector<uint8_t> &buf) {
    buf.push_back(0xAD);
    buf.push_back(0xDE);
    buf.push_back(0xAD);
    buf.push_back(0xDE);
}

// Write a big-endian float
inline void PutBEFloat(std::vector<uint8_t> &buf, float val) {
    uint32_t bits;
    memcpy(&bits, &val, 4);
    PutBE32(buf, bits);
}

// Write a big-endian double
inline void PutBEDouble(std::vector<uint8_t> &buf, double val) {
    uint64_t bits;
    memcpy(&bits, &val, 8);
    // Big-endian: MSB first
    for (int i = 7; i >= 0; i--)
        buf.push_back((bits >> (i * 8)) & 0xFF);
}

// Fill with N garbage bytes
inline void PutGarbage(std::vector<uint8_t> &buf, int n) {
    for (int i = 0; i < n; i++)
        buf.push_back((uint8_t)(i & 0xFF));
}

// ============================================================================
// Minimal init — just Symbol/StringTable (for pure unit tests using Symbol)
// ============================================================================
void EnsureSymbolInit();

// GTest fixture for tests that need Symbol but not the full engine
class SymbolTestFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        EnsureSymbolInit();
    }
};

// ============================================================================
// Engine init fixture — headless engine initialization for integration tests
// ============================================================================
// Call EnsureEngineInit() once per test suite. Sets MILO_HEADLESS=1 and runs
// the same init sequence as milo-viewer.

void EnsureEngineInit();

// GTest fixture that calls EnsureEngineInit() in SetUpTestSuite
class EngineTestFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        EnsureEngineInit();
    }
};

// ============================================================================
// Bink test asset path
// ============================================================================
// Returns path to a .bik file for testing. Checks MILO_TEST_BIK env var first,
// then falls back to pre-extracted files from ExtractBik.ExtractSmallest.
// Returns nullptr if no .bik is available.
const char *GetTestBikPath();

// ============================================================================
// Synthetic milo file helpers
// ============================================================================

// Write a valid uncompressed .milo_xbox file to disk.
// chunks: vector of chunk data buffers. Each becomes one chunk.
// Returns true on success.
bool WriteSyntheticMilo(const char *path,
                        const std::vector<std::vector<uint8_t>> &chunks);
