// DC3 Native Port — Video Encoder
// Pipes raw RGBA frames to ffmpeg for video output.

#pragma once

#include <cstdint>
#include <cstdio>

class VideoEncoder {
public:
    VideoEncoder() : mPipe(nullptr), mWidth(0), mHeight(0), mFrameCount(0) {}
    ~VideoEncoder() { Finish(); }

    // Start encoding to output file. Returns true on success.
    bool Start(const char* path, int w, int h, int fps = 30);

    // Write one RGBA frame. size must be w*h*4 bytes.
    bool WriteFrame(const uint8_t* rgba, size_t size);

    // Finish encoding and close the pipe. Returns true on success.
    bool Finish();

    int FrameCount() const { return mFrameCount; }
    int Width() const { return mWidth; }
    int Height() const { return mHeight; }

private:
    FILE* mPipe;
    int mWidth;
    int mHeight;
    int mFrameCount;
};
