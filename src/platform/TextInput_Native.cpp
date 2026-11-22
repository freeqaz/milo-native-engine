// platform/TextInput_Native.cpp -- text-editing input (contract:
// platform/TextInput_Native.h). No game headers.
//
// One state for every source: a bounded queue, the capture switch, and the
// two "held over" sets that keep a key from changing roles mid-press (a key
// held when capture turns on types nothing; a text key held when capture
// turns off presses no pad button). The desktop hooks are GLFW callbacks that
// chain; the web hook is a document keydown listener that fills a JavaScript
// array TextInputDrain pulls from.
#include "platform/TextInput_Native.h"

#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_asm.h>
#else
#include <GLFW/glfw3.h>
// Set by Rnd_Wgpu during Init(); null when headless.
extern GLFWwindow* gNativeWindow;
#endif

namespace {

// GLFW's key numbers, spelled here so the web build (no GLFW header) and the
// key classification share them.
enum {
    kKeySpace = 32,
    kKeyWorld2 = 162,     // last printable key
    kKeyBackspace = 259,
    kKeyDelete = 261,
    kKeyRight = 262,
    kKeyLeft = 263,
    kKeyHome = 268,
    kKeyEnd = 269,
    kKeyKp0 = 320,
    kKeyKpAdd = 334,
    kKeyKpEnter = 335,
    kKeyKpEqual = 336,
    kKeyLast = 348,
    kActionRelease = 0,
};

TextInputEvent sQueue[kTextInputQueueMax];
int sHead = 0;  // next write
int sCount = 0;
bool sCapture = false;
bool sInitialized = false;

// Keys the device has down (from our own key hook), and the ones that were
// down when capture turned on: those type nothing until released.
bool sDown[kKeyLast + 1];
bool sHeldAtCapture[kKeyLast + 1];
int sHeldAtCaptureCount = 0;
// The last key event was a press or repeat of a held-over key: the character
// event that follows it (GLFW delivers key, then char) is its repeat.
bool sSwallowNextChar = false;

// Pad-map rows whose key was suppressed and is still down (TextInputPadKeyAllowed).
enum { kMaxPadRows = 64 };
bool sPadHeldOver[kMaxPadRows];

void Push(TextInputEventType type, uint32_t codepoint, unsigned mods) {
    if (sCount >= kTextInputQueueMax) return; // full: keep what was typed first
    TextInputEvent& e = sQueue[sHead];
    e.type = type;
    e.codepoint = codepoint;
    e.mods = (uint8_t)mods;
    sHead = (sHead + 1) % kTextInputQueueMax;
    sCount++;
}

bool IsTextCodepoint(uint32_t cp) {
    // No control characters (Backspace, Enter and Tab arrive as keys), no
    // surrogates, nothing past the Unicode range.
    if (cp < 0x20 || cp == 0x7F) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    return cp <= 0x10FFFF;
}

bool EditKeyType(int key, TextInputEventType* type) {
    switch (key) {
    case kKeyBackspace: *type = kTextInputBackspace; return true;
    case kKeyDelete:    *type = kTextInputDelete;    return true;
    case kKeyLeft:      *type = kTextInputLeft;      return true;
    case kKeyRight:     *type = kTextInputRight;     return true;
    case kKeyHome:      *type = kTextInputHome;      return true;
    case kKeyEnd:       *type = kTextInputEnd;       return true;
    default:            return false;
    }
}

// GLFW_MOD_SHIFT 1, CONTROL 2, ALT 4, SUPER 8: the same bits as TextInputMods.
unsigned ModsFromGlfw(int mods) { return (unsigned)mods & 0xF; }

void ClearQueue() {
    sHead = 0;
    sCount = 0;
}

#ifndef __EMSCRIPTEN__
GLFWcharfun sPrevChar = nullptr;
GLFWkeyfun sPrevKey = nullptr;
GLFWwindow* sHookedWindow = nullptr;
#endif

#ifdef __EMSCRIPTEN__
// Pulls what the document listener queued (InstallWebListener) into sQueue.
// Entries are numbers: a character is its code point; an edit key is
// -(type + 16 * mods).
void PullWebEvents() {
    for (int guard = 0; guard < kTextInputQueueMax; guard++) {
        const int v = EM_ASM_INT({
            var q = window._miloTextQ;
            return q && q.length ? q.shift() : 0;
        });
        if (v == 0) break;
        if (v > 0) {
            if (IsTextCodepoint((uint32_t)v)) Push(kTextInputChar, (uint32_t)v, 0);
        } else {
            const int packed = -v;
            const int type = packed & 15;
            if (type >= kTextInputBackspace && type <= kTextInputEnd)
                Push((TextInputEventType)type, 0, (unsigned)(packed >> 4) & 0xF);
        }
    }
}

void InstallWebListener() {
    // EM_ASM blocks cannot hold C comments or object literals, so the tables
    // are built by assignment (as JoypadNativeSource.cpp's InitWebInput does).
    //   _miloTextCapture  the capture switch, read by this listener and by the
    //                     pad listener (JoypadNativeSource.cpp)
    //   _miloTextQ        events for PullWebEvents
    //   _miloTextDown     event.code of every key now down
    //   _miloTextHeld     the keys that were down when capture turned on
    EM_ASM({
        if (window._miloTextInstalled) return;
        window._miloTextInstalled = 1;
        window._miloTextCapture = 0;
        window._miloTextQ = [];
        window._miloTextDown = new Object();
        window._miloTextHeld = new Object();
        var edit = new Object();
        edit['Backspace'] = 1;
        edit['Delete'] = 2;
        edit['ArrowLeft'] = 3;
        edit['ArrowRight'] = 4;
        edit['Home'] = 5;
        edit['End'] = 6;
        document.addEventListener('keydown', function(e) {
            window._miloTextDown[e.code] = 1;
            if (!window._miloTextCapture || e.isComposing) return;
            if (window._miloTextHeld[e.code]) return;
            if (window._miloTextQ.length >= 256) return;
            var t = edit[e.key];
            if (t) {
                var mods = (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) |
                           (e.metaKey ? 8 : 0);
                window._miloTextQ.push(-(t + 16 * mods));
                if (t != 3 && t != 4) e.preventDefault();
                return;
            }
            if (e.ctrlKey || e.metaKey) return;
            var cp = e.key.codePointAt(0);
            if (e.key.length == (cp > 65535 ? 2 : 1) && cp >= 32 && cp != 127) {
                window._miloTextQ.push(cp);
                e.preventDefault();
            }
        }, true);
        document.addEventListener('keyup', function(e) {
            delete window._miloTextDown[e.code];
            delete window._miloTextHeld[e.code];
        }, true);
    });
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

int TextInputDrain(TextInputEvent* out, int max) {
#ifdef __EMSCRIPTEN__
    if (sInitialized) PullWebEvents();
#endif
    if (!out || max <= 0) return 0;
    int n = 0;
    int tail = (sHead - sCount + kTextInputQueueMax) % kTextInputQueueMax;
    while (n < max && sCount > 0) {
        out[n++] = sQueue[tail];
        tail = (tail + 1) % kTextInputQueueMax;
        sCount--;
    }
    return n;
}

int TextInputPending() {
#ifdef __EMSCRIPTEN__
    if (sInitialized) PullWebEvents();
#endif
    return sCount;
}

void TextInputClear() {
    ClearQueue();
#ifdef __EMSCRIPTEN__
    if (sInitialized) EM_ASM({ if (window._miloTextQ) window._miloTextQ.length = 0; });
#endif
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool TextInputCapture() { return sCapture; }

void TextInputSetCapture(bool on) {
    if (on) TextInputInit(); // a consumer whose keyboard is its own
    if (on == sCapture) return;
    sCapture = on;
    TextInputClear();
    sSwallowNextChar = false;
    if (on) {
        // What is down now was pressed for something else.
        memcpy(sHeldAtCapture, sDown, sizeof(sHeldAtCapture));
        sHeldAtCaptureCount = 0;
        for (int k = 0; k <= kKeyLast; k++) sHeldAtCaptureCount += sHeldAtCapture[k] ? 1 : 0;
    } else {
        memset(sHeldAtCapture, 0, sizeof(sHeldAtCapture));
        sHeldAtCaptureCount = 0;
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({
        window._miloTextCapture = $0;
        window._miloTextHeld = new Object();
        if ($0) {
            for (var k in window._miloTextDown) window._miloTextHeld[k] = 1;
        }
    }, on ? 1 : 0);
#endif
}

bool TextInputKeyIsText(int key) {
    if (key >= kKeySpace && key <= kKeyWorld2) return true;
    if (key >= kKeyKp0 && key <= kKeyKpEqual) return key != kKeyKpEnter;
    return key == kKeyBackspace || key == kKeyDelete || key == kKeyHome || key == kKeyEnd;
}

bool TextInputPadKeyAllowed(int index, int key, bool down) {
    const bool tracked = index >= 0 && index < kMaxPadRows;
    if (sCapture && TextInputKeyIsText(key)) {
        if (tracked) sPadHeldOver[index] = down;
        return false;
    }
    if (tracked && sPadHeldOver[index]) {
        // Suppressed under capture and not released since: still not a press.
        if (down) return false;
        sPadHeldOver[index] = false;
    }
    return down;
}

// ---------------------------------------------------------------------------
// Device side
// ---------------------------------------------------------------------------

void TextInputDeviceChar(uint32_t codepoint) {
    if (!sCapture) return;
    if (sSwallowNextChar) {
        sSwallowNextChar = false;
        return;
    }
    if (IsTextCodepoint(codepoint)) Push(kTextInputChar, codepoint, 0);
}

void TextInputDeviceKey(int key, int action, int mods) {
    if (key < 0 || key > kKeyLast) return;
    if (action == kActionRelease) {
        sDown[key] = false;
        if (sHeldAtCapture[key]) {
            sHeldAtCapture[key] = false;
            sHeldAtCaptureCount--;
        }
        return;
    }
    sDown[key] = true;
    if (!sCapture) return;
    // A press or repeat of a key held since before capture: neither it nor
    // the character event that follows it is text.
    sSwallowNextChar = sHeldAtCapture[key];
    if (sSwallowNextChar) return;
    TextInputEventType type;
    if (EditKeyType(key, &type)) Push(type, 0, ModsFromGlfw(mods));
}

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

void TextInputInjectChar(uint32_t codepoint) {
    if (IsTextCodepoint(codepoint)) Push(kTextInputChar, codepoint, 0);
}

void TextInputInjectKey(TextInputEventType key, unsigned mods) {
    if (key > kTextInputChar && key <= kTextInputEnd) Push(key, 0, mods & 0xF);
}

int TextInputInjectUtf8(const char* utf8) {
    if (!utf8) return 0;
    int queued = 0;
    while (*utf8) {
        uint32_t cp = 0;
        const int n = TextInputDecodeUtf8(utf8, &cp);
        if (n <= 0) break;
        utf8 += n;
        if (cp == 0xFFFD && n == 1) continue; // an invalid byte: skipped
        if (IsTextCodepoint(cp) && sCount < kTextInputQueueMax) {
            Push(kTextInputChar, cp, 0);
            queued++;
        }
    }
    return queued;
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

int TextInputEncodeUtf8(uint32_t cp, char out[5]) {
    unsigned char* o = (unsigned char*)out;
    int n = 0;
    if (cp < 0x80) {
        o[n++] = (unsigned char)cp;
    } else if (cp < 0x800) {
        o[n++] = (unsigned char)(0xC0 | (cp >> 6));
        o[n++] = (unsigned char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            out[0] = 0;
            return 0;
        }
        o[n++] = (unsigned char)(0xE0 | (cp >> 12));
        o[n++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[n++] = (unsigned char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        o[n++] = (unsigned char)(0xF0 | (cp >> 18));
        o[n++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        o[n++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[n++] = (unsigned char)(0x80 | (cp & 0x3F));
    }
    o[n] = 0;
    return n;
}

int TextInputDecodeUtf8(const char* str, uint32_t* codepoint) {
    const unsigned char* s = (const unsigned char*)str;
    if (!s || !s[0]) return 0;
    uint32_t cp = 0xFFFD;
    int len = 1;
    const unsigned char c = s[0];
    if (c < 0x80) {
        cp = c;
    } else {
        int need = 0;
        uint32_t min = 0;
        if ((c & 0xE0) == 0xC0) {
            need = 1;
            min = 0x80;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            min = 0x800;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            min = 0x10000;
            cp = c & 0x07;
        }
        bool ok = need > 0;
        for (int i = 1; ok && i <= need; i++) {
            if ((s[i] & 0xC0) != 0x80) ok = false; // also stops at the NUL
            else cp = (cp << 6) | (s[i] & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are invalid.
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (ok) {
            len = need + 1;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
    }
    if (codepoint) *codepoint = cp;
    return len;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

#ifndef __EMSCRIPTEN__
void TextInputGlfwChar(GLFWwindow* window, unsigned int codepoint) {
    TextInputDeviceChar(codepoint);
    if (sPrevChar) sPrevChar(window, codepoint);
}

void TextInputGlfwKey(GLFWwindow* window, int key, int scancode, int action, int mods) {
    TextInputDeviceKey(key, action, mods);
    if (sPrevKey) sPrevKey(window, key, scancode, action, mods);
}

void TextInputSetChainForTest(void (*prevChar)(GLFWwindow*, unsigned int),
                              void (*prevKey)(GLFWwindow*, int, int, int, int)) {
    sPrevChar = prevChar;
    sPrevKey = prevKey;
}
#endif

void TextInputInit() {
    if (sInitialized) return;
#ifdef __EMSCRIPTEN__
    InstallWebListener();
    sInitialized = true;
#else
    // Headless: no window, nothing to hook (and a later call tries again).
    if (!gNativeWindow) return;
    // Ours go on top and call whatever was there (the game's Keyboard,
    // GpuDevice's fullscreen toggle, ImGui's backend if it came first). One
    // installed after ours chains to ours the same way.
    GLFWcharfun prevChar = glfwSetCharCallback(gNativeWindow, TextInputGlfwChar);
    if (prevChar != TextInputGlfwChar) sPrevChar = prevChar;
    GLFWkeyfun prevKey = glfwSetKeyCallback(gNativeWindow, TextInputGlfwKey);
    if (prevKey != TextInputGlfwKey) sPrevKey = prevKey;
    sHookedWindow = gNativeWindow;
    sInitialized = true;
#endif
}

void TextInputTerminate() {
#ifndef __EMSCRIPTEN__
    if (sInitialized && sHookedWindow && sHookedWindow == gNativeWindow) {
        // Unhook only where ours is still the top callback; under a later one
        // (which holds a pointer to ours) it stays, forwarding to its
        // predecessor.
        GLFWcharfun curChar = glfwSetCharCallback(sHookedWindow, sPrevChar);
        if (curChar != TextInputGlfwChar) glfwSetCharCallback(sHookedWindow, curChar);
        else sPrevChar = nullptr;
        GLFWkeyfun curKey = glfwSetKeyCallback(sHookedWindow, sPrevKey);
        if (curKey != TextInputGlfwKey) glfwSetKeyCallback(sHookedWindow, curKey);
        else sPrevKey = nullptr;
    }
    sHookedWindow = nullptr;
#else
    if (sInitialized) EM_ASM({ window._miloTextCapture = 0; });
#endif
    sInitialized = false;
    sCapture = false;
    ClearQueue();
    sSwallowNextChar = false;
    memset(sDown, 0, sizeof(sDown));
    memset(sHeldAtCapture, 0, sizeof(sHeldAtCapture));
    sHeldAtCaptureCount = 0;
    memset(sPadHeldOver, 0, sizeof(sPadHeldOver));
}
