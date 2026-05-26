// DC3 Native Port - Joypad via GLFW gamepad + keyboard fallback
// Replaces Joypad_Stub.cpp
//
// Headless input: set MILO_INPUT_SCRIPT to a text file with scripted actions.
// Supports absolute frames, wait_screen directives, and relative offsets.
// See scripts/dc3-input-flows/README.txt for full format docs.

#include "os/Joypad.h"
#include "os/JoypadMsgs.h"
#include "os/Debug.h"
#include "os/System.h"
#include "rndobj/Rnd.h"
#include "ui/UI.h"
#ifdef DC3_HTTP_SERVER
#include "platform/HttpServer.h"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#else
#include <GLFW/glfw3.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#ifndef __EMSCRIPTEN__
// Set by Rnd_Wgpu during Init()
extern GLFWwindow *gNativeWindow;
#endif

// ============================================================================
// Web keyboard state (JS keydown/keyup → shared bitmask read from C)
// ============================================================================

#ifdef __EMSCRIPTEN__
#include <emscripten/em_asm.h>

static bool sWebInputInitialized = false;

// Read the current button bitmask from JS (set by keydown/keyup listeners)
static unsigned int GetWebKeyButtons() {
    return (unsigned int)EM_ASM_INT({ return window._dc3Keys || 0; });
}

// Clear stale key state — call during screen transitions so held keys
// from menu navigation don't bleed into gameplay (e.g., Space/Start
// triggering an immediate pause on game_screen).
void JoypadWebClearKeys() {
    EM_ASM({ window._dc3Keys = 0; });
}

// Poll the Gamepad API — returns bitmask matching JoypadButton enum.
// navigator.getGamepads() returns a snapshot; we read pad 0.
static unsigned int GetWebGamepadButtons() {
    return (unsigned int)EM_ASM_INT({
        var gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
        var gp = gamepads[0];
        if (!gp || !gp.connected) return 0;
        var b = 0;
        var btn = gp.buttons;
        // Standard gamepad mapping (same as GLFW layout)
        if (btn[0] && btn[0].pressed)  b |= (1 << 6);  // A -> kPad_X
        if (btn[1] && btn[1].pressed)  b |= (1 << 5);  // B -> kPad_Circle
        if (btn[2] && btn[2].pressed)  b |= (1 << 7);  // X -> kPad_Square
        if (btn[3] && btn[3].pressed)  b |= (1 << 4);  // Y -> kPad_Tri
        if (btn[4] && btn[4].pressed)  b |= (1 << 2);  // LB -> kPad_L1
        if (btn[5] && btn[5].pressed)  b |= (1 << 3);  // RB -> kPad_R1
        if (btn[6] && btn[6].value > 0.3)  b |= (1 << 0);  // LT -> kPad_L2
        if (btn[7] && btn[7].value > 0.3)  b |= (1 << 1);  // RT -> kPad_R2
        if (btn[8] && btn[8].pressed)  b |= (1 << 8);  // Back -> kPad_Select
        if (btn[9] && btn[9].pressed)  b |= (1 << 11); // Start -> kPad_Start
        if (btn[10] && btn[10].pressed) b |= (1 << 9);  // L3 -> kPad_L3
        if (btn[11] && btn[11].pressed) b |= (1 << 10); // R3 -> kPad_R3
        if (btn[12] && btn[12].pressed) b |= (1 << 12); // DUp -> kPad_DUp
        if (btn[13] && btn[13].pressed) b |= (1 << 14); // DDown -> kPad_DDown
        if (btn[14] && btn[14].pressed) b |= (1 << 15); // DLeft -> kPad_DLeft
        if (btn[15] && btn[15].pressed) b |= (1 << 13); // DRight -> kPad_DRight
        return b;
    });
}

static void InitWebInput() {
    if (sWebInputInitialized) return;
    sWebInputInitialized = true;

    // Install JS keydown/keyup listeners that maintain a bitmask.
    // Bit positions match JoypadButton enum:
    //   L2=0 R2=1 L1=2 R1=3 Tri=4 Circle=5 X=6 Square=7
    //   Select=8 L3=9 R3=10 Start=11 DUp=12 DRight=13 DDown=14 DLeft=15
    // EM_ASM JS blocks cannot contain C-style comments or unescaped braces in
    // object literals, so we build the key map with bracket assignment.
    EM_ASM({
        window._dc3Keys = 0;
        var m = new Object();
        m['ArrowUp']    = 1<<12;
        m['ArrowDown']  = 1<<14;
        m['ArrowLeft']  = 1<<15;
        m['ArrowRight'] = 1<<13;
        m['w'] = 1<<12;
        m['W'] = 1<<12;
        m['s'] = 1<<14;
        m['S'] = 1<<14;
        m['a'] = 1<<15;
        m['A'] = 1<<15;
        m['d'] = 1<<13;
        m['D'] = 1<<13;
        m['Enter']     = 1<<6;
        m['Escape']    = 1<<5;
        m['Backspace'] = 1<<5;
        m[' ']         = 1<<11;
        m['Tab']       = 1<<8;
        m['q'] = 1<<2;
        m['Q'] = 1<<2;
        m['e'] = 1<<3;
        m['E'] = 1<<3;
        m['z'] = 1<<0;
        m['Z'] = 1<<0;
        m['c'] = 1<<1;
        m['C'] = 1<<1;
        m['x'] = 1<<7;
        m['X'] = 1<<7;
        m['y'] = 1<<4;
        m['Y'] = 1<<4;
        var consume = new Object();
        consume['ArrowUp'] = 1;
        consume['ArrowDown'] = 1;
        consume['ArrowLeft'] = 1;
        consume['ArrowRight'] = 1;
        consume[' '] = 1;
        consume['Tab'] = 1;
        consume['Escape'] = 1;
        consume['Backspace'] = 1;
        document.addEventListener('keydown', function(e) {
            var bit = m[e.key];
            if (bit) {
                window._dc3Keys |= bit;
                if (consume[e.key]) e.preventDefault();
            }
        }, true);
        document.addEventListener('keyup', function(e) {
            var bit = m[e.key];
            if (bit) {
                window._dc3Keys &= ~bit;
            }
        }, true);
        console.log('DC3 Web: keyboard input ready');
    });

    printf("DC3 Web: keyboard input initialized\n");
}
#endif // __EMSCRIPTEN__

static const float kTriggerThreshold = 0.3f;

// ============================================================================
// Headless scripted input
// ============================================================================

enum ScriptDirectiveType {
    kDirectiveButton,
    kDirectiveWaitScreen,
};

struct ScriptDirective {
    ScriptDirectiveType type;
    int frame;
    JoypadButton button;
    char screenName[64];
    bool relative;
};

static std::vector<ScriptDirective> gScript;
static bool gInputScriptLoaded = false;
static size_t gScriptCursor = 0;
static bool gWaiting = false;
static const char *gWaitTarget = nullptr;
static int gWaitSatisfiedFrame = -1;
static int gWaitStartFrame = -1;
static const int kWaitTimeoutFrames = 30 * 60; // 30 seconds at 60fps

static JoypadButton ParseButtonName(const char *name) {
    // Confirm / A
    if (!strcmp(name, "confirm") || !strcmp(name, "a"))     return kPad_X;
    // Cancel / B
    if (!strcmp(name, "cancel") || !strcmp(name, "b"))      return kPad_Circle;
    // Start
    if (!strcmp(name, "start"))                              return kPad_Start;
    // Option / Back / Select
    if (!strcmp(name, "option") || !strcmp(name, "back") ||
        !strcmp(name, "select"))                             return kPad_Select;
    // D-pad
    if (!strcmp(name, "up"))                                 return kPad_DUp;
    if (!strcmp(name, "down"))                               return kPad_DDown;
    if (!strcmp(name, "left"))                               return kPad_DLeft;
    if (!strcmp(name, "right"))                              return kPad_DRight;
    // Bumpers
    if (!strcmp(name, "l1") || !strcmp(name, "lb"))          return kPad_L1;
    if (!strcmp(name, "r1") || !strcmp(name, "rb"))          return kPad_R1;
    // Triggers
    if (!strcmp(name, "l2") || !strcmp(name, "lt"))          return kPad_L2;
    if (!strcmp(name, "r2") || !strcmp(name, "rt"))          return kPad_R2;
    // Face buttons by Xbox name
    if (!strcmp(name, "x"))                                  return kPad_Square;
    if (!strcmp(name, "y"))                                  return kPad_Tri;
    // Sticks
    if (!strcmp(name, "l3") || !strcmp(name, "ls"))          return kPad_L3;
    if (!strcmp(name, "r3") || !strcmp(name, "rs"))          return kPad_R3;

    return (JoypadButton)-1;
}

static void LoadInputScript() {
    gInputScriptLoaded = true;
    const char *path = getenv("MILO_INPUT_SCRIPT");
    if (!path || !path[0]) return;

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("DC3 Native: MILO_INPUT_SCRIPT: cannot open '%s'\n", path);
        return;
    }

    char line[256];
    int lineNum = 0;
    while (fgets(line, sizeof(line), f)) {
        lineNum++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        // Try wait_screen directive
        char screenBuf[64];
        if (sscanf(line, " wait_screen %63s", screenBuf) == 1) {
            ScriptDirective d = {};
            d.type = kDirectiveWaitScreen;
            strncpy(d.screenName, screenBuf, sizeof(d.screenName) - 1);
            gScript.push_back(d);
            continue;
        }

        // Parse button: "+N button" (relative) or "N button" (absolute)
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') continue;

        bool isRelative = false;
        if (*p == '+') {
            isRelative = true;
            p++;
        }

        int frame;
        char btnName[64];
        if (sscanf(p, "%d %63s", &frame, btnName) != 2) continue;

        for (char *c = btnName; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c += 32;
        }

        JoypadButton btn = ParseButtonName(btnName);
        if ((int)btn < 0) {
            printf("DC3 Native: MILO_INPUT_SCRIPT:%d: unknown button '%s'\n", lineNum, btnName);
            continue;
        }

        ScriptDirective d = {};
        d.type = kDirectiveButton;
        d.frame = frame;
        d.button = btn;
        d.relative = isRelative;
        gScript.push_back(d);
    }
    fclose(f);

    printf("DC3 Native: loaded %d script directives from '%s'\n", (int)gScript.size(), path);
}

// Stateful script executor. Processes directives in order, blocking on
// wait_screen until the target screen is current and not in transition.
static unsigned int GetScriptedButtons(int currentFrame) {
    if (gScript.empty()) return 0;

    unsigned int buttons = 0;

    while (gScriptCursor < gScript.size()) {
        ScriptDirective &d = gScript[gScriptCursor];

        if (d.type == kDirectiveWaitScreen) {
            if (!gWaiting) {
                gWaiting = true;
                gWaitTarget = d.screenName;
                gWaitSatisfiedFrame = -1;
                gWaitStartFrame = currentFrame;
            }

            bool satisfied = false;
            if (TheUI && TheUI->CurrentScreen() && !TheUI->InTransition()) {
                satisfied = (strcmp(TheUI->CurrentScreen()->Name(), gWaitTarget) == 0);
            }

            if (satisfied) {
                gWaitSatisfiedFrame = currentFrame;
                gWaiting = false;
                printf("DC3 Input: wait_screen '%s' satisfied at frame %d\n",
                    gWaitTarget, currentFrame);
                gScriptCursor++;
                continue;
            }

            if (currentFrame - gWaitStartFrame > kWaitTimeoutFrames) {
                const char *actual = (TheUI && TheUI->CurrentScreen())
                    ? TheUI->CurrentScreen()->Name() : "<none>";
                printf("DC3 Input: TIMEOUT waiting for '%s' (current='%s', %d frames)\n",
                    gWaitTarget, actual, currentFrame - gWaitStartFrame);
                gWaiting = false;
                gWaitSatisfiedFrame = currentFrame;
                gScriptCursor++;
                continue;
            }

            break; // Still waiting
        }

        if (d.type == kDirectiveButton) {
            int targetFrame = d.relative
                ? (gWaitSatisfiedFrame >= 0 ? gWaitSatisfiedFrame + d.frame : d.frame)
                : d.frame;

            if (currentFrame == targetFrame) {
                buttons |= (1 << d.button);
                gScriptCursor++;
                continue;
            } else if (currentFrame > targetFrame) {
                gScriptCursor++;
                continue;
            } else {
                break; // Future event
            }
        }

        gScriptCursor++;
    }

    return buttons;
}

// ============================================================================
// Joypad lifecycle
// ============================================================================

void JoypadInit() {
    MILO_LOG("[Native] JoypadInit\n");
    DataArray *cfg = SystemConfig("joypad");
    JoypadInitCommon(cfg);
    JoypadReset();
#ifdef __EMSCRIPTEN__
    InitWebInput();
#endif
}

void JoypadReset() {
    ResetAllUsersPads();
    // Set pad 0 as connected analog controller with default user
    JoypadData *pad = JoypadGetPadData(0);
    pad->mConnected = true;
    pad->mType = kJoypadAnalog;
    // Don't hardcode a controller type — let JoypadControllerTypePadNum
    // auto-detect from gControllersCfg. Avoids DTA lookup crash when
    // the type doesn't exist in joypad.dta button_meanings.
    pad->mControllerType = Symbol();
    pad->mNumAnalogSticks = 2;
    pad->mTranslateSticks = true;
}

void JoypadTerminate() {
    JoypadTerminateCommon();
}

// ============================================================================
// Poll — reads GLFW input (windowed) or scripted input (headless)
// ============================================================================

void JoypadPoll() {
    // Lazy-load input script on first poll
    if (!gInputScriptLoaded) LoadInputScript();

    // Use our own monotonic frame counter for input scripting.
    // TheRnd.GetFrameID() can stall or jump (it's incremented in
    // BeginDrawing, but JoypadPoll runs before BeginDrawing).
    static int sInputFrame = 0;
    int currentFrame = sInputFrame++;

    for (int pad = 0; pad < kNumJoypads; pad++) {
        JoypadData *data = JoypadGetPadData(pad);
        if (!data->mConnected)
            continue;

        unsigned int newButtons = 0;

#ifdef __EMSCRIPTEN__
        if (pad == 0) {
            // --- Web: keyboard + gamepad buttons (OR'd together) ---
            newButtons = GetWebKeyButtons() | GetWebGamepadButtons();
        }
#else
        if (gNativeWindow) {
            // --- Windowed mode: GLFW Gamepad ---
            GLFWgamepadstate gpState;
            bool hasGamepad = (glfwJoystickIsGamepad(pad) && glfwGetGamepadState(pad, &gpState));

            if (hasGamepad) {
                // Face buttons (Xbox layout -> Milo PS-style enum)
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_A])           newButtons |= (1 << kPad_X);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_B])           newButtons |= (1 << kPad_Circle);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_X])           newButtons |= (1 << kPad_Square);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_Y])           newButtons |= (1 << kPad_Tri);

                // Bumpers
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER]) newButtons |= (1 << kPad_L1);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER])newButtons |= (1 << kPad_R1);

                // Menu buttons
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_BACK])        newButtons |= (1 << kPad_Select);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_START])       newButtons |= (1 << kPad_Start);

                // Thumbstick clicks
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB])  newButtons |= (1 << kPad_L3);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB]) newButtons |= (1 << kPad_R3);

                // D-pad
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP])    newButtons |= (1 << kPad_DUp);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN])  newButtons |= (1 << kPad_DDown);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT])  newButtons |= (1 << kPad_DLeft);
                if (gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]) newButtons |= (1 << kPad_DRight);

                // Triggers (remap [-1,1] -> [0,1])
                float lt = (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  + 1.0f) * 0.5f;
                float rt = (gpState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
                data->mTriggers[0] = lt;
                data->mTriggers[1] = rt;
                if (lt > kTriggerThreshold) newButtons |= (1 << kPad_L2);
                if (rt > kTriggerThreshold) newButtons |= (1 << kPad_R2);

                // Sticks
                data->mSticks[0][0] = gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
                data->mSticks[0][1] = gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
                data->mSticks[1][0] = gpState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
                data->mSticks[1][1] = gpState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
            }

            // --- Keyboard as pad 0 fallback ---
            if (pad == 0) {
                if (glfwGetKey(gNativeWindow, GLFW_KEY_UP)    == GLFW_PRESS) newButtons |= (1 << kPad_DUp);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_DOWN)  == GLFW_PRESS) newButtons |= (1 << kPad_DDown);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_LEFT)  == GLFW_PRESS) newButtons |= (1 << kPad_DLeft);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_RIGHT) == GLFW_PRESS) newButtons |= (1 << kPad_DRight);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_ENTER) == GLFW_PRESS) newButtons |= (1 << kPad_X);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_ESCAPE)== GLFW_PRESS) newButtons |= (1 << kPad_Circle);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_SPACE) == GLFW_PRESS) newButtons |= (1 << kPad_Start);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_TAB)   == GLFW_PRESS) newButtons |= (1 << kPad_Select);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_Q)     == GLFW_PRESS) newButtons |= (1 << kPad_L1);
                if (glfwGetKey(gNativeWindow, GLFW_KEY_E)     == GLFW_PRESS) newButtons |= (1 << kPad_R1);
            }
        } else if (pad == 0) {
            // --- Headless mode: scripted input (pad 0 only) ---
            newButtons = GetScriptedButtons(currentFrame);
        }
#endif // __EMSCRIPTEN__

        // Phase 4: HTTP input injection (pad 0 only, works in both modes)
#ifdef DC3_HTTP_SERVER
        if (pad == 0 && TheHttpServer)
            newButtons |= TheHttpServer->ConsumeHttpButtons();
#endif

        // Translate analog sticks to digital buttons
        if (data->mTranslateSticks) {
            TranslateSticksToButs(*data, newButtons);
        }

        // Compute deltas
        unsigned int oldButtons = data->mButtons;
        data->mNewPressed  = newButtons & ~oldButtons;
        data->mNewReleased = oldButtons & ~newButtons;
        data->mButtons = newButtons;

        // Native button-to-action mapping (DTA config may not be loaded)
        auto nativeButtonToAction = [](JoypadButton btn) -> JoypadAction {
            switch (btn) {
            case kPad_X:        return kAction_Confirm;     // A button
            case kPad_Circle:   return kAction_Cancel;      // B button
            case kPad_Start:    return kAction_Start;
            case kPad_Select:   return kAction_Option;
            case kPad_DUp:      return kAction_Up;
            case kPad_DDown:    return kAction_Down;
            case kPad_DLeft:    return kAction_Left;
            case kPad_DRight:   return kAction_Right;
            case kPad_L1:       return kAction_PageUp;      // LB
            case kPad_R1:       return kAction_PageDown;    // RB
            case kPad_Square:   return kAction_ViewModify;  // X button
            case kPad_Tri:      return kAction_ShellOption;  // Y button
            case kPad_LStickUp:    return kAction_Up;
            case kPad_LStickDown:  return kAction_Down;
            case kPad_LStickLeft:  return kAction_Left;
            case kPad_LStickRight: return kAction_Right;
            default:            return kAction_None;
            }
        };

        // Broadcast button messages
        for (int b = 0; b < kPad_NumButtons; b++) {
            if (data->mNewPressed & (1 << b)) {
                JoypadAction action = ButtonToAction((JoypadButton)b, data->mControllerType);
                if (action == kAction_None)
                    action = nativeButtonToAction((JoypadButton)b);
                ButtonDownMsg msg(data->mUser, (JoypadButton)b, action, pad);
                JoypadPushThroughMsg(msg);
            }
        }
        for (int b = 0; b < kPad_NumButtons; b++) {
            if (data->mNewReleased & (1 << b)) {
                JoypadAction action = ButtonToAction((JoypadButton)b, data->mControllerType);
                if (action == kAction_None)
                    action = nativeButtonToAction((JoypadButton)b);
                ButtonUpMsg msg(data->mUser, (JoypadButton)b, action, pad);
                JoypadPushThroughMsg(msg);
            }
        }
    }
}
