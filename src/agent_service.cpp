/**
 * @file agent_service.cpp
 * @brief LingoFuse agent beacon / main service (C++ ABI port).
 *
 * This program is the C++ counterpart of pascal_agent_service.lpr.
 * It is a LingoFuse App that registers on the C4 mesh and exposes
 * three Call APIs:
 *
 *   agent_log       - Forward a log message to the console.
 *   agent_main      - Return the list of available tools as JSON.
 *   register_agent  - Add or replace a dynamic tool definition.
 *
 * The service listens on both ipc:agent and 0.0.0.0:9897, so local
 * and remote clients can reach it. It also prepares a local IPC
 * client, so the App is discoverable on the mesh.
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
#include <mutex>
#include <string>
#include <vector>

#include "LingoFuse.h"
#include "json.hpp"

using json = nlohmann::json;

// ============================================================================
// Configuration
// ============================================================================

static const char* APP_NAME        = "agent_main_app";
static const char* APP_DESC        = "agent main application";
static const char* IPC_ENDPOINT    = "ipc:agent";
static const char* TCP_LISTEN_ADDR = "0.0.0.0:9897";
static const char* TCP_PUBLIC_ADDR = "127.0.0.1:9897";

// ============================================================================
// Status output
// ============================================================================
//
// All diagnostic output goes to stderr so that it never interferes
// with any stdout-based consumer of this program.

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
//
// Every DataHandle payload exchanged with a client goes through these
// two helpers. They follow the LingoFuse wire convention:
//   - On write, a trailing NUL byte is appended.
//   - On read, the payload is terminated by the first NUL or by the
//     end of the buffer, whichever comes first.

/**
 * @brief Read the full payload from an input DataHandle as a string.
 *
 * Returns an empty string when the payload is empty or the read
 * fails. The returned string is a copy of the bytes actually read;
 * it is never NUL-terminated on the wire.
 */
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

/**
 * @brief Write a text payload to an output DataHandle.
 *
 * LF_WriteStringBytes appends the NUL terminator required by the
 * wire protocol.
 */
static void writePayload(TDataHnd output, const std::string& text) {
    LF_WriteStringBytes(
        output,
        text.empty() ? "" : text.data(),
        static_cast<int64_t>(text.size()));
}

// ============================================================================
// JSON helpers
// ============================================================================
//
// Every JSON document emitted by this program is serialized with
// ensure_ascii = false, matching the LingoFuse toolchain-wide policy.
// No \uXXXX escape is ever produced for a character that can be
// emitted literally in UTF-8.

static std::string jsonDump(const json& j) {
    return j.dump(-1, ' ', false);
}

static void writeError(TDataHnd output, const std::string& err) {
    json j;
    j["status"] = "error";
    j["message"] = err;
    writePayload(output, jsonDump(j));
}

// ============================================================================
// Registered tool storage
// ============================================================================
//
// The beacon keeps a flat list of tool definitions. Each entry is an
// independent copy of the JSON object received from a register_agent
// call. The list is protected by a mutex because register_agent may
// be invoked concurrently with agent_main.

struct ToolEntry {
    std::string name;
    std::string description;
    std::string target_app;
    std::string target_api;
    json parameters;
};

static std::mutex g_agents_mutex;
static std::vector<ToolEntry> g_agents;

// ============================================================================
// Call API: agent_log
// ============================================================================
//
// Reads a JSON request, extracts the "message" field, prints it to
// the console, and returns {"status":"ok"}. If the payload is not
// JSON, the raw text is used as the log message.

static void LF_CDECL do_agent_log(void* trigger, void* input, void* output) {
    (void)trigger;

    const std::string payload = readPayload(static_cast<TDataHnd>(input));

    std::string msg;
    if (!payload.empty()) {
        try {
            const json j = json::parse(payload);
            if (j.is_object() && j.count("message") > 0) {
                msg = j.value("message", std::string());
            } else {
                msg = payload;
            }
        } catch (...) {
            msg = payload;
        }
    }

    if (!msg.empty()) {
        status("[agent_log] %s", msg.c_str());
    }

    json resp;
    resp["status"] = "ok";
    writePayload(static_cast<TDataHnd>(output), jsonDump(resp));
}

// ============================================================================
// Call API: agent_main
// ============================================================================
//
// Returns {"tools":[...]} with the built-in agent_log entry plus every
// registered tool whose target API is currently reachable on the mesh.

static void LF_CDECL do_agent_main(void* trigger, void* input, void* output) {
    (void)trigger;
    (void)input;

    json tools = json::array();

    // ---- Built-in tool: agent_log ----
    {
        json entry;
        entry["name"]        = "agent_log";
        entry["description"] = "Send log messages to the backend";
        entry["target_app"]  = APP_NAME;
        entry["target_api"]  = "agent_log";

        json params;
        params["type"] = "object";
        params["properties"]["message"]["type"]        = "string";
        params["properties"]["message"]["description"] =
            "Log message content";
        params["required"] = json::array({ "message" });

        entry["parameters"] = std::move(params);
        tools.push_back(std::move(entry));
    }

    int tool_count = 1;

    // ---- Dynamically registered tools ----
    {
        std::lock_guard<std::mutex> lk(g_agents_mutex);
        for (const auto& e : g_agents) {
            // Only advertise tools whose target API is reachable.
            if (LF_CheckApi(e.target_app.c_str(),
                            e.target_api.c_str()) == 0) {
                status("[agent_main] Skipped dynamic tool \"%s\": "
                       "API %s.%s not available",
                       e.name.c_str(),
                       e.target_app.c_str(),
                       e.target_api.c_str());
                continue;
            }

            json entry;
            entry["name"]        = e.name;
            entry["description"] = e.description;
            entry["target_app"]  = e.target_app;
            entry["target_api"]  = e.target_api;
            entry["parameters"]  = e.parameters;
            tools.push_back(std::move(entry));

            status("[agent_main] Included dynamic tool: %s "
                   "(API %s.%s available)",
                   e.name.c_str(),
                   e.target_app.c_str(),
                   e.target_api.c_str());
            ++tool_count;
        }
    }

    json resp;
    resp["tools"] = std::move(tools);
    writePayload(static_cast<TDataHnd>(output), jsonDump(resp));

    status("[agent_main] Tool list sent (%d tools)", tool_count);
}

// ============================================================================
// Call API: register_agent
// ============================================================================
//
// Validates the request fields and adds or replaces a tool entry.
// Required fields: name, description, target_app, target_api,
// parameters. Missing fields produce {"status":"error","message":...}.

static void LF_CDECL do_register_agent(void* trigger, void* input,
                                        void* output) {
    (void)trigger;
    TDataHnd out_hnd = static_cast<TDataHnd>(output);

    const std::string payload = readPayload(static_cast<TDataHnd>(input));

    json req;
    try {
        req = json::parse(payload);
    } catch (...) {
        status("[register_agent] Error: Invalid JSON payload");
        writeError(out_hnd, "Invalid JSON payload");
        return;
    }

    if (!req.is_object()) {
        status("[register_agent] Error: Request is not a JSON object");
        writeError(out_hnd, "Request is not a JSON object");
        return;
    }

    static const char* const kRequired[] = {
        "name", "description", "target_app", "target_api", "parameters"
    };
    for (const char* field : kRequired) {
        if (req.count(field) == 0) {
            const std::string err =
                std::string("Missing \"") + field + "\" field";
            status("[register_agent] Error: %s", err.c_str());
            writeError(out_hnd, err);
            return;
        }
    }

    ToolEntry entry;
    entry.name        = req.value("name", std::string());
    entry.description = req.value("description", std::string());
    entry.target_app  = req.value("target_app", std::string());
    entry.target_api  = req.value("target_api", std::string());
    entry.parameters  = req["parameters"];

    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_agents_mutex);
        for (auto& e : g_agents) {
            if (e.name == entry.name) {
                e = entry;
                found = true;
                break;
            }
        }
        if (!found) {
            g_agents.push_back(std::move(entry));
        }
    }

    status("[register_agent] %s tool: %s \"%s\"",
           found ? "Updated existing" : "Added new",
           req.value("name", std::string("?")).c_str(),
           req.value("description", std::string("")).c_str());

    json resp;
    resp["status"] = "ok";
    writePayload(out_hnd, jsonDump(resp));
}

// ============================================================================
// Main
// ============================================================================

int main() {
    status("[MAIN] LingoFuse agent service starting");
    LF_LoadLibrary();

    // ---- Create the App and register the three Call APIs ------------
    TAppHnd app = LF_CreateApp(APP_NAME, APP_DESC);
    if (!app) {
        status("[MAIN] FATAL: LF_CreateApp returned null");
        return 1;
    }
    status("[MAIN] Application \"%s\" created", APP_NAME);

    bool ok = true;
    ok = ok && (LF_RegisterCall(app, "agent_log", "Logging tool",
                                nullptr, &do_agent_log) == 1);
    ok = ok && (LF_RegisterCall(app, "agent_main", "Tool list entry",
                                nullptr, &do_agent_main) == 1);
    ok = ok && (LF_RegisterCall(app, "register_agent",
                                "Dynamic tool registration",
                                nullptr, &do_register_agent) == 1);
    if (!ok) {
        status("[MAIN] FATAL: Failed to register one or more APIs");
        LF_FreeApp(app);
        return 1;
    }
    status("[MAIN] Registered APIs: agent_log, agent_main, register_agent");

    // ---- Configure the mesh and prepare the service -----------------
    LF_SetOption("Wait_Ready", "False");
    status("[MAIN] Set Wait_Ready=False (deployment mode)");

    LF_ResetPrepare();

    status("[MAIN] Preparing IPC service on %s", IPC_ENDPOINT);
    LF_PrepareService(IPC_ENDPOINT, IPC_ENDPOINT);

    status("[MAIN] Preparing TCP service on %s (public %s)",
           TCP_LISTEN_ADDR, TCP_PUBLIC_ADDR);
    LF_PrepareService(TCP_LISTEN_ADDR, TCP_PUBLIC_ADDR);

    status("[MAIN] Connecting local IPC client to %s", IPC_ENDPOINT);
    LF_PrepareClient(IPC_ENDPOINT, app);

    status("[MAIN] Starting framework...");
    if (LF_PrepareDone() != 1) {
        status("[MAIN] FATAL: LF_PrepareDone failed");
        LF_Shutdown();
        LF_FreeApp(app);
        return 1;
    }
    status("[MAIN] Framework started successfully");

    // ---- Main loop: read lines from stdin until "exit" --------------
    status("[MAIN] Service is running. Type \"exit\" to quit.");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit") {
            break;
        }
    }

    // ---- Cleanup ----------------------------------------------------
    // Order matches the documented contract:
    //   1. LF_ExitMainThread  - stop the network loop
    //   2. LF_FreeApp         - detach the App and stop its threads
    //   3. LF_Shutdown        - unload the library and free the pool
    status("[MAIN] Shutting down...");
    LF_ExitMainThread();
    LF_FreeApp(app);
    LF_Shutdown();

    status("[MAIN] Cleanup complete.");
    return 0;
}