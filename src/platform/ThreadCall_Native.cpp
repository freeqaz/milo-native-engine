// milo-native-engine - ThreadCall (POSIX)
// SDK-agnostic, matched-fork-symbol-preserving implementation of the
// ThreadCall.h surface declared by the consuming decomp. Uses pthread + sem_t
// directly — no Win32 type names (no HANDLE / DWORD / WaitForSingleObject), no
// xdk/ includes.
//
// Maps the Xbox ThreadCall_Win.cpp semantics:
//   - one worker thread, started in ThreadCallInit
//   - a count==1 binary semaphore signals "data available in gData[gCurCall]"
//     (ReleaseSemaphore -> sem_post, WaitForSingleObject(INFINITE) -> sem_wait)
//   - work entries are queued in a fixed 12-slot ring (gData), filled by
//     ThreadCall(), drained by the worker, completion posted via gCallDone
//   - ThreadCallPoll() advances the ring on the main thread
//
// Emscripten (single-threaded) keeps the prior synchronous-poll fallback.

#include "os/ThreadCall.h"
#include "os/Debug.h"

#include <cstring>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#include <semaphore.h>
#include <cstdlib>
#include <ctime>

namespace {
// W0.3d-b (Wave 12, A-S2): TEST-ONLY worker-latency jitter for the load-
// determinism fail-red. Env-gated (RB3_LOADDET_JITTER), default-OFF: a normal
// build never sleeps (getenv parsed once; 0/unset => no-op). When set to a
// positive integer N it sleeps a pseudo-random 0..N microseconds on the worker
// thread before each job dispatch, amplifying the worker<->main allocation-order
// race A-S1 traced so the OFF-arm gRand spread reproduces reliably under
// contention (and the seam-ON arm must still collapse). Worker-thread only; the
// main thread is untouched, so this only perturbs completion timing.
int gJitterUs = -1;  // -1 unchecked; 0 = off; >0 = max microseconds
unsigned int gJitterState = 0x1234567u;
inline void LoadDetWorkerJitter() {
    if (gJitterUs < 0) {
        const char *v = std::getenv("RB3_LOADDET_JITTER");
        long n = (v && *v) ? std::strtol(v, nullptr, 10) : 0;
        gJitterUs = (n > 0) ? (int)n : 0;
    }
    if (gJitterUs == 0)
        return;
    gJitterState = gJitterState * 1103515245u + 12345u;
    long us = (long)((gJitterState >> 16) % (unsigned)gJitterUs);
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = us * 1000L;
    nanosleep(&ts, nullptr);
}

// W0.3d-b (Wave 12, A-S2): the H-RESEED seam's COMPLEMENTARY root lever. A-S1
// traced the boot-to-boot gRand divergence to the worker<->main glibc-arena
// allocation RACE: the ThreadCall worker parses milo DTA on its own arena
// concurrently with main-thread allocs, so object addresses shuffle per boot ->
// the unsorted address-ordered mAnims walk visits variable-count rejection
// samplers in a different ORDER -> per-frame gRand COUNT diverges. Reseeding at
// the anchor resets the TABLE but NOT that order, so the post-anchor COUNT still
// diverges (A-S2 measured ON deltaSpread != 0). Removing the race AT SOURCE —
// draining ThreadCall jobs synchronously on the main thread (the proven
// __EMSCRIPTEN__ path) — makes the allocation order deterministic so the
// post-anchor stream actually collapses. Scoped to RB3_FIXED_CLOCK &&
// RB3_LOAD_DETERMINISM (same opt-in as the reseed), parsed once; default-OFF, so
// a normal build keeps the async worker (byte-identical).
int gSerialize = -1;  // -1 unchecked; 0 = worker (default); 1 = synchronous drain
inline bool LoadDetSerialize() {
    if (gSerialize < 0) {
        const char *fc = std::getenv("RB3_FIXED_CLOCK");
        const char *ld = std::getenv("RB3_LOAD_DETERMINISM");
        bool fcOn = fc && *fc && std::strcmp(fc, "0") != 0;
        bool ldOn = ld && *ld && std::strcmp(ld, "0") != 0;
        gSerialize = (fcOn && ldOn) ? 1 : 0;
    }
    return gSerialize == 1;
}
}  // namespace
#endif

namespace {
    bool gReadyForNext = true;
    ThreadCallData gData[12];
    bool gTerminate;
    bool gCallDone;
    int gCurCall;
    int gFreeCall;

#ifndef __EMSCRIPTEN__
    pthread_t gWorker;
    bool gWorkerStarted;
    sem_t gWorkerSem;

    void *WorkerMain(void *) {
        // First wait — entry: matches the Xbox flow where the worker blocks
        // immediately on the semaphore until the main thread posts work.
        sem_wait(&gWorkerSem);
        while (!gTerminate) {
            LoadDetWorkerJitter();  // W0.3d-b: env-gated fail-red jitter (default no-op)
            switch (gData[gCurCall].mType) {
            case kTCDT_Func:
                gData[gCurCall].mArg = gData[gCurCall].mFunc();
                gCallDone = true;
                break;
            case kTCDT_Class:
                gData[gCurCall].mArg = gData[gCurCall].mClass->ThreadStart();
                gCallDone = true;
                break;
            default:
                MILO_ASSERT(false, 199);
                break;
            }
            sem_wait(&gWorkerSem);
        }
        sem_destroy(&gWorkerSem);
        return nullptr;
    }
#endif
}

u32 gMainThreadID = (u32)-1;

void ThreadCallInit() {
    memset(gData, 0, sizeof(gData));
    gCurCall = 0;
    gFreeCall = 0;
#ifdef __EMSCRIPTEN__
    // Single-threaded WASM: no worker thread. Work runs synchronously in
    // ThreadCallPoll() below.
#else
    if (sem_init(&gWorkerSem, 0, 0) != 0) {
        MILO_LOG("sem_init() failed.\n");
        gWorkerStarted = false;
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);
    int rc = pthread_create(&gWorker, &attr, WorkerMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        MILO_LOG("pthread_create() failed.\n");
        sem_destroy(&gWorkerSem);
        gWorkerStarted = false;
    } else {
        gWorkerStarted = true;
    }
#endif
}

void ThreadCall(ThreadCallFunc *func, ThreadCallCallbackFunc *callback) {
    ThreadCallData &data = gData[gFreeCall];
    MILO_ASSERT(data.mType == kTCDT_None, 0x6E);
    data.mFunc = func;
    data.mCallback = callback;
    data.mType = kTCDT_Func;
    data.mClass = nullptr;
    gFreeCall = (gFreeCall + 1) % 12;
}

void ThreadCall(ThreadCallback *callback) {
    ThreadCallData &data = gData[gFreeCall];
    MILO_ASSERT(data.mType == kTCDT_None, 0x7B);
    data.mType = kTCDT_Class;
    data.mFunc = nullptr;
    data.mCallback = nullptr;
    data.mClass = callback;
    gFreeCall = (gFreeCall + 1) % 12;
}

void ThreadCallPoll() {
#ifdef __EMSCRIPTEN__
    // Single-threaded WASM: run one pending job synchronously per poll
    ThreadCallData &data = gData[gCurCall];
    if (data.mType != kTCDT_None) {
        int result = 0;
        ThreadCallDataType type = data.mType;
        switch (type) {
        case kTCDT_Func:
            result = data.mFunc();
            data.mType = kTCDT_None;
            gCurCall = (gCurCall + 1) % 12;
            data.mCallback(result);
            break;
        case kTCDT_Class: {
            ThreadCallback *cls = data.mClass;
            result = cls->ThreadStart();
            data.mType = kTCDT_None;
            gCurCall = (gCurCall + 1) % 12;
            cls->ThreadDone(result);
            break;
        }
        default:
            break;
        }
    }
#else
    // W0.3d-b (Wave 12, A-S2): determinism seam's synchronous-drain lever. When
    // RB3_FIXED_CLOCK && RB3_LOAD_DETERMINISM, run the pending job inline on the
    // main thread (no worker sem_post) so the worker<->main alloc race that
    // shuffles object addresses — and thus the mAnims consumer order feeding the
    // per-frame rejection samplers — is removed at source. Mirrors the proven
    // __EMSCRIPTEN__ single-threaded drain above. Default-OFF: the async worker
    // path below is byte-identical when the flag is off.
    if (LoadDetSerialize()) {
        ThreadCallData &data = gData[gCurCall];
        if (data.mType != kTCDT_None) {
            LoadDetWorkerJitter();  // keep the fail-red jitter meaningful here too
            ThreadCallDataType type = data.mType;
            int result = 0;
            switch (type) {
            case kTCDT_Func:
                result = data.mFunc();
                data.mType = kTCDT_None;
                gCurCall = (gCurCall + 1) % 12;
                data.mCallback(result);
                break;
            case kTCDT_Class: {
                ThreadCallback *cls = data.mClass;
                result = cls->ThreadStart();
                data.mType = kTCDT_None;
                gCurCall = (gCurCall + 1) % 12;
                cls->ThreadDone(result);
                break;
            }
            default:
                break;
            }
        }
        return;
    }
    if (gCallDone) {
        ThreadCallData &data = gData[gCurCall];
        ThreadCallDataType oldType = data.mType;
        if (data.mType) {
            data.mType = kTCDT_None;
            gCallDone = false;
            gCurCall = (gCurCall + 1) % 12;
            gReadyForNext = true;
            switch (oldType) {
            case kTCDT_None:
                MILO_ASSERT(false, 0x97);
                break;
            case kTCDT_Func:
                data.mCallback(data.mArg);
                break;
            case kTCDT_Class:
                data.mClass->ThreadDone(data.mArg);
                break;
            }
        }
    }
    if (gReadyForNext && gData[gCurCall].mType != kTCDT_None) {
        gReadyForNext = false;
        if (gWorkerStarted) {
            sem_post(&gWorkerSem);
        }
    }
#endif
}

void ThreadCallPreInit() {
#ifndef __EMSCRIPTEN__
    gMainThreadID = (u32)(uintptr_t)pthread_self();
#else
    // Single-threaded web build: everything runs on the one (main) thread, so
    // thread-affinity checks are meaningless. We can't match GetCurrentThreadId()
    // here (it lives in the consumer's xdk shim as pthread_self(), and this TU is
    // SDK-agnostic with no pthread.h on Emscripten), and a hardcoded 0 never
    // equalled that non-zero pthread_self() value — making MainThread() always
    // false and flooding RandomFloat/RandomInt's MILO_ASSERT(MainThread()) so the
    // engine never finished booting. -1 is the documented "thread checks disabled"
    // sentinel (see the consumer's os/OSFuncs.h MainThread()).
    gMainThreadID = (u32)-1;
#endif
}

void ThreadCallTerminate() {
#ifndef __EMSCRIPTEN__
    if (gWorkerStarted) {
        gTerminate = true;
        sem_post(&gWorkerSem);
    }
#endif
}
