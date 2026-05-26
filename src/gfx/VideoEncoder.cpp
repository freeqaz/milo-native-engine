// DC3 Native Port — Video Encoder
// Pipes raw RGBA frames to ffmpeg for video output.

#include "gfx/VideoEncoder.h"
#include <cstdio>
#include <cstring>

bool VideoEncoder::Start(const char* path, int w, int h, int fps) {
    if (mPipe) {
        fprintf(stderr, "VideoEncoder: already started\n");
        return false;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d "
        "-i pipe:0 -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p '%s' "
        "2>/dev/null",
        w, h, fps, path);

    mPipe = popen(cmd, "w");
    if (!mPipe) {
        fprintf(stderr, "VideoEncoder: failed to start ffmpeg — is ffmpeg installed?\n");
        return false;
    }

    mWidth = w;
    mHeight = h;
    mFrameCount = 0;
    printf("VideoEncoder: encoding %dx%d @ %d fps → %s\n", w, h, fps, path);
    return true;
}

bool VideoEncoder::WriteFrame(const uint8_t* rgba, size_t size) {
    if (!mPipe) return false;

    size_t expected = (size_t)mWidth * mHeight * 4;
    if (size != expected) {
        fprintf(stderr, "VideoEncoder: frame size mismatch (%zu != %zu)\n", size, expected);
        return false;
    }

    size_t written = fwrite(rgba, 1, size, mPipe);
    if (written != size) {
        fprintf(stderr, "VideoEncoder: write failed (%zu / %zu bytes)\n", written, size);
        return false;
    }

    mFrameCount++;
    return true;
}

bool VideoEncoder::Finish() {
    if (!mPipe) return true;

    int status = pclose(mPipe);
    mPipe = nullptr;

    if (status != 0) {
        fprintf(stderr, "VideoEncoder: ffmpeg exited with status %d\n", status);
        return false;
    }

    printf("VideoEncoder: finished — %d frames encoded\n", mFrameCount);
    return true;
}
