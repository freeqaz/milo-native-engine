// JoypadNativeSource -- the engine's native input sources, as pad state a
// game's own joypad back end can consume.
//
// The engine has four sources of pad input (JoypadNativeSource.cpp):
//   - headless: the MILO_INPUT_SCRIPT script (platform/JoypadScriptHook.h for
//     its wake / highlight / UI hooks), pad 0;
//   - a GLFW window: a GLFW gamepad on every pad, and the keyboard as pad 0
//     (platform/JoypadKeyMap.h), with sticky keys and the Ctrl+Q quit chord;
//   - the web build: the page's keyboard and its first Gamepad API pad, pad 0;
//   - with DC3_HTTP_SERVER: buttons injected over the debug server, pad 0.
// A window, when there is one, wins over the script (a script drives headless
// runs only), exactly as the engine's own JoypadPoll has always read them.
//
// The engine's JoypadPoll (Joypad_Native.cpp, DC3's JoypadData) is one
// consumer. A game whose joypad layer is not DC3-shaped -- rb3-xenon's
// retail XInput path, say -- calls the same two functions and gets the same
// presses, frame for frame, with none of DC3's JoypadData.
//
// This header includes nothing and names no game type. Buttons are a mask of
// (1 << b) for Milo's JoypadButton numbering, which every Milo game shares:
// kPad_L2 = 0, R2 1, L1 2, R1 3, Tri 4, Circle 5, X 6 (Xbox A), Square 7
// (Xbox X), Select 8, L3 9, R3 10, Start 11, DUp 12, DRight 13, DDown 14,
// DLeft 15.
//
// USE, once per joypad poll:
//       JoypadNativeSourceAdvance();
//       for (int pad = 0; pad < numPads; pad++) {
//           JoypadNativePadState s;
//           JoypadNativeSourceRead(pad, &s);
//           ... s.buttons, and s.sticks / s.triggers when s.has* ...
//       }
// Read a pad AT MOST ONCE per poll: the script's presses, the keyboard's
// sticky-key taps, the quit chord and the HTTP queue are consumed by the read
// that returns them, so a second read in the same poll would not see them.

#ifndef MILO_ENGINE_PLATFORM_JOYPADNATIVESOURCE_H
#define MILO_ENGINE_PLATFORM_JOYPADNATIVESOURCE_H

struct JoypadNativePadState {
    // A source can drive this pad now (JoypadNativeSourceConnected).
    bool connected;
    // (1 << JoypadButton) bits this poll: the script, or the GLFW gamepad ORed
    // with the keyboard, or the web keyboard ORed with the web gamepad; then
    // the HTTP buttons ORed in. Triggers past 0.3 set kPad_L2 / kPad_R2.
    // The sticks are NOT translated into kPad_LStick* bits: that is the
    // consumer's (TranslateSticksToButs on DC3).
    unsigned int buttons;
    // sticks[0] = left, sticks[1] = right; [0] = x, [1] = y; GLFW's range and
    // sign (-1..1, y positive DOWN). Only a GLFW gamepad reads them: with
    // hasSticks false, leave the pad's sticks as they were.
    bool hasSticks;
    float sticks[2][2];
    // triggers[0] = LT, triggers[1] = RT, 0..1. A GLFW gamepad reads them; in a
    // window without one, pad 0's keyboard gives 1 for a held Z / C and 0
    // otherwise. With hasTriggers false, leave the pad's triggers as they were.
    bool hasTriggers;
    float triggers[2];
};

// From the game's JoypadInit: installs the web build's key listeners (a no-op
// on desktop). Safe to call more than once.
void JoypadNativeSourceInit();

// Once per joypad poll, before the reads: loads MILO_INPUT_SCRIPT on the first
// call, then advances the script's input frame -- the engine's own monotonic
// counter, one per poll, so a script's frame N is the N-th poll (from 0)
// whatever the renderer's frame ID does. Returns this poll's frame.
int JoypadNativeSourceAdvance();

// Pad `pad`'s state for this poll (see the struct). At most once per pad per
// poll. A pad no source drives reads as connected = false, buttons 0.
void JoypadNativeSourceRead(int pad, JoypadNativePadState *out);

// Whether a source can drive `pad`, without consuming anything (so a game can
// answer "is a controller plugged in" at any time): pad 0 in a window (the
// keyboard), on the web, or headless with a non-empty MILO_INPUT_SCRIPT (loaded
// on first use); any pad with a GLFW gamepad in a window.
bool JoypadNativeSourceConnected(int pad);

// True when MILO_INPUT_SCRIPT named a script with at least one directive
// (loaded on first use).
bool JoypadNativeSourceScripted();

#endif // MILO_ENGINE_PLATFORM_JOYPADNATIVESOURCE_H
