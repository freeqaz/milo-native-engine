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
    gMainThreadID = 0;
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
