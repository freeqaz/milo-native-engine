// DC3 Native Port — Embedded HTTP Debug Server
// Phases 1-4: Health, Telemetry, Screenshot, Settings, DTA Eval,
//             Object Introspection, Input Injection + Automation

#ifdef DC3_HTTP_SERVER

#include "platform/HttpServer.h"
#include "platform/NativeSettings.h"
#include "platform/Rnd_Wgpu.h"
#include "telemetry/GameplayTelemetry.h"
#include "gfx/Screenshot.h"
#include "gfx/GpuDevice.h"
#include "obj/Data.h"
#include "obj/DataFile.h"
#include "obj/DataFunc.h"
#include "obj/Dir.h"
#include "obj/Task.h"
#include "rndobj/Trans.h"
#include "rndobj/Draw.h"
#include "ui/UI.h"

#include <httplib.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <signal.h>
#include <setjmp.h>

extern WgpuRnd* gWgpuRnd;
extern std::map<Symbol, DataFunc*> gDataFuncs;

HttpServer* TheHttpServer = nullptr;

// JSON helpers — hand-rolled to avoid pulling in a JSON library
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

static std::string JsonOk(const std::string& dataJson) {
    return "{\"ok\":true,\"data\":" + dataJson + "}";
}

static std::string JsonError(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(msg) + "\"}";
}

// Phase 4: Button name → bit position mapping (mirrors Joypad_Native.cpp ParseButtonName)
static int ParseHttpButtonName(const char* name) {
    if (!strcmp(name, "confirm") || !strcmp(name, "a"))      return 6;   // kPad_X
    if (!strcmp(name, "cancel")  || !strcmp(name, "b"))      return 5;   // kPad_Circle
    if (!strcmp(name, "start"))                               return 11;  // kPad_Start
    if (!strcmp(name, "option")  || !strcmp(name, "back") ||
        !strcmp(name, "select"))                              return 8;   // kPad_Select
    if (!strcmp(name, "up")      || !strcmp(name, "dpad_up"))    return 12;
    if (!strcmp(name, "down")    || !strcmp(name, "dpad_down"))  return 14;
    if (!strcmp(name, "left")    || !strcmp(name, "dpad_left"))  return 15;
    if (!strcmp(name, "right")   || !strcmp(name, "dpad_right")) return 13;
    if (!strcmp(name, "l1") || !strcmp(name, "lb"))           return 2;
    if (!strcmp(name, "r1") || !strcmp(name, "rb"))           return 3;
    if (!strcmp(name, "l2") || !strcmp(name, "lt"))           return 0;
    if (!strcmp(name, "r2") || !strcmp(name, "rt"))           return 1;
    if (!strcmp(name, "x"))                                   return 7;   // kPad_Square
    if (!strcmp(name, "y"))                                   return 4;   // kPad_Tri
    if (!strcmp(name, "l3") || !strcmp(name, "ls"))           return 9;
    if (!strcmp(name, "r3") || !strcmp(name, "rs"))           return 10;
    return -1;
}

// Phase 3: Build scene tree JSON recursively
static std::string SceneTreeJson(ObjectDir* dir, int depth, int maxDepth) {
    if (!dir) return "null";

    std::string json = "{";
    json += "\"name\":\"" + JsonEscape(dir->Name()) + "\"";
    json += ",\"type\":\"" + JsonEscape(dir->ClassName().Str()) + "\"";
    const char* path = dir->GetPathName();
    if (path && path[0])
        json += ",\"path\":\"" + JsonEscape(path) + "\"";

    // Count objects in this directory
    int count = 0;
    auto* entry = dir->HashTable().Begin();
    while (entry) {
        if (entry->obj) count++;
        entry = dir->HashTable().Next(entry);
    }
    json += ",\"objectCount\":" + std::to_string(count);

    // Recurse into subdirs if within depth limit
    if (depth < maxDepth) {
        const auto& subdirs = dir->SubDirs();
        if (!subdirs.empty()) {
            json += ",\"subdirs\":[";
            bool first = true;
            for (size_t i = 0; i < subdirs.size(); i++) {
                ObjectDir* sub = subdirs[i];
                if (!sub) continue;
                if (!first) json += ",";
                json += SceneTreeJson(sub, depth + 1, maxDepth);
                first = false;
            }
            json += "]";
        }
    }

    json += "}";
    return json;
}

// ---------------------------------------------------------------------------
// HttpServer implementation
// ---------------------------------------------------------------------------

HttpServer::HttpServer() {}

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(int port) {
    if (mRunning) return true;

    mStartTime = TheTaskMgr.Seconds(TaskMgr::kRealTime);
    auto* svr = new httplib::Server();
    mServer = svr;
    RegisterEndpoints();

    // Bind on calling thread so we fail fast if port is taken.
    if (!svr->bind_to_port("0.0.0.0", port)) {
        fprintf(stderr, "[HttpServer] FATAL: port %d already in use\n", port);
        abort();
    }

    mPort = port;
    mRunning = true;
    mServerThread = std::thread(&HttpServer::ServerThread, this);

    // Machine-readable line on stdout for scripts to parse
    fprintf(stdout, "DC3_HTTP_PORT=%d\n", port);
    fflush(stdout);
    fprintf(stderr, "[HttpServer] Listening on port %d\n", port);
    return true;
}

void HttpServer::Stop() {
    if (!mRunning) return;
    mRunning = false;

    // Wake any long-polling threads so they can exit
    mWaitCv.notify_all();

    auto* svr = static_cast<httplib::Server*>(mServer);
    if (svr) svr->stop();

    if (mServerThread.joinable())
        mServerThread.join();

    delete static_cast<httplib::Server*>(mServer);
    mServer = nullptr;

    fprintf(stderr, "[HttpServer] Stopped\n");
}

void HttpServer::ServerThread() {
    auto* svr = static_cast<httplib::Server*>(mServer);
    if (!svr->listen_after_bind()) {
        fprintf(stderr, "[HttpServer] listen_after_bind() failed\n");
        mRunning = false;
    }
}

HttpServer::CommandResult HttpServer::QueueAndWait(
    CommandType type, const std::string& p1, const std::string& p2
) {
    Command cmd;
    cmd.type = type;
    cmd.param1 = p1;
    cmd.param2 = p2;

    // Enqueue
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        if (type == kCmdScreenshot)
            mPendingScreenshots.push_back(&cmd);
        else
            mPendingCommands.push_back(&cmd);
    }

    // Wait for main thread to process it
    {
        std::unique_lock<std::mutex> lk(cmd.mtx);
        cmd.cv.wait_for(lk, std::chrono::seconds(10), [&] { return cmd.done; });
    }

    if (!cmd.done) {
        CommandResult timeout;
        timeout.ok = false;
        timeout.error = "Command timed out (main thread not processing?)";
        return timeout;
    }

    return cmd.result;
}

void HttpServer::ProcessCommands() {
    std::vector<Command*> batch;
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        batch.swap(mPendingCommands);
    }

    for (Command* cmd : batch) {
        switch (cmd->type) {
            case kCmdDtaEval:      HandleDtaEval(*cmd); break;
            case kCmdSetSetting:   HandleSetSetting(*cmd); break;
            case kCmdListObjects:  HandleListObjects(*cmd); break;
            case kCmdGetObject:    HandleGetObject(*cmd); break;
            case kCmdGetChildren:  HandleGetChildren(*cmd); break;
            case kCmdSceneTree:    HandleSceneTree(*cmd); break;
            default: cmd->result.error = "Unknown command type"; break;
        }
        {
            std::lock_guard<std::mutex> lk(cmd->mtx);
            cmd->done = true;
        }
        cmd->cv.notify_one();
    }
}

void HttpServer::ProcessScreenshots() {
    std::vector<Command*> batch;
    {
        std::lock_guard<std::mutex> lk(mQueueMutex);
        batch.swap(mPendingScreenshots);
    }

    for (Command* cmd : batch) {
        HandleScreenshot(*cmd);
        {
            std::lock_guard<std::mutex> lk(cmd->mtx);
            cmd->done = true;
        }
        cmd->cv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Phase 4: Frame notification + input injection
// ---------------------------------------------------------------------------

void HttpServer::NotifyFrame(const char* screenName, int frame) {
    {
        std::lock_guard<std::mutex> lk(mWaitMutex);
        mCurrentScreen = screenName ? screenName : "";
        mCurrentFrame = frame;
    }
    mWaitCv.notify_all();
}

unsigned int HttpServer::ConsumeHttpButtons() {
    std::lock_guard<std::mutex> lk(mInputMutex);

    unsigned int buttons = mImmediateButtons;
    mImmediateButtons = 0;

    // Tick sequence events — fire any that have reached zero
    auto it = mInputQueue.begin();
    while (it != mInputQueue.end()) {
        if (it->framesRemaining <= 0) {
            buttons |= it->buttonBit;
            it = mInputQueue.erase(it);
        } else {
            it->framesRemaining--;
            ++it;
        }
    }

    return buttons;
}

// ---------------------------------------------------------------------------
// Command handlers (run on main thread)
// ---------------------------------------------------------------------------

void HttpServer::HandleScreenshot(Command& cmd) {
    if (!gWgpuRnd) {
        cmd.result.ok = false;
        cmd.result.error = "Renderer not initialized";
        return;
    }

    int w = gWgpuRnd->Gpu().WindowWidth();
    int h = gWgpuRnd->Gpu().WindowHeight();
    size_t pixelSize = (size_t)w * h * 4;
    std::vector<uint8_t> pixels(pixelSize);

    if (!gWgpuRnd->Gpu().ReadbackHeadlessFrame(pixels.data(), pixelSize)) {
        cmd.result.ok = false;
        cmd.result.error = "Framebuffer readback failed (headless mode required)";
        return;
    }

    std::vector<uint8_t> png;
    if (!WritePNGToMemory(png, pixels.data(), w, h)) {
        cmd.result.ok = false;
        cmd.result.error = "PNG encoding failed";
        return;
    }

    cmd.result.ok = true;
    cmd.result.binaryData = std::move(png);
}

// DTA eval crash recovery — sigsetjmp/siglongjmp safety net.
// ParseArray→ParseNode recursion can stack-overflow on deep nesting,
// and Evaluate can segfault on malformed data. We catch both.
static sigjmp_buf sDtaEvalJmpBuf;
static volatile sig_atomic_t sInDtaEval = 0;
static volatile int sDtaEvalSignal = 0;

static void DtaEvalCrashHandler(int sig) {
    if (sInDtaEval) {
        sDtaEvalSignal = sig;
        siglongjmp(sDtaEvalJmpBuf, sig);
    }
    // Not in DTA eval — restore default and re-raise
    signal(sig, SIG_DFL);
    raise(sig);
}

// Pre-validate nesting depth as a fast-reject before hitting the parser.
// The parser itself has a depth guard (kMaxParseDepth=512 in DataFile.cpp),
// but rejecting here gives a cleaner error message and avoids partial parses.
static const int kMaxDtaNesting = 256;

static int MaxNestingDepth(const char* s) {
    int depth = 0, maxDepth = 0;
    bool inString = false;
    for (; *s; s++) {
        if (*s == '"') { inString = !inString; continue; }
        if (inString) continue;
        if (*s == '(' || *s == '{' || *s == '[') {
            if (++depth > maxDepth) maxDepth = depth;
        } else if (*s == ')' || *s == '}' || *s == ']') {
            if (depth > 0) depth--;
        }
    }
    return maxDepth;
}

void HttpServer::HandleDtaEval(Command& cmd) {
    // Pre-validation: reject expressions that would stack-overflow the parser
    int nesting = MaxNestingDepth(cmd.param1.c_str());
    if (nesting > kMaxDtaNesting) {
        cmd.result.ok = false;
        cmd.result.httpStatus = 400;
        cmd.result.error = "DTA expression too deeply nested (" +
            std::to_string(nesting) + " levels, max " +
            std::to_string(kMaxDtaNesting) + ")";
        return;
    }

    // Install signal handlers for crash recovery during eval
    struct sigaction sa_segv, sa_bus, sa_fpe, sa_abrt;
    struct sigaction old_segv, old_bus, old_fpe, old_abrt;
    memset(&sa_segv, 0, sizeof(sa_segv));
    sa_segv.sa_handler = DtaEvalCrashHandler;
    sigemptyset(&sa_segv.sa_mask);
    sa_segv.sa_flags = 0;
    sa_bus = sa_fpe = sa_abrt = sa_segv;

    sigaction(SIGSEGV, &sa_segv, &old_segv);
    sigaction(SIGBUS, &sa_bus, &old_bus);
    sigaction(SIGFPE, &sa_fpe, &old_fpe);
    sigaction(SIGABRT, &sa_abrt, &old_abrt);

    sInDtaEval = 1;
    sDtaEvalSignal = 0;

    if (sigsetjmp(sDtaEvalJmpBuf, 1) != 0) {
        // Recovered from a crash during DTA eval
        sInDtaEval = 0;
        sigaction(SIGSEGV, &old_segv, nullptr);
        sigaction(SIGBUS, &old_bus, nullptr);
        sigaction(SIGFPE, &old_fpe, nullptr);
        sigaction(SIGABRT, &old_abrt, nullptr);

        const char* sigName = "unknown signal";
        switch (sDtaEvalSignal) {
            case SIGSEGV: sigName = "SIGSEGV (null pointer or bad memory access)"; break;
            case SIGBUS:  sigName = "SIGBUS (alignment or bus error)"; break;
            case SIGFPE:  sigName = "SIGFPE (arithmetic error)"; break;
            case SIGABRT: sigName = "SIGABRT (abort)"; break;
        }
        cmd.result.ok = false;
        cmd.result.error = std::string("DTA eval crashed: ") + sigName;
        fprintf(stderr, "[HttpServer] DTA eval recovered from %s for expression: %.200s\n",
                sigName, cmd.param1.c_str());
        return;
    }

    try {
        DataArray* parsed = DataReadString(cmd.param1.c_str());
        if (!parsed || parsed->Size() == 0) {
            cmd.result.ok = false;
            cmd.result.httpStatus = 400;
            cmd.result.error = "Failed to parse DTA expression";
            if (parsed) parsed->Release();
            goto cleanup;
        }

        {
            // DataReadString returns a top-level array containing parsed commands.
            // Evaluate each node — for command sub-arrays, Evaluate calls Execute.
            DataNode result(0);
            for (int i = 0; i < parsed->Size(); i++) {
                result = parsed->Evaluate(i);
            }
            parsed->Release();

            // Serialize DataNode result to JSON based on type
            cmd.result.ok = true;
            switch (result.Type()) {
                case kDataInt:
                    cmd.result.jsonData = "{\"type\":\"int\",\"value\":" +
                        std::to_string(result.UncheckedInt()) + "}";
                    break;
                case kDataFloat:
                    cmd.result.jsonData = "{\"type\":\"float\",\"value\":" +
                        std::to_string(result.UncheckedFloat()) + "}";
                    break;
                case kDataSymbol: {
                    const char* s = result.UncheckedStr();
                    cmd.result.jsonData = "{\"type\":\"symbol\",\"value\":\"" +
                        JsonEscape(s ? s : "") + "\"}";
                    break;
                }
                case kDataObject: {
                    Hmx::Object* obj = result.GetObj(nullptr);
                    const char* name = obj ? obj->Name() : "null";
                    cmd.result.jsonData = "{\"type\":\"object\",\"value\":\"" +
                        JsonEscape(name) + "\"}";
                    break;
                }
                default:
                    cmd.result.jsonData = "{\"type\":" +
                        std::to_string((int)result.Type()) + ",\"value\":null}";
                    break;
            }
        }
    } catch (const std::exception& e) {
        cmd.result.ok = false;
        cmd.result.error = std::string("DTA eval exception: ") + e.what();
    } catch (...) {
        cmd.result.ok = false;
        cmd.result.error = "DTA eval threw unknown exception";
    }

cleanup:
    sInDtaEval = 0;
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    sigaction(SIGFPE, &old_fpe, nullptr);
    sigaction(SIGABRT, &old_abrt, nullptr);
}

void HttpServer::HandleSetSetting(Command& cmd) {
    NativeSettings& s = NativeSettings::Get();
    const std::string& name = cmd.param1;
    const std::string& val = cmd.param2;

    if (name == "cameraBlend") s.cameraBlend = (atoi(val.c_str()) != 0);
    else if (name == "blendFramesSame") s.blendFramesSame = (float)atof(val.c_str());
    else if (name == "blendFramesCross") s.blendFramesCross = (float)atof(val.c_str());
    else if (name == "fovScale") s.fovScale = (float)atof(val.c_str());
    else if (name == "nearPlaneOverride") s.nearPlaneOverride = (float)atof(val.c_str());
    else if (name == "farPlaneOverride") s.farPlaneOverride = (float)atof(val.c_str());
    else if (name == "aspectOverride") s.aspectOverride = (float)atof(val.c_str());
    else if (name == "camForwardOffset") s.camForwardOffset = (float)atof(val.c_str());
    else if (name == "camHeightOffset") s.camHeightOffset = (float)atof(val.c_str());
    else if (name == "camLateralOffset") s.camLateralOffset = (float)atof(val.c_str());
    else if (name == "cameraDebug") s.cameraDebug = (atoi(val.c_str()) != 0);
    else {
        cmd.result.ok = false;
        cmd.result.error = "Unknown setting: " + name;
        return;
    }

    cmd.result.ok = true;
    cmd.result.jsonData = "{\"updated\":\"" + JsonEscape(name) + "\"}";
}

// ---------------------------------------------------------------------------
// Phase 3: Object Introspection handlers (run on main thread)
// ---------------------------------------------------------------------------

void HttpServer::HandleListObjects(Command& cmd) {
    ObjectDir* dir = ObjectDir::Main();
    if (!dir) {
        cmd.result.ok = false;
        cmd.result.error = "No main ObjectDir";
        return;
    }

    // param1 = "true" for recursive, param2 = specific dir name to list
    bool recurse = (cmd.param1 == "true");

    // If a specific directory was requested, find it
    if (!cmd.param2.empty()) {
        Hmx::Object* found = dir->FindObject(cmd.param2.c_str(), false, true);
        ObjectDir* subdir = found ? dynamic_cast<ObjectDir*>(found) : nullptr;
        if (!subdir) {
            cmd.result.ok = false;
            cmd.result.error = "Directory not found: " + cmd.param2;
            return;
        }
        dir = subdir;
    }

    std::string json = "[";
    bool first = true;
    int count = 0;
    const int kMaxObjects = 5000; // safety limit

    if (recurse) {
        ObjDirItr<Hmx::Object> it(dir, true);
        for (; it && count < kMaxObjects; ++it) {
            Hmx::Object* obj = it;
            if (!first) json += ",";
            json += "{\"name\":\"" + JsonEscape(obj->Name()) +
                    "\",\"type\":\"" + JsonEscape(obj->ClassName().Str()) + "\"}";
            first = false;
            count++;
        }
    } else {
        auto* entry = dir->HashTable().Begin();
        while (entry && count < kMaxObjects) {
            if (entry->obj) {
                if (!first) json += ",";
                json += "{\"name\":\"" + JsonEscape(entry->name) +
                        "\",\"type\":\"" + JsonEscape(entry->obj->ClassName().Str()) + "\"}";
                first = false;
                count++;
            }
            entry = dir->HashTable().Next(entry);
        }
    }
    json += "]";

    cmd.result.ok = true;
    cmd.result.jsonData = json;
}

void HttpServer::HandleGetObject(Command& cmd) {
    ObjectDir* mainDir = ObjectDir::Main();
    if (!mainDir) {
        cmd.result.ok = false;
        cmd.result.error = "No main ObjectDir";
        return;
    }

    Hmx::Object* obj = mainDir->FindObject(cmd.param1.c_str(), false, true);
    if (!obj) {
        cmd.result.ok = false;
        cmd.result.error = "Object not found: " + cmd.param1;
        return;
    }

    std::string json = "{";
    json += "\"name\":\"" + JsonEscape(obj->Name()) + "\"";
    json += ",\"type\":\"" + JsonEscape(obj->ClassName().Str()) + "\"";
    if (obj->Dir()) {
        const char* dirPath = obj->Dir()->GetPathName();
        json += ",\"dir\":\"" + JsonEscape(dirPath ? dirPath : "") + "\"";
    }

    // RndTransformable: position
    RndTransformable* trans = dynamic_cast<RndTransformable*>(obj);
    if (trans) {
        const Transform& xfm = trans->LocalXfm();
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f}",
            xfm.v.x, xfm.v.y, xfm.v.z);
        json += ",\"position\":" + std::string(buf);

        // Include parent transform name if any
        RndTransformable* parent = trans->TransParent();
        if (parent)
            json += ",\"transParent\":\"" + JsonEscape(parent->Name()) + "\"";
    }

    // RndDrawable: showing, order, sphere
    RndDrawable* draw = dynamic_cast<RndDrawable*>(obj);
    if (draw) {
        json += std::string(",\"showing\":") + (draw->Showing() ? "true" : "false");
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", draw->GetOrder());
        json += ",\"order\":" + std::string(buf);

        const Sphere& sph = draw->GetSphere();
        if (sph.radius > 0.f) {
            snprintf(buf, sizeof(buf), "{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"r\":%.2f}",
                sph.center.x, sph.center.y, sph.center.z, sph.radius);
            json += ",\"sphere\":" + std::string(buf);
        }
    }

    // ObjectDir: note if it's a directory
    ObjectDir* asDir = dynamic_cast<ObjectDir*>(obj);
    if (asDir) {
        json += ",\"isDir\":true";
        json += ",\"objectCount\":" + std::to_string(asDir->HashTableUsedSize());
        json += ",\"subDirCount\":" + std::to_string((int)asDir->SubDirs().size());
    }

    json += "}";
    cmd.result.ok = true;
    cmd.result.jsonData = json;
}

void HttpServer::HandleGetChildren(Command& cmd) {
    ObjectDir* mainDir = ObjectDir::Main();
    if (!mainDir) {
        cmd.result.ok = false;
        cmd.result.error = "No main ObjectDir";
        return;
    }

    Hmx::Object* obj = mainDir->FindObject(cmd.param1.c_str(), false, true);
    if (!obj) {
        cmd.result.ok = false;
        cmd.result.error = "Object not found: " + cmd.param1;
        return;
    }

    ObjectDir* dir = dynamic_cast<ObjectDir*>(obj);
    if (!dir) {
        cmd.result.ok = false;
        cmd.result.error = "Object is not a directory: " + cmd.param1;
        return;
    }

    std::string json = "{\"objects\":[";
    bool first = true;
    int count = 0;
    auto* entry = dir->HashTable().Begin();
    while (entry && count < 5000) {
        if (entry->obj) {
            if (!first) json += ",";
            json += "{\"name\":\"" + JsonEscape(entry->name) +
                    "\",\"type\":\"" + JsonEscape(entry->obj->ClassName().Str()) + "\"}";
            first = false;
            count++;
        }
        entry = dir->HashTable().Next(entry);
    }
    json += "],\"subdirs\":[";

    first = true;
    for (size_t i = 0; i < dir->SubDirs().size(); i++) {
        ObjectDir* sub = dir->SubDirs()[i];
        if (!sub) continue;
        if (!first) json += ",";
        json += "{\"name\":\"" + JsonEscape(sub->Name()) +
                "\",\"type\":\"" + JsonEscape(sub->ClassName().Str()) +
                "\",\"objectCount\":" + std::to_string(sub->HashTableUsedSize()) + "}";
        first = false;
    }
    json += "]}";

    cmd.result.ok = true;
    cmd.result.jsonData = json;
}

void HttpServer::HandleSceneTree(Command& cmd) {
    ObjectDir* dir = ObjectDir::Main();
    if (!dir) {
        cmd.result.ok = false;
        cmd.result.error = "No main ObjectDir";
        return;
    }

    int maxDepth = 3;
    if (!cmd.param1.empty())
        maxDepth = std::max(1, std::min(10, atoi(cmd.param1.c_str())));

    cmd.result.ok = true;
    cmd.result.jsonData = SceneTreeJson(dir, 0, maxDepth);
}

// ---------------------------------------------------------------------------
// Endpoint registration
// ---------------------------------------------------------------------------

void HttpServer::RegisterEndpoints() {
    auto* svr = static_cast<httplib::Server*>(mServer);

    // CORS middleware
    svr->set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });

    // Exception handler
    svr->set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                   std::exception_ptr ep) {
        std::string msg = "Internal server error";
        try { std::rethrow_exception(ep); }
        catch (std::exception& e) { msg = e.what(); }
        catch (...) {}
        res.status = 500;
        res.set_content(JsonError(msg), "application/json");
    });

    // OPTIONS preflight for CORS
    svr->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // -----------------------------------------------------------------------
    // Phase 1: Health, Telemetry, Screenshot, Settings
    // -----------------------------------------------------------------------

    // GET /api/health
    svr->Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
        int frame = gWgpuRnd ? gWgpuRnd->FrameID() : 0;
        float uptime = TheTaskMgr.Seconds(TaskMgr::kRealTime) - mStartTime;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"status\":\"ok\",\"frame\":%d,\"uptime_s\":%.1f}", frame, uptime);
        res.set_content(JsonOk(buf), "application/json");
    });

    // GET /api/telemetry
    svr->Get("/api/telemetry", [](const httplib::Request&, httplib::Response& res) {
        // Read the last snapshot — this is safe from any thread since it's a value copy
        auto s = GameplayTelemetry::LastSnapshot();
        char buf[4096];
        snprintf(buf, sizeof(buf),
            "{\"frame\":%d,\"state\":\"%s\",\"screen\":\"%s\","
            "\"transitionScreen\":\"%s\",\"uiInTransition\":%s,"
            "\"gameScreenActive\":%s,\"currentHasWorldPanel\":%s,"
            "\"transitionHasWorldPanel\":%s,\"worldPanelLoaded\":%s,"
            "\"gamePanelLoadState\":%d,\"gameWaitState\":%d,\"gameLoadState\":%d,"
            "\"gameUsesMoveGraph\":%s,\"gamePaused\":%s,\"gameRealTime\":%s,"
            "\"beat\":%.2f,\"realSecs\":%.2f,"
            "\"songAnimFrame\":%.1f,\"pollEnabled\":%s,"
            "\"worldLoaded\":%s,\"worldPresent\":%s,\"venuePresent\":%s,"
            "\"typeDef\":\"%s\",\"gameStage\":\"%s\",\"hamProvider\":%s,\"mergerDir\":%s,"
            "\"clipDir\":%s,\"masterClip\":%s,\"clipPlayerInit\":%s,"
            "\"charClipLayers\":%d,\"player0\":%s,\"player1\":%s,"
            "\"clipKeyCount\":%d,\"songAnimKeys\":%d,\"diffProxy\":%d,"
            "\"routineLoaded\":%d,\"mergeMoves\":%d,"
            "\"p0SongAnim\":%d,\"doSongAnim\":%d}",
            s.frame, s.state, s.screen, s.transitionScreen,
            s.uiInTransition ? "true" : "false",
            s.gameScreenActive ? "true" : "false",
            s.currentHasWorldPanel ? "true" : "false",
            s.transitionHasWorldPanel ? "true" : "false",
            s.worldPanelLoaded ? "true" : "false",
            s.gamePanelLoadState, s.gameWaitState, s.gameLoadState,
            s.gameUsesMoveGraph ? "true" : "false",
            s.gamePaused ? "true" : "false",
            s.gameRealTime ? "true" : "false",
            s.beat, s.realSecs,
            s.songAnimFrame, s.pollEnabled ? "true" : "false",
            s.worldLoaded ? "true" : "false",
            s.worldPresent ? "true" : "false",
            s.venuePresent ? "true" : "false",
            s.typeDef, s.gameStage, s.hamProvider ? "true" : "false",
            s.mergerDir ? "true" : "false",
            s.clipDir ? "true" : "false", s.masterClip ? "true" : "false",
            s.clipPlayerInit ? "true" : "false",
            s.charClipLayers, s.player0 ? "true" : "false", s.player1 ? "true" : "false",
            s.clipKeyCount, s.songAnimKeys, s.diffProxy,
            s.routineLoaded, s.mergeMoves,
            s.p0SongAnim, s.doSongAnim);
        res.set_content(JsonOk(buf), "application/json");
    });

    // GET /api/screenshot
    svr->Get("/api/screenshot", [this](const httplib::Request&, httplib::Response& res) {
        auto result = QueueAndWait(kCmdScreenshot);
        if (result.ok) {
            res.set_content(
                reinterpret_cast<const char*>(result.binaryData.data()),
                result.binaryData.size(), "image/png");
        } else {
            res.status = 500;
            res.set_content(JsonError(result.error), "application/json");
        }
    });

    // GET /api/settings
    svr->Get("/api/settings", [](const httplib::Request&, httplib::Response& res) {
        NativeSettings& s = NativeSettings::Get();
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"cameraBlend\":%s,"
            "\"blendFramesSame\":%.1f,\"blendFramesCross\":%.1f,"
            "\"fovScale\":%.3f,"
            "\"nearPlaneOverride\":%.2f,\"farPlaneOverride\":%.2f,"
            "\"aspectOverride\":%.3f,"
            "\"camForwardOffset\":%.2f,\"camHeightOffset\":%.2f,\"camLateralOffset\":%.2f,"
            "\"cameraDebug\":%s}",
            s.cameraBlend ? "true" : "false",
            s.blendFramesSame, s.blendFramesCross,
            s.fovScale,
            s.nearPlaneOverride, s.farPlaneOverride,
            s.aspectOverride,
            s.camForwardOffset, s.camHeightOffset, s.camLateralOffset,
            s.cameraDebug ? "true" : "false");
        res.set_content(JsonOk(buf), "application/json");
    });

    // PUT /api/settings
    svr->Put("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
        // Simple key=value via query params: PUT /api/settings?fovScale=1.2
        for (auto& p : req.params) {
            auto result = QueueAndWait(kCmdSetSetting, p.first, p.second);
            if (!result.ok) {
                res.status = 400;
                res.set_content(JsonError(result.error), "application/json");
                return;
            }
        }
        // Return current settings after update
        NativeSettings& s = NativeSettings::Get();
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"cameraBlend\":%s,"
            "\"blendFramesSame\":%.1f,\"blendFramesCross\":%.1f,"
            "\"fovScale\":%.3f,"
            "\"nearPlaneOverride\":%.2f,\"farPlaneOverride\":%.2f,"
            "\"aspectOverride\":%.3f,"
            "\"camForwardOffset\":%.2f,\"camHeightOffset\":%.2f,\"camLateralOffset\":%.2f,"
            "\"cameraDebug\":%s}",
            s.cameraBlend ? "true" : "false",
            s.blendFramesSame, s.blendFramesCross,
            s.fovScale,
            s.nearPlaneOverride, s.farPlaneOverride,
            s.aspectOverride,
            s.camForwardOffset, s.camHeightOffset, s.camLateralOffset,
            s.cameraDebug ? "true" : "false");
        res.set_content(JsonOk(buf), "application/json");
    });

    // -----------------------------------------------------------------------
    // Phase 2: DTA Script Execution
    // -----------------------------------------------------------------------

    // POST /api/dta/eval — execute DTA expression
    svr->Post("/api/dta/eval", [this](const httplib::Request& req, httplib::Response& res) {
        // Accept either JSON body {"expr":"..."} or raw text body
        std::string expr;
        if (req.has_header("Content-Type") &&
            req.get_header_value("Content-Type").find("application/json") != std::string::npos) {
            // Minimal JSON parse for {"expr":"..."}
            auto pos = req.body.find("\"expr\"");
            if (pos != std::string::npos) {
                auto colon = req.body.find(':', pos);
                auto quote1 = req.body.find('"', colon + 1);
                if (quote1 != std::string::npos) {
                    // Handle escaped quotes in the expression
                    std::string result;
                    for (size_t i = quote1 + 1; i < req.body.size(); i++) {
                        if (req.body[i] == '\\' && i + 1 < req.body.size()) {
                            result += req.body[i + 1];
                            i++;
                        } else if (req.body[i] == '"') {
                            break;
                        } else {
                            result += req.body[i];
                        }
                    }
                    expr = result;
                }
            }
        }
        if (expr.empty()) expr = req.body; // fallback: raw body is the expression

        if (expr.empty()) {
            res.status = 400;
            res.set_content(JsonError("No expression provided"), "application/json");
            return;
        }

        auto result = QueueAndWait(kCmdDtaEval, expr);
        if (result.ok) {
            res.set_content(JsonOk(result.jsonData), "application/json");
        } else {
            res.status = result.httpStatus;
            res.set_content(JsonError(result.error), "application/json");
        }
    });

    // GET /api/dta/funcs — list all registered DataFunc names
    svr->Get("/api/dta/funcs", [](const httplib::Request&, httplib::Response& res) {
        std::string json = "[";
        bool first = true;
        for (auto& kv : gDataFuncs) {
            if (!first) json += ",";
            json += "\"" + JsonEscape(kv.first.Str()) + "\"";
            first = false;
        }
        json += "]";
        res.set_content(JsonOk(json), "application/json");
    });

    // -----------------------------------------------------------------------
    // Phase 3: Object Introspection
    // -----------------------------------------------------------------------

    // GET /api/objects — list all objects in main dir (or specific dir)
    svr->Get("/api/objects", [this](const httplib::Request& req, httplib::Response& res) {
        std::string recurse = req.has_param("recurse") ? req.get_param_value("recurse") : "";
        std::string dir = req.has_param("dir") ? req.get_param_value("dir") : "";
        auto result = QueueAndWait(kCmdListObjects, recurse, dir);
        if (result.ok) {
            res.set_content(JsonOk(result.jsonData), "application/json");
        } else {
            res.status = 404;
            res.set_content(JsonError(result.error), "application/json");
        }
    });

    // GET /api/scene/tree — full scene graph
    svr->Get("/api/scene/tree", [this](const httplib::Request& req, httplib::Response& res) {
        std::string depth = req.has_param("depth") ? req.get_param_value("depth") : "3";
        auto result = QueueAndWait(kCmdSceneTree, depth);
        if (result.ok) {
            res.set_content(JsonOk(result.jsonData), "application/json");
        } else {
            res.status = 500;
            res.set_content(JsonError(result.error), "application/json");
        }
    });

    // GET /api/objects/<name>/children — must be registered BEFORE /api/objects/<name>
    svr->Get(R"(/api/objects/(.+)/children)", [this](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        auto result = QueueAndWait(kCmdGetChildren, name);
        if (result.ok) {
            res.set_content(JsonOk(result.jsonData), "application/json");
        } else {
            res.status = 404;
            res.set_content(JsonError(result.error), "application/json");
        }
    });

    // GET /api/objects/<name> — get object details
    svr->Get(R"(/api/objects/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        auto result = QueueAndWait(kCmdGetObject, name);
        if (result.ok) {
            res.set_content(JsonOk(result.jsonData), "application/json");
        } else {
            res.status = 404;
            res.set_content(JsonError(result.error), "application/json");
        }
    });

    // -----------------------------------------------------------------------
    // Phase 4: Input Injection + Automation
    // -----------------------------------------------------------------------

    // POST /api/input/press — inject a single button press
    svr->Post("/api/input/press", [this](const httplib::Request& req, httplib::Response& res) {
        // Parse button name from JSON body {"button":"confirm"} or query param
        std::string btnName;
        if (!req.body.empty()) {
            // Minimal JSON parse for {"button":"..."}
            auto pos = req.body.find("\"button\"");
            if (pos != std::string::npos) {
                auto colon = req.body.find(':', pos);
                auto q1 = req.body.find('"', colon + 1);
                auto q2 = req.body.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    btnName = req.body.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        if (btnName.empty() && req.has_param("button"))
            btnName = req.get_param_value("button");

        if (btnName.empty()) {
            res.status = 400;
            res.set_content(JsonError("No button specified"), "application/json");
            return;
        }

        // Lowercase for matching
        for (char& c : btnName) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }

        int bit = ParseHttpButtonName(btnName.c_str());
        if (bit < 0) {
            res.status = 400;
            res.set_content(JsonError("Unknown button: " + btnName), "application/json");
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mInputMutex);
            mImmediateButtons |= (1u << bit);
        }

        res.set_content(JsonOk("{\"button\":\"" + JsonEscape(btnName) + "\"}"),
                        "application/json");
    });

    // POST /api/input/sequence — queue a button sequence
    svr->Post("/api/input/sequence", [this](const httplib::Request& req, httplib::Response& res) {
        // Parse JSON array: [{"button":"down","delay":15}, ...]
        std::vector<std::pair<unsigned int, int>> events; // <buttonBit, delay>

        size_t pos = 0;
        while (pos < req.body.size()) {
            // Find "button"
            auto bp = req.body.find("\"button\"", pos);
            if (bp == std::string::npos) break;

            auto colon = req.body.find(':', bp + 8);
            auto q1 = req.body.find('"', colon + 1);
            auto q2 = req.body.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) break;

            std::string btnName = req.body.substr(q1 + 1, q2 - q1 - 1);
            for (char& c : btnName) {
                if (c >= 'A' && c <= 'Z') c += 32;
            }

            // Find "delay" within the same object (before next '}')
            int delay = 1;
            auto objEnd = req.body.find('}', q2);
            auto dp = req.body.find("\"delay\"", q2);
            if (dp != std::string::npos && (objEnd == std::string::npos || dp < objEnd)) {
                auto dc = req.body.find(':', dp + 7);
                if (dc != std::string::npos)
                    delay = atoi(req.body.c_str() + dc + 1);
            }

            int bit = ParseHttpButtonName(btnName.c_str());
            if (bit < 0) {
                res.status = 400;
                res.set_content(JsonError("Unknown button: " + btnName), "application/json");
                return;
            }

            events.push_back({1u << bit, delay});
            pos = (objEnd != std::string::npos) ? objEnd + 1 : req.body.size();
        }

        if (events.empty()) {
            res.status = 400;
            res.set_content(JsonError("No valid events in sequence"), "application/json");
            return;
        }

        // Convert relative delays to cumulative frame offsets and enqueue
        {
            std::lock_guard<std::mutex> lk(mInputMutex);
            int accumFrames = 0;
            for (auto& [btnBit, delay] : events) {
                accumFrames += delay;
                mInputQueue.push_back({btnBit, accumFrames});
            }
        }

        res.set_content(JsonOk("{\"queued\":" + std::to_string(events.size()) + "}"),
                        "application/json");
    });

    // GET /api/screen — current UI screen name
    svr->Get("/api/screen", [this](const httplib::Request&, httplib::Response& res) {
        std::string screen;
        {
            std::lock_guard<std::mutex> lk(mWaitMutex);
            screen = mCurrentScreen;
        }
        res.set_content(
            JsonOk("{\"screen\":\"" + JsonEscape(screen) + "\"}"),
            "application/json");
    });

    // GET /api/screen/wait/<name> — long-poll until screen matches
    svr->Get(R"(/api/screen/wait/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto target = req.matches[1].str();
        int timeout = 60;
        if (req.has_param("timeout")) {
            timeout = atoi(req.get_param_value("timeout").c_str());
            timeout = std::max(1, std::min(timeout, 60));
        }

        std::unique_lock<std::mutex> lk(mWaitMutex);
        bool ok = mWaitCv.wait_for(lk, std::chrono::seconds(timeout),
            [&] { return !mRunning || mCurrentScreen == target; });

        if (!mRunning) {
            res.status = 503;
            res.set_content(JsonError("Server shutting down"), "application/json");
        } else if (ok) {
            res.set_content(
                JsonOk("{\"screen\":\"" + JsonEscape(mCurrentScreen) + "\"}"),
                "application/json");
        } else {
            res.status = 408;
            std::string curScreen;
            curScreen = mCurrentScreen;
            res.set_content(
                JsonError("Timeout waiting for screen '" + target +
                          "' (current: '" + curScreen + "')"),
                "application/json");
        }
    });

    // GET /api/frame — current frame number
    svr->Get("/api/frame", [this](const httplib::Request&, httplib::Response& res) {
        int frame;
        {
            std::lock_guard<std::mutex> lk(mWaitMutex);
            frame = mCurrentFrame;
        }
        res.set_content(
            JsonOk("{\"frame\":" + std::to_string(frame) + "}"),
            "application/json");
    });

    // GET /api/frame/wait/<n> — long-poll until frame N reached
    svr->Get(R"(/api/frame/wait/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        int target = atoi(req.matches[1].str().c_str());
        int timeout = 60;
        if (req.has_param("timeout")) {
            timeout = atoi(req.get_param_value("timeout").c_str());
            timeout = std::max(1, std::min(timeout, 60));
        }

        std::unique_lock<std::mutex> lk(mWaitMutex);
        bool ok = mWaitCv.wait_for(lk, std::chrono::seconds(timeout),
            [&] { return !mRunning || mCurrentFrame >= target; });

        if (!mRunning) {
            res.status = 503;
            res.set_content(JsonError("Server shutting down"), "application/json");
        } else if (ok) {
            res.set_content(
                JsonOk("{\"frame\":" + std::to_string(mCurrentFrame) + "}"),
                "application/json");
        } else {
            res.status = 408;
            res.set_content(
                JsonError("Timeout waiting for frame " + std::to_string(target) +
                          " (current: " + std::to_string(mCurrentFrame) + ")"),
                "application/json");
        }
    });
}

// ---------------------------------------------------------------------------
// Init/shutdown helper (called from App.cpp)
// ---------------------------------------------------------------------------

void HttpServerInit() {
    const char* env = getenv("DC3_HTTP");
    if (!env || atoi(env) == 0) return;

    int port = 9090;
    const char* portEnv = getenv("DC3_HTTP_PORT");
    if (portEnv) port = atoi(portEnv);
    if (port <= 0) port = 9090;

    TheHttpServer = new HttpServer();
    TheHttpServer->Start(port);
}

void HttpServerShutdown() {
    if (TheHttpServer) {
        TheHttpServer->Stop();
        delete TheHttpServer;
        TheHttpServer = nullptr;
    }
}

#endif // DC3_HTTP_SERVER
