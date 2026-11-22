// DC3 Native Port - Joypad: DC3's JoypadData over the engine's native input
// sources. Replaces Joypad_Stub.cpp.
//
// The sources themselves -- GLFW gamepad + keyboard (windowed), the page's
// keyboard + gamepad (web), the MILO_INPUT_SCRIPT script (headless; format in
// JoypadNativeSource.cpp and scripts/dc3-input-flows/README.txt) and the debug
// server's buttons -- live in JoypadNativeSource.cpp behind the game-agnostic
// platform/JoypadNativeSource.h. This file is the DC3-shaped consumer: its
// JoypadPoll reads them each poll into JoypadData and sends the pad messages.

#include "os/Joypad.h"
#include "os/JoypadMsgs.h"
#include "os/Debug.h"
#include "os/System.h"
#include "ui/UI.h"
#include "platform/JoypadNativeSource.h"
#include "platform/JoypadScriptHook.h"
#include "platform/JoypadKeyMap.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

// MILO_INPUT_LOG=1: one line per pad button message JoypadPoll sends, so a
// test (or a person) can see which button and action a key produced.
static bool InputLogEnabled() {
    static const bool on = [] {
        const char *v = getenv("MILO_INPUT_LOG");
        return v && v[0] && strcmp(v, "0") != 0;
    }();
    return on;
}

// The script's waits (wait_screen, highlight) see TheUI: its current screen
// and whether a transition is running. Registered at static init, so it is in
// place before the first poll, as the direct TheUI reads it replaces were.
static bool Dc3ScriptUi(const char **screen, bool *inTransition) {
    if (!TheUI) return false;
    *screen = TheUI->CurrentScreen() ? TheUI->CurrentScreen()->Name() : nullptr;
    *inTransition = TheUI->InTransition();
    return true;
}
static const bool sScriptUiRegistered = (JoypadScriptSetUiQuery(Dc3ScriptUi), true);

// ============================================================================
// Joypad lifecycle
// ============================================================================

void JoypadInit() {
    MILO_LOG("[Native] JoypadInit\n");
    DataArray *cfg = SystemConfig("joypad");
    JoypadInitCommon(cfg);
    JoypadReset();
    JoypadNativeSourceInit(); // the web build's key listeners
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
// Poll — the native sources (JoypadNativeSource.h) into DC3's JoypadData
// ============================================================================

void JoypadPoll() {
    // Loads the input script on the first poll and advances the script's
    // frame counter, once per poll.
    JoypadNativeSourceAdvance();

    for (int pad = 0; pad < kNumJoypads; pad++) {
        JoypadData *data = JoypadGetPadData(pad);
        if (!data->mConnected)
            continue;

        // Script (headless), or GLFW gamepad | keyboard (windowed), or the
        // web's keyboard | gamepad; then the HTTP buttons.
        JoypadNativePadState src;
        JoypadNativeSourceRead(pad, &src);
        unsigned int newButtons = src.buttons;
        if (src.hasTriggers) {
            data->mTriggers[0] = src.triggers[0];
            data->mTriggers[1] = src.triggers[1];
        }
        if (src.hasSticks) {
            data->mSticks[0][0] = src.sticks[0][0];
            data->mSticks[0][1] = src.sticks[0][1];
            data->mSticks[1][0] = src.sticks[1][0];
            data->mSticks[1][1] = src.sticks[1][1];
        }

        // Translate analog sticks to digital buttons
        if (data->mTranslateSticks) {
            TranslateSticksToButs(*data, newButtons);
        }

        // Compute deltas
        unsigned int oldButtons = data->mButtons;
        data->mNewPressed  = newButtons & ~oldButtons;
        data->mNewReleased = oldButtons & ~newButtons;
        data->mButtons = newButtons;

        // Native button-to-action mapping, for presses ButtonToAction cannot
        // map yet (the first press of a session, before the pad has a
        // controller type). It must agree with joypad.dta's
        // DEFAULT_BUTTON_MEANINGS / GLOBAL_BUTTON_MEANINGS, which answer every
        // later press: X = ShellOption, Y = Option, Back = ViewModify. (It
        // had X/Y/Back as ViewModify/ShellOption/Option, so a session's first
        // X press and its release carried different actions.)
        auto nativeButtonToAction = [](JoypadButton btn) -> JoypadAction {
            switch (btn) {
            case kPad_X:        return kAction_Confirm;     // A button
            case kPad_Circle:   return kAction_Cancel;      // B button
            case kPad_Start:    return kAction_Start;
            case kPad_Select:   return kAction_ViewModify;  // Back
            case kPad_DUp:      return kAction_Up;
            case kPad_DDown:    return kAction_Down;
            case kPad_DLeft:    return kAction_Left;
            case kPad_DRight:   return kAction_Right;
            case kPad_L1:       return kAction_PageUp;      // LB
            case kPad_R1:       return kAction_PageDown;    // RB
            case kPad_Square:   return kAction_ShellOption; // X button
            case kPad_Tri:      return kAction_Option;      // Y button
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
                if (InputLogEnabled())
                    printf("Joypad: down pad=%d button=%d (%s) action=%d\n", pad, b,
                        JoypadXboxButtonName(b), (int)action);
                ButtonDownMsg msg(data->mUser, (JoypadButton)b, action, pad);
                JoypadPushThroughMsg(msg);
            }
        }
        for (int b = 0; b < kPad_NumButtons; b++) {
            if (data->mNewReleased & (1 << b)) {
                JoypadAction action = ButtonToAction((JoypadButton)b, data->mControllerType);
                if (action == kAction_None)
                    action = nativeButtonToAction((JoypadButton)b);
                if (InputLogEnabled())
                    printf("Joypad: up pad=%d button=%d (%s) action=%d\n", pad, b,
                        JoypadXboxButtonName(b), (int)action);
                ButtonUpMsg msg(data->mUser, (JoypadButton)b, action, pad);
                JoypadPushThroughMsg(msg);
            }
        }
    }
}
