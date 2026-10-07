// JoypadScriptHook — the seam between the scripted-input `wake` directive
// (MILO_INPUT_SCRIPT, parsed in Joypad_Native.cpp) and the game that knows
// whether a wake press is needed.
//
// Some Milo games swallow the first pad press that arrives while the game is
// not in its pad-driven mode: on Dance Central 3 a press outside "controller
// mode" only enters controller mode (ShellInput::OnMsg(ButtonDownMsg)), and
// controller mode times out after a few seconds without pad input. A flow that
// must drive both the original game (under Xenia) and a native port therefore
// writes, before each screen's first real press,
//
//     +30 wake
//
// meaning "the player presses the wake button". At its frame the directive asks
// the registered JoypadScriptWakeNeededFn; if it answers false (the game is
// already in its pad-driven mode) the directive does nothing, otherwise it
// presses the wake button for one frame like any other button directive. With
// no function registered (a game with no such mode) the press always happens.
//
// The wake button is kPad_L3 unless MILO_INPUT_WAKE_BUTTON names another
// script button (same names as the script: l3, r3, start, ...).
//
// This header includes nothing and names no game type.

#ifndef MILO_ENGINE_PLATFORM_JOYPADSCRIPTHOOK_H
#define MILO_ENGINE_PLATFORM_JOYPADSCRIPTHOOK_H

// True when a wake press would change anything (the game is NOT in its
// pad-driven mode), false when the press would be a no-op.
typedef bool (*JoypadScriptWakeNeededFn)();

// Register (or, with nullptr, clear) the game's answer. Safe to call before
// the first JoypadPoll; the directive reads it at the frame it fires.
void JoypadScriptSetWakeNeeded(JoypadScriptWakeNeededFn fn);

#endif // MILO_ENGINE_PLATFORM_JOYPADSCRIPTHOOK_H
