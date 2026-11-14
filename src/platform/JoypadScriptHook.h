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

// ---------------------------------------------------------------------------
// `highlight NAME` (or `+N highlight NAME`: from N frames after the last
// satisfied wait): move the focused list's highlight onto the entry NAME, by
// pressing down / up, whatever row it is on. It does not confirm.
//
// A flow that counts presses ("51 downs reach omg") breaks as soon as the list
// changes -- downloadable songs, a sort order, an unlocked entry -- and then
// plays the wrong song, silently. `highlight omg` asks the game where omg is
// relative to the highlight, presses toward it one row at a time (a press
// every few frames, only while the list is still), and is satisfied when the
// highlight is on it; a `+60 confirm` after it then plays omg however many
// rows precede it. Like wait_screen it is a wait: the `+N` lines after it
// count from the frame the highlight arrived.
//
// The game answers through a JoypadScriptSelectFn. With none registered the
// directive gives up at once; when the entry is never reached (no focused
// list, no such entry, a list that does not move) it gives up after its
// timeout. Either way it says why and the rest of the script is abandoned:
// pressing on would confirm the wrong entry.
enum JoypadScriptSelectState {
    kJoypadSelectNoList = 0, // nothing focused that the game can search
    kJoypadSelectNotFound, // the focused list has no entry by that name
    kJoypadSelectMoving, // the list is still scrolling; ask again later
    kJoypadSelectReady, // *delta = entry's row - highlighted row (0 = on it)
};
typedef JoypadScriptSelectState (*JoypadScriptSelectFn)(const char *name, int *delta);

// Defined so a game can compile its answer only against an engine that has it.
#define MILO_JOYPAD_SCRIPT_HIGHLIGHT 1

// Register (or, with nullptr, clear) the game's answer.
void JoypadScriptSetSelectQuery(JoypadScriptSelectFn fn);

#endif // MILO_ENGINE_PLATFORM_JOYPADSCRIPTHOOK_H
