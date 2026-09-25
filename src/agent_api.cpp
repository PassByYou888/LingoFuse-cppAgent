/**
 * @file agent_api.cpp
 * @brief LingoFuse arithmetic tool provider (C++ ABI port).
 *
 * This program is the C++ counterpart of pascal_agent_api.lpr. It
 * registers as an App on the C4 mesh and exposes four Call APIs:
 *
 *   add  - Add two integers: a + b
 *   sub  - Subtract two integers: a - b
 *   mul  - Multiply two integers: a * b
 *   div  - Divide two integers (floating result): a / b
 *
 * On startup, after connecting to the beacon, it registers each of
 * those APIs as a tool through the beacon's register_agent Call API.
 *
 * All LingoFuse interaction goes through the raw C ABI declared in
 * LingoFuse.h. No C++ RAII wrappers from LingoFuse.hpp are used.
 *
 * All comments and log messages are in English.
 */

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "LingoFuse.h"
#include "json.hpp"

using json = nlohmann::json;

// ============================================================================
// Configuration
// ============================================================================

static const char* MY_APP_NAME    = "my_calculator";
static const char* MY_APP_DESC    =
    "Calculator service providing arithmetic tools";
static const char* IPC_ENDPOINT   = "ipc:agent";
static const char* BEACON_APP     = "agent_main_app";
static const char* REGISTER_API   = "register_agent";
static const char* AGENT_LOG_API  = "agent_log";

static const bool DEBUG_LOG = true;

// Set to true just before cleanup begins. Any in-flight logging thread
// checks this flag before issuing an RPC.
static std::atomic<bool> g_shutting_down{ false };

// ============================================================================
// Status output
// ============================================================================

static void status(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// ============================================================================
// Payload I/O helpers
// ============================================================================

static std::string readPayload(TDataHnd input) {
    const int64_t size = LF_GetSize(input);
    if (size <= 0) {
        return std::string();
    }
    std::vector<char> buf(static_cast<std::size_t>(size));
    LF_SetPos(input, 0);
    const int64_t n = LF_ReadStringBytes(
        input, buf.data(), static_cast<int64_t>(buf.size()));
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

static void writePayload(TDataHnd output, const std::string& text) {
    LF_WriteStringBytes(
        output,
        text.empty() ? "" : text.data(),
        static_cast<int64_t>(text.size()));
}

static std::string jsonDump(const json& j) {
    return j.dump(-1, ' ', false);
}

static void writeError(TDataHnd output, const std::string& err) {
    json j;
    j["error"] = err;
    writePayload(output, jsonDump(j));
}

// ============================================================================
// Asynchronous logging
// ============================================================================
//
// A log message is forwarded to the beacon's agent_log Call API. The
// RPC must NOT run inside the LingoFuse notification thread (doing so
// would deadlock), so a detached thread performs it.
//
// The g_shutting_down flag is checked twice: once before spawning the
// thread, and once inside the thread after it starts. This narrows the
// window during which a logging RPC could race with library shutdown.

static void sendLogAsync(const std::string& msg) {
    if (g_shutting_down.load()) {
        return;
    }

    std::thread([msg]() {
        if (g_shutting_down.load()) {
            return;
        }

        TDataHnd data = LF_CreateData(AGENT_LOG_API);
        if (!data) {
            return;
        }

        json j;
        j["message"] = msg;
        const std::string text = jsonDump(j);
        LF_WriteStringBytes(data, text.data(),
                            static_cast<int64_t>(text.size()));

        TDataHnd result = LF_Call(BEACON_APP, data, 3000);
        if (result) {
            LF_FreeData(result);
        }
        LF_FreeData(data);
    }).detach();
}

// ============================================================================
// Argument parsing for the arithmetic tools
// ============================================================================

/**
 * @brief Extract integer operands a and b from a JSON request.
 *
 * Returns true on success. On failure, `err` describes the problem in
 * English. The accepted request shape is {"a": <int>, "b": <int>}.
 */
static bool parseAB(const std::string& payload, int& a, int& b,
                    std::string& err) {
    if (payload.empty()) {
        err = "Empty input";
        return false;
    }

    json j;
    try {
        j = json::parse(payload);
    } catch (...) {
        err = "Invalid JSON";
        return false;
    }

    if (!j.is_object() || j.count("a") == 0 || j.count("b") == 0) {
        err = "Missing \"a\" or \"b\"";
        return false;
    }

    try {
        a = j["a"].get<int>();
        b = j["b"].get<int>();
    } catch (...) {
        err = "Invalid type for \"a\" or \"b\"";
        return false;
    }
    return true;
}

// ============================================================================
// Call API: add
// ============================================================================

static void LF_CDECL do_add(void* trigger, void* input, void* output) {
    (void)trigger;
    TDataHnd out_hnd = static_cast<TDataHnd>(output);

    int a = 0, b = 0;
    std::string err;
    if (!parseAB(readPayload(static_cast<TDataHnd>(input)), a, b, err)) {
        writeError(out_hnd, err);
        if (DEBUG_LOG) {
            status("[add] Error: %s", err.c_str());
            sendLogAsync(std::string("[add] Error: ") + err);
        }
        return;
    }

    const int sum = a + b;
    json resp;
    resp["result"] = sum;
    writePayload(out_hnd, jsonDump(resp));

    if (DEBUG_LOG) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[add] %d + %d = %d", a, b, sum);
        status("%s", buf);
        sendLogAsync(buf);
    }
}

// ============================================================================
// Call API: sub
// ============================================================================

static void LF_CDECL do_sub(void* trigger, void* input, void* output) {
    (void)trigger;
    TDataHnd out_hnd = static_cast<TDataHnd>(output);

    int a = 0, b = 0;
    std::string err;
    if (!parseAB(readPayload(static_cast<TDataHnd>(input)), a, b, err)) {
        writeError(out_hnd, err);
        if (DEBUG_LOG) {
            status("[sub] Error: %s", err.c_str());
            sendLogAsync(std::string("[sub] Error: ") + err);
        }
        return;
    }

    const int diff = a - b;
    json resp;
    resp["result"] = diff;
    writePayload(out_hnd, jsonDump(resp));

    if (DEBUG_LOG) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[sub] %d - %d = %d", a, b, diff);
        status("%s", buf);
        sendLogAsync(buf);
    }
}

// ============================================================================
// Call API: mul
// ============================================================================

static void LF_CDECL do_mul(void* trigger, void* input, void* output) {
    (void)trigger;
    TDataHnd out_hnd = static_cast<TDataHnd>(output);

    int a = 0, b = 0;
    std::string err;
    if (!parseAB(readPayload(static_cast<TDataHnd>(input)), a, b, err)) {
        writeError(out_hnd, err);
        if (DEBUG_LOG) {
            status("[mul] Error: %s", err.c_str());
            sendLogAsync(std::string("[mul] Error: ") + err);
        }
        return;
    }

    const int prod = a * b;
    json resp;
    resp["result"] = prod;
    writePayload(out_hnd, jsonDump(resp));

    if (DEBUG_LOG) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[mul] %d * %d = %d", a, b, prod);
        status("%s", buf);
        sendLogAsync(buf);
    }
}

// ============================================================================
// Call API: div
// ============================================================================

static void LF_CDECL do_div(void* trigger, void* input, void* output) {
    (void)trigger;
    TDataHnd out_hnd = static_cast<TDataHnd>(output);

    int a = 0, b = 0;
    std::string err;
    if (!parseAB(readPayload(static_cast<TDataHnd>(input)), a, b, err)) {
        writeError(out_hnd, err);
        if (DEBUG_LOG) {
            status("[div] Error: %s", err.c_str());
            sendLogAsync(std::string("[div] Error: ") + err);
        }
        return;
    }

    if (b == 0) {
        const std::string err2 = "Division by zero";
        writeError(out_hnd, err2);
        if (DEBUG_LOG) {
            status("[div] Error: %s", err2.c_str());
            sendLogAsync(std::string("[div] Error: ") + err2);
        }
        return;
    }

    const double quot = static_cast<double>(a) / static_cast<double>(b);
    json resp;
    resp["result"] = quot;
    writePayload(out_hnd, jsonDump(resp));

    if (DEBUG_LOG) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[div] %d / %d = %.2f",
                      a, b, quot);
        status("%s", buf);
        sendLogAsync(buf);
    }
}

// ============================================================================
// Tool registration with the beacon
// ============================================================================

/**
 * @brief Send one tool definition to the beacon's register_agent API.
 *
 * Returns true only when the beacon replies with {"status":"ok"}.
 * All failures are logged. This function never raises.
 */
static bool registerTool(const json& toolDef) {
    TDataHnd data = LF_CreateData(REGISTER_API);
    if (!data) {
        status("[Register] Error: LF_CreateData failed");
        return false;
    }

    const std::string text = jsonDump(toolDef);
    LF_WriteStringBytes(data, text.data(),
                        static_cast<int64_t>(text.size()));

    TDataHnd result = LF_Call(BEACON_APP, data, 5000);
    LF_FreeData(data);

    if (!result) {
        status("[Register] Error: LF_Call returned null");
        return false;
    }

    const std::string respText = readPayload(result);
    LF_FreeData(result);

    if (respText.empty()) {
        status("[Register] Error: empty response");
        return false;
    }

    try {
        const json resp = json::parse(respText);
        if (resp.is_object() &&
            resp.value("status", std::string()) == "ok") {
            return true;
        }
        status("[Register] Failed: %s",
               resp.value("message", std::string("unknown")).c_str());
    } catch (...) {
        status("[Register] Failed: invalid response");
    }
    return false;
}

/**
 * @brief Build a JSON tool definition for one arithmetic API.
 */
static json buildToolDef(const std::string& name,
                         const std::string& description,
                         const std::string& param_a_desc,
                         const std::string& param_b_desc) {
    json def;
    def["name"]        = name;
    def["description"] = description;
    def["target_app"]  = MY_APP_NAME;
    def["target_api"]  = name;

    json params;
    params["type"] = "object";
    params["properties"]["a"]["type"]        = "integer";
    params["properties"]["a"]["description"] = param_a_desc;
    params["properties"]["b"]["type"]        = "integer";
    params["properties"]["b"]["description"] = param_b_desc;
    params["required"] = json::array({ "a", "b" });

    def["parameters"] = std::move(params);
    return def;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    status("[MAIN] LingoFuse calculator tool provider starting");
    LF_LoadLibrary();

    // ---- Create the App and register the four Call APIs -------------
    TAppHnd app = LF_CreateApp(MY_APP_NAME, MY_APP_DESC);
    if (!app) {
        status("[MAIN] FATAL: LF_CreateApp returned null");
        return 1;
    }

    bool ok = true;
    ok = ok && (LF_RegisterCall(app, "add", "Add two integers",
                                nullptr, &do_add) == 1);
    ok = ok && (LF_RegisterCall(app, "sub", "Subtract two integers",
                                nullptr, &do_sub) == 1);
    ok = ok && (LF_RegisterCall(app, "mul", "Multiply two integers",
                                nullptr, &do_mul) == 1);
    ok = ok && (LF_RegisterCall(app, "div",
                                "Divide two integers (floating result)",
                                nullptr, &do_div) == 1);
    if (!ok) {
        status("[MAIN] FATAL: Failed to register one or more APIs");
        LF_FreeApp(app);
        return 1;
    }
    status("[MAIN] Registered APIs: add, sub, mul, div");

    // ---- Connect to the beacon --------------------------------------
    LF_SetOption("WaitConnect", "True");
    LF_ResetPrepare();
    LF_PrepareClient(IPC_ENDPOINT, app);

    if (LF_PrepareDone() != 1) {
        status("[MAIN] FATAL: Failed to connect to beacon at %s",
               IPC_ENDPOINT);
        LF_Shutdown();
        LF_FreeApp(app);
        return 1;
    }
    status("[MAIN] Connected to beacon; registering tools...");

    // ---- Register each arithmetic API with the beacon ---------------
    {
        const json add = buildToolDef(
            "add", "Add two integers: a + b",
            "First operand", "Second operand");
        if (registerTool(add)) {
            status("[OK] Registered tool: add");
        } else {
            status("[FAIL] Failed to register add");
        }
    }
    {
        const json sub = buildToolDef(
            "sub", "Subtract two integers: a - b",
            "First operand", "Second operand");
        if (registerTool(sub)) {
            status("[OK] Registered tool: sub");
        } else {
            status("[FAIL] Failed to register sub");
        }
    }
    {
        const json mul = buildToolDef(
            "mul", "Multiply two integers: a * b",
            "First operand", "Second operand");
        if (registerTool(mul)) {
            status("[OK] Registered tool: mul");
        } else {
            status("[FAIL] Failed to register mul");
        }
    }
    {
        const json div = buildToolDef(
            "div", "Divide two integers (floating result): a / b",
            "Dividend", "Divisor (must be non-zero)");
        if (registerTool(div)) {
            status("[OK] Registered tool: div");
        } else {
            status("[FAIL] Failed to register div");
        }
    }

    status("[MAIN] All tools registered. Type \"exit\" to quit.");

    // ---- Main loop: read lines from stdin until "exit" --------------
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit") {
            break;
        }
    }

    // ---- Cleanup ----------------------------------------------------
    // Set the shutdown flag first, so that any in-flight logging
    // thread bails out before the library is torn down.
    status("[MAIN] Shutting down...");
    g_shutting_down.store(true);
    LF_ExitMainThread();
    LF_FreeApp(app);
    LF_Shutdown();

    status("[MAIN] Cleanup complete.");
    return 0;
}