// DC3 Native Port - RndTex & RndBitmap loading
// Replaces engine_stubs_generated.cpp stubs for RndTex::Load/PreLoad/PostLoad
// and RndBitmap::Load. These must consume the correct bytes from the stream
// to keep it aligned for subsequent object loading in .milo files.

#include "rndobj/Tex.h"
#include "rndobj/Bitmap.h"
#include "utl/BinStream.h"
#include "utl/ChunkStream.h"

// Forward declaration - defined in ChunkStream.cpp, no header declaration
BinStream &ReadChunks(BinStream &bs, void *data, int total_len, int max_chunk_size);

// --- RndBitmap::Load ---
// Reads bitmap header, palette, pixel data (via ReadChunks), and mip chain.
// Based on RB3 reference: rb3/src/system/rndobj/Bitmap.cpp:1018
void RndBitmap::Load(BinStream &bs) {
    u8 mipCt;
    LoadHeader(bs, mipCt);
    if (mBuffer) {
        MemFree(mBuffer);
        mBuffer = nullptr;
    }
    mPalette = nullptr;
    AllocateBuffer();
    if (mPalette)
        bs.Read(mPalette, PaletteBytes());
    ReadChunks(bs, mPixels, PixelBytes(), 0x8000);
    RELEASE(mMip);
    RndBitmap *workingMip = this;
    int working_w = mWidth;
    int working_h = mHeight;
    while (mipCt--) {
        RndBitmap *newMip = new RndBitmap();
        workingMip->mMip = newMip;
        workingMip = newMip;
        working_w = working_w >> 1;
        working_h = working_h >> 1;
        newMip->Create(working_w, working_h, 0, mBpp, mOrder, mPalette, 0, 0);
        ReadChunks(bs, newMip->Pixels(), newMip->PixelBytes(), 0x8000);
    }
}

// --- RndTex::Load ---
void RndTex::Load(BinStream &bs) {
    PreLoad(bs);
    PostLoad(bs);
}

// --- RndTex::PreLoad ---
// Reads revision, superclass, and texture properties.
// Pushes revision to rev stack for PostLoad to pop.
// Based on DC3 Save: SAVE_REVS(11, 0), SAVE_SUPERCLASS(Object),
//   bs << mWidth << mHeight << mBpp << mFilepath
void RndTex::PreLoad(BinStream &bs) {
    LOAD_REVS(bs);
#ifdef HX_NATIVE
    if (d.rev > 20) {
        printf("  RndTex::PreLoad '%s': BAD REVISION %d, stream desync!\n", Name(), d.rev);
        return;
    }
#endif
    LOAD_SUPERCLASS(Hmx::Object)
    d.stream >> mWidth >> mHeight >> mBpp >> mFilepath;
    bs.PushRev(packRevs(d.altRev, d.rev), this);
}

// --- RndTex::PostLoad ---
// Reads remaining properties and bitmap data.
// Based on DC3 Save: bs << mMipMapK << mType << (bool)mNumMips << mOptimizeForPS3;
//   if (bs.Cached()) mBitmap.Save(bs);
void RndTex::PostLoad(BinStream &bs) {
    int revs = bs.PopRev(this);
    unsigned short rev = getHmxRev(revs);

    bs >> mMipMapK;
    bs >> (int &)mType;
    bool numMips;
    bs >> numMips;
    mNumMips = numMips;
    bs >> mOptimizeForPS3;

    if (bs.Cached()) {
        mBitmap.Load(bs);
    }
}
