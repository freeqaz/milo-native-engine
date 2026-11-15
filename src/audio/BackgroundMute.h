// Silence the output while the game window is in the background and the main
// loop is starved.
//
// The mixer runs on the audio thread, but what it mixes is decoded by the main
// thread a frame at a time. A compositor that stops sending frame callbacks to
// a window nobody can see (Xwayland, and Wayland itself, drop a hidden window
// to about one present a second) therefore starves the streams: each second
// plays a burst of whatever was decoded and then nothing, which is heard as
// the music skipping. There is no portable "is this window visible" query, so
// the rule is built from what can be observed:
//
//   mute    no frame has finished for kStallMs AND the window does not have
//           focus; or the main thread has been inside one wait on the
//           compositor (acquiring or presenting the window's frame) for
//           kStallMs, focus or not. Decided on the audio thread, so it takes
//           effect while the main loop is still stuck in the long frame.
//   resume  a frame that did not wait on the compositor (less than
//           kStarvedWaitMs in all) arrives while the window has focus, or
//           kResumeFrames such frames in a row have each taken less than
//           kFastFrameMs (visible again, focus elsewhere).
//
// The wait on the compositor is the second signal because focus alone misses
// a case: on Hyprland, switching to an empty workspace leaves the hidden
// window focused (GPD, 2026-11-15: 1 frame a second, focus still reported,
// no mute). A frame that waited kStarvedWaitMs on the compositor never
// resumes, or a focused hidden window would play half of every second.
//
// A focused window whose frame is long in the game's own code never mutes, so
// a long frame while loading does not dip the loading music; neither does an
// unfocused window that is still drawing at full rate (the game beside a
// terminal). With no window (headless) OnFrame is never called and nothing
// mutes.
//
// One writer per field: OnFrame and the compositor-wait pair are the main
// thread's, ShouldMute the audio thread's. They meet only in the atomics.

#pragma once

#include <atomic>
#include <cstdint>

class BackgroundMute {
public:
    static constexpr int64_t kStallMs = 500;
    static constexpr int64_t kFastFrameMs = 100;
    static constexpr int kResumeFrames = 30;
    static constexpr int64_t kStarvedWaitMs = 250;

    void SetEnabled(bool enabled) { mEnabled.store(enabled, std::memory_order_relaxed); }

    // Main thread, around each call that can block on the compositor
    // (acquiring the window's frame, presenting it).
    void BeginCompositorWait(int64_t nowMs) {
        mWaitStartMs.store(nowMs, std::memory_order_relaxed);
    }
    void EndCompositorWait(int64_t nowMs) {
        const int64_t start = mWaitStartMs.exchange(kNever, std::memory_order_relaxed);
        if (start != kNever && nowMs > start) mFrameWaitMs += nowMs - start;
    }

    // Main thread, once per frame of a windowed game.
    void OnFrame(int64_t nowMs, bool windowFocused) {
        const int64_t last = mLastFrameMs.exchange(nowMs, std::memory_order_relaxed);
        mFocused.store(windowFocused, std::memory_order_relaxed);
        const bool starved = mFrameWaitMs >= kStarvedWaitMs;
        mFrameWaitMs = 0;
        if (!mMuted.load(std::memory_order_relaxed)) {
            mFastFrames = 0;
            return;
        }
        if (starved) {
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
        const int64_t wait = mWaitStartMs.load(std::memory_order_relaxed);
        if (wait != kNever && nowMs - wait > kStallMs) {
            mMuted.store(true, std::memory_order_relaxed);
            return true;
        }
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
    std::atomic<int64_t> mWaitStartMs{kNever}; // kNever: not waiting on the compositor
    int mFastFrames = 0; // main thread only
    int64_t mFrameWaitMs = 0; // main thread only: compositor wait in this frame
};
