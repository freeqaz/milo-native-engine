// platform/TextInput_Native.h -- text-editing input for natively drawn UI: the
// characters a keyboard types and the keys that edit a line of text, as a
// queue the consumer drains once a frame. No game headers; every consumer of
// the engine can include it.
//
// WHAT IT IS
//   The pad map (platform/JoypadKeyMap.h) turns keys into pad buttons and the
//   game's Keyboard (os/Keyboard.h) sees key codes; neither gives text.
//   This is the third reader of the same keyboard: Unicode characters as the
//   OS composes them (layout, Shift, dead keys), and Backspace, Delete, Left,
//   Right, Home, End, with key repeat as the OS delivers it.
//
// USE (a text field, each frame while it has focus)
//       TextInputSetCapture(true);                  // once, on focus
//       TextInputEvent ev[32];
//       for (int n; (n = TextInputDrain(ev, 32)) > 0;)
//           for (int i = 0; i < n; i++) {
//               if (ev[i].type == kTextInputChar) field.Insert(ev[i].codepoint);
//               else field.Edit(ev[i].type, ev[i].mods);
//           }
//       ...
//       TextInputSetCapture(false);                 // once, on blur
//
// CAPTURE
//   Off by default, and while it is off nothing here changes anything: keyboard
//   events are not queued, every key keeps its pad meaning, and a consumer that
//   never turns it on behaves exactly as before (rb3 and rb3-xenon link this
//   engine too). While capture is ON:
//     - typed characters and edit keys are queued;
//     - a key that produces text or edits it is NOT also a pad button. Of the
//       pad map's keys that is X, Y, Q, E, Z, C, Space and Backspace (on the
//       web also W, A, S, D), so typing "queen" does not change the sort and
//       Backspace does not go back;
//     - Arrows, Enter, Esc and Tab KEEP their pad meaning (Left and Right are
//       both: an edit event here and a D-pad press there -- the consumer
//       decides which one moves the caret);
//     - a key already held when capture turned on types nothing until it is
//       released (press Y to open a search box: no "yyyy" from its repeat), and
//       a suppressed key still held when capture turns off is not a pad press
//       until it is released and pressed again.
//   Turning capture on or off empties the queue.
//
// QUEUE
//   At most kTextInputQueueMax events; when full, new events are dropped (what
//   was typed first is kept). Events are in the order the OS delivered them.
//   Drain from the thread that polls input (the main thread).
//
// SOURCES
//   Desktop: GLFW character and key callbacks on the engine's window, CHAINED:
//     TextInputInit keeps whatever callbacks were installed and calls them
//     after its own, and a callback installed later (ImGui's GLFW backend
//     installs its own and chains to its predecessor) keeps ours running. So
//     ImGui, the fullscreen toggle and the game's Keyboard all keep working
//     whichever is installed first.
//   Web: a `keydown` listener on the document (event.key of length 1 is a
//     character; the named keys map to the edit keys). The pad key bitmask is
//     maintained as before; under capture it skips the text keys.
//   Headless (no window): nothing but injection.
//   Injection (TextInputInject*): for tests and a debug-server endpoint.
//     Injected events are queued whether or not capture is on, and never touch
//     pad buttons.
//
// Not measured: IME composition (CJK input methods) is not handled -- GLFW's
//   character callback delivers only committed characters, and the web
//   listener ignores `isComposing` keys.

#ifndef MILO_ENGINE_PLATFORM_TEXTINPUT_NATIVE_H
#define MILO_ENGINE_PLATFORM_TEXTINPUT_NATIVE_H

#include <cstdint>

enum TextInputEventType {
    kTextInputChar = 0, // `codepoint` was typed
    kTextInputBackspace,
    kTextInputDelete,
    kTextInputLeft,
    kTextInputRight,
    kTextInputHome,
    kTextInputEnd,
};

// Modifier bits of an edit-key event (0 for characters: the OS has already
// applied them).
enum TextInputMods {
    kTextInputShift = 1,
    kTextInputCtrl = 2,
    kTextInputAlt = 4,
    kTextInputSuper = 8,
};

struct TextInputEvent {
    TextInputEventType type;
    uint32_t codepoint; // kTextInputChar: a Unicode scalar value >= 0x20; else 0
    uint8_t mods;       // TextInputMods, edit keys only
};

enum { kTextInputQueueMax = 256 };

// Installs the keyboard hooks (desktop: on the engine's window, if there is
// one; web: the document listener). Idempotent. The engine's KeyboardInit
// calls it; TextInputSetCapture(true) calls it too, for a consumer whose
// keyboard is its own.
void TextInputInit();
// Removes the hooks (desktop: only while they are still the window's top
// callbacks; otherwise they stay as a pass-through) and clears the state.
void TextInputTerminate();

void TextInputSetCapture(bool on);
bool TextInputCapture();

// Moves up to `max` events, oldest first, into `out`; returns how many.
int TextInputDrain(TextInputEvent* out, int max);
int TextInputPending();
void TextInputClear();

// Injection: as if typed, but queued regardless of capture.
void TextInputInjectChar(uint32_t codepoint);
void TextInputInjectKey(TextInputEventType key, unsigned mods = 0);
// Every character of a UTF-8 string; returns how many were queued. An invalid
// byte sequence is skipped (one byte at a time), control characters too.
int TextInputInjectUtf8(const char* utf8);

// UTF-8 helpers for the consumer's line buffer. Encode writes 1-4 bytes plus a
// NUL and returns the byte count (0 for a value that is not a Unicode scalar).
// Decode reads one character at `s` (NUL-terminated), stores it and returns
// the bytes consumed; 0 at the end of the string, and for an invalid sequence
// it stores U+FFFD and returns 1.
int TextInputEncodeUtf8(uint32_t codepoint, char out[5]);
int TextInputDecodeUtf8(const char* s, uint32_t* codepoint);

// True when, under capture, this key (a GLFW key code) is text's and not the
// pad's: the printable keys, the keypad, Backspace, Delete, Home and End.
// Arrows, Enter, Esc, Tab and the function keys are not.
bool TextInputKeyIsText(int glfwKey);
// The pad map's question, one call per bound key per poll, `index` being the
// key's row in the map: should a key that is `down` count as its pad button?
// False for a text key under capture, and afterwards until it is released.
// With capture off and no key held over from it this is just `down`.
bool TextInputPadKeyAllowed(int index, int glfwKey, bool down);

// ---- device side: what the hooks call; tests call them as a keyboard ----
// A character from the OS (GLFW's character callback).
void TextInputDeviceChar(uint32_t codepoint);
// A key event with GLFW's numbers: `action` 0 release, 1 press, 2 repeat;
// `mods` GLFW_MOD_* bits.
void TextInputDeviceKey(int glfwKey, int action, int mods);

#ifndef __EMSCRIPTEN__
struct GLFWwindow;
// The GLFW callbacks TextInputInit installs: the two device calls above, then
// the callback that was installed before (the chain).
void TextInputGlfwChar(GLFWwindow* window, unsigned int codepoint);
void TextInputGlfwKey(GLFWwindow* window, int key, int scancode, int action, int mods);
// Test seam: stand in for "the callbacks that were installed before ours".
void TextInputSetChainForTest(void (*prevChar)(GLFWwindow*, unsigned int),
                              void (*prevKey)(GLFWwindow*, int, int, int, int));
#endif

#endif // MILO_ENGINE_PLATFORM_TEXTINPUT_NATIVE_H
