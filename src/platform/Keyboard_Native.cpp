// DC3 Native Port - Keyboard via GLFW key callbacks
// Replaces Keyboard_Stub.cpp

#include "os/Keyboard.h"
#include "os/Debug.h"

#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>

// Set by Rnd_Wgpu during Init()
extern GLFWwindow *gNativeWindow;

// Ring buffer for key events from GLFW callback
struct KeyEvent {
    int key;
    int action; // GLFW_PRESS, GLFW_RELEASE
    int mods;
};

static const int kMaxKeyEvents = 64;
static KeyEvent sKeyQueue[kMaxKeyEvents];
static int sKeyQueueHead = 0;
static int sKeyQueueTail = 0;

static void PushKeyEvent(int key, int action, int mods) {
    int next = (sKeyQueueHead + 1) % kMaxKeyEvents;
    if (next == sKeyQueueTail)
        return; // queue full, drop event
    sKeyQueue[sKeyQueueHead] = {key, action, mods};
    sKeyQueueHead = next;
}

static bool PopKeyEvent(KeyEvent &out) {
    if (sKeyQueueTail == sKeyQueueHead)
        return false;
    out = sKeyQueue[sKeyQueueTail];
    sKeyQueueTail = (sKeyQueueTail + 1) % kMaxKeyEvents;
    return true;
}

static void GlfwKeyCallback(GLFWwindow *, int key, int /*scancode*/, int action, int mods) {
    if (action == GLFW_PRESS || action == GLFW_RELEASE) {
        PushKeyEvent(key, action, mods);
    }
}
#endif // !__EMSCRIPTEN__

void KeyboardInit() {
    MILO_LOG("[Native] KeyboardInit\n");
    KeyboardInitCommon();
#ifndef __EMSCRIPTEN__
    if (gNativeWindow) {
        glfwSetKeyCallback(gNativeWindow, GlfwKeyCallback);
    }
#endif
}

void KeyboardTerminate() {
#ifndef __EMSCRIPTEN__
    if (gNativeWindow) {
        glfwSetKeyCallback(gNativeWindow, nullptr);
    }
#endif
    KeyboardTerminateCommon();
}

void KeyboardPoll() {
#ifndef __EMSCRIPTEN__
    KeyEvent ev;
    while (PopKeyEvent(ev)) {
        if (ev.action == GLFW_PRESS) {
            bool shift = (ev.mods & GLFW_MOD_SHIFT) != 0;
            bool ctrl  = (ev.mods & GLFW_MOD_CONTROL) != 0;
            bool alt   = (ev.mods & GLFW_MOD_ALT) != 0;
            KeyboardSendMsg(ev.key, shift, ctrl, alt);
        }
        // KeyboardKeyReleaseMsg has no payload in the engine — skip for now
    }
#endif
}
