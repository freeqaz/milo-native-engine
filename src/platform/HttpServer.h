// DC3 Native Port — Embedded HTTP Debug Server
// Background thread serving REST endpoints for engine introspection.
// Desktop-only (no Emscripten). Guarded by DC3_HTTP_SERVER define.

#pragma once

#ifdef DC3_HTTP_SERVER

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstdint>

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    // Start the server on the given port. Returns true if started.
    bool Start(int port = 9090);

    // Stop the server and join the background thread.
    void Stop();

    bool IsRunning() const { return mRunning; }
    int Port() const { return mPort; }

    // --- Command queue (main thread processes these each frame) ---

    enum CommandType {
        kCmdScreenshot,     // Capture framebuffer as PNG
        kCmdDtaEval,        // Evaluate a DTA expression
        kCmdSetSetting,     // Modify a NativeSettings field
        // Phase 3: Object Introspection
        kCmdListObjects,    // List objects in a directory
        kCmdGetObject,      // Get object details by name
        kCmdGetChildren,    // List children of a dir-type object
        kCmdSceneTree,      // Full scene graph as nested JSON
    };

    struct CommandResult {
        bool ok = false;
        int httpStatus = 500;           // HTTP status for errors (400=client, 500=server)
        std::string error;
        std::string jsonData;           // For JSON responses
        std::vector<uint8_t> binaryData; // For screenshot PNG
    };

    struct Command {
        CommandType type;
        std::string param1;     // DTA expression, setting name, object path, etc.
        std::string param2;     // Setting value, query params, etc.

        // Signaling: HTTP handler waits on this
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        CommandResult result;
    };

    // Called from main thread each frame — processes pending commands
    void ProcessCommands();

    // Called from main thread after EndDrawing — processes screenshot requests
    void ProcessScreenshots();

    // Phase 4: Notify current screen + frame from main loop (called each frame)
    void NotifyFrame(const char* screenName, int frame);

    // Phase 4: Get HTTP-injected button bits (called by JoypadPoll on main thread)
    unsigned int ConsumeHttpButtons();

private:
    void ServerThread();
    void RegisterEndpoints();

    // Queue a command from an HTTP handler thread, wait for main thread to process it
    CommandResult QueueAndWait(CommandType type, const std::string& p1 = "",
                               const std::string& p2 = "");

    // Handlers (called on main thread via command queue)
    void HandleScreenshot(Command& cmd);
    void HandleDtaEval(Command& cmd);
    void HandleSetSetting(Command& cmd);
    // Phase 3
    void HandleListObjects(Command& cmd);
    void HandleGetObject(Command& cmd);
    void HandleGetChildren(Command& cmd);
    void HandleSceneTree(Command& cmd);

    volatile bool mRunning = false;
    int mPort = 0;
    std::thread mServerThread;

    // Command queue — HTTP threads push, main thread pops
    std::mutex mQueueMutex;
    std::vector<Command*> mPendingCommands;
    std::vector<Command*> mPendingScreenshots; // processed after EndDrawing

    // Opaque pointer to httplib::Server (avoid header in .h)
    void* mServer = nullptr;

    // Uptime tracking
    float mStartTime = 0;

    // Phase 4: Input injection
    struct InputEvent {
        unsigned int buttonBit;
        int framesRemaining; // frames until this event fires
    };
    std::mutex mInputMutex;
    unsigned int mImmediateButtons = 0;     // buttons to inject this frame
    std::vector<InputEvent> mInputQueue;    // sequence events counting down

    // Phase 4: Screen/frame state for long-poll waits
    std::mutex mWaitMutex;
    std::condition_variable mWaitCv;
    std::string mCurrentScreen;
    int mCurrentFrame = 0;
};

extern HttpServer* TheHttpServer;

#endif // DC3_HTTP_SERVER
