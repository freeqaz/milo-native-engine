// Silence the output while the game window is in the background and the main
// loop is starved.
//
// The mixer runs on the audio thread, but what it mixes is decoded by the main
// thread a frame at a time. A compositor that stops sending frame callbacks to
// a window nobody can see (Xwayland, and Wayland itself, drop a hidden window
// to about one present a second) therefore starves the streams: each second
// plays a burst of whatever was decoded and then nothing, which is heard as
// the music skipping. There is no portable "is this window visible" query, so
// the rule is built from the two things that can be observed:
//
//   mute    the window does not have focus AND no frame has finished for
//           kStallMs. Decided on the audio thread, so it takes effect while
//           the main loop is still stuck in the long frame.
//   resume  the window has focus again, or kResumeFrames frames in a row have
//           each taken less than kFastFrameMs (visible again, focus elsewhere).
//
// A focused window never mutes, so a long frame while loading does not dip
// the loading music; neither does an unfocused window that is still drawing at
// full rate (the game beside a terminal). With no window (headless) OnFrame is
// never called and nothing mutes.
//
// One writer per field: OnFrame is the main thread's, ShouldMute the audio
// thread's. They meet only in the atomics.

#pragma once

#include <atomic>
#include <cstdint>

class BackgroundMute {
public:
    static constexpr int64_t kStallMs = 500;
    static constexpr int64_t kFastFrameMs = 100;
    static constexpr int kResumeFrames = 30;

    void SetEnabled(bool enabled) { mEnabled.store(enabled, std::memory_order_relaxed); }

    // Main thread, once per frame of a windowed game.
    void OnFrame(int64_t nowMs, bool windowFocused) {
        const int64_t last = mLastFrameMs.exchange(nowMs, std::memory_order_relaxed);
        mFocused.store(windowFocused, std::memory_order_relaxed);
        if (!mMuted.load(std::memory_order_relaxed)) {
            mFastFrames = 0;
            return;
        }
        const bool fast = last != kNever && nowMs - last < kFastFrameMs;
        mFastFrames = fast ? mFastFrames + 1 : 0;
        if (windowFocused || mFastFrames >= kResumeFrames) {
            mFastFrames = 0;
            mMuted.store(false, std::memory_order_relaxed);
        }
    }

    // Audio thread, once per callback.
    bool ShouldMute(int64_t nowMs) {
        if (!mEnabled.load(std::memory_order_relaxed)) return false;
        if (mMuted.load(std::memory_order_relaxed)) return true;
        const int64_t last = mLastFrameMs.load(std::memory_order_relaxed);
        if (last == kNever) return false;
        if (mFocused.load(std::memory_order_relaxed)) return false;
        if (nowMs - last <= kStallMs) return false;
        mMuted.store(true, std::memory_order_relaxed);
        return true;
    }

    bool IsMuted() const {
        return mEnabled.load(std::memory_order_relaxed) && mMuted.load(std::memory_order_relaxed);
    }

private:
    static constexpr int64_t kNever = INT64_MIN;

    std::atomic<bool> mEnabled{true};
    std::atomic<int64_t> mLastFrameMs{kNever};
    std::atomic<bool> mFocused{true};
    std::atomic<bool> mMuted{false};
    int mFastFrames = 0; // main thread only
};
