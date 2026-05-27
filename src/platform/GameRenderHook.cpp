// GameRenderHook — engine-side storage for the registered game render hook.
//
// File-scope static pointer, default null. See GameRenderHook.h for the
// rationale and the contract.

#include "platform/GameRenderHook.h"

namespace {
GameRenderHook* gGameRenderHook = nullptr;
}

void SetGameRenderHook(GameRenderHook* hook) {
    gGameRenderHook = hook;
}

GameRenderHook* GetGameRenderHook() {
    return gGameRenderHook;
}
