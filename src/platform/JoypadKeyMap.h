// JoypadKeyMap — the desktop keyboard as pad 0 in a GLFW window
// (Joypad_Native.cpp). One table, so the poll, the startup printout and the
// tests cannot disagree about which key is which button.
//
// Keys are GLFW key codes (GLFW_KEY_*). Buttons are JoypadButton values: Milo
// keeps the PlayStation names (kPad_X is the Xbox A button, kPad_Square is X,
// kPad_Tri is Y, kPad_L2/R2 are LT/RT), so each binding also carries the Xbox
// name the game's help bars show.
//
// A key pressed together with Ctrl, Alt or Super is not a pad press: those
// chords belong to the host (Ctrl+Q, Alt+Tab, Cmd+Q). The one host chord the
// engine itself reads is Ctrl+Q, the quit key, and only once a game has
// registered what quitting means (JoypadSetQuitKeyHandler); without one,
// Ctrl+Q does nothing.
//
// The web build keeps its own JavaScript copy of the map in
// Joypad_Native.cpp (InitWebInput); keep the two in step.

#ifndef MILO_ENGINE_PLATFORM_JOYPADKEYMAP_H
#define MILO_ENGINE_PLATFORM_JOYPADKEYMAP_H

#include "os/Joypad.h"

struct JoypadKeyBinding {
    int key;             // GLFW_KEY_*
    JoypadButton button; // pad-0 button the key holds down
    const char *keyName; // as printed: "Enter", "Esc", "X"
    const char *padName; // Xbox name: "A", "B", "LT", "D-pad"
};

// The table, in print order (consecutive rows with one padName print as one
// group). *count receives its length.
const JoypadKeyBinding *JoypadKeyBindings(int *count);

// Answers "is this GLFW key held?" for one key.
typedef bool (*JoypadKeyDownFn)(int key, void *ctx);

// The pad buttons the keyboard holds, as a (1 << JoypadButton) mask. Zero
// while Ctrl, Alt or Super is held.
unsigned int JoypadKeyboardButtons(JoypadKeyDownFn keyDown, void *ctx);

// True while the quit chord (Ctrl+Q) is held.
bool JoypadKeyboardQuitChordHeld(JoypadKeyDownFn keyDown, void *ctx);

// What the quit chord does: called once per press of Ctrl+Q, from JoypadPoll.
// nullptr (the default) = no quit key.
typedef void (*JoypadQuitKeyFn)();
void JoypadSetQuitKeyHandler(JoypadQuitKeyFn fn);

// Prints the map as one line, "<prefix>Up/Down/Left/Right = D-pad, Enter = A,
// ...", ending with "Ctrl+Q = quit" when a quit handler is registered.
void JoypadPrintKeyMap(const char *prefix);

// Xbox name of a pad button ("A", "LT", "D-pad up", ...), for logs.
const char *JoypadXboxButtonName(int button);

#endif // MILO_ENGINE_PLATFORM_JOYPADKEYMAP_H
