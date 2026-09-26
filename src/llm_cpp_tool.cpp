/**
 * @file llm_cpp_tool.cpp
 * @brief Interactive / one-shot command-line client for LingoFuse LLM
 *        services.
 *
 * Architecture: main-thread event loop
 * -------------------------------------
 * The program has exactly one main thread that owns the console. It
 * runs a small event loop:
 *
 *   1. Drain any pending LLM events with client.pumpEvents(0).
 *   2. React to any state changes (a turn finishing, for example).
 *   3. If a line of user input is ready, process it.
 *   4. Sleep briefly and repeat.
 *
 * A dedicated input thread performs a blocking read on the console and
 * places each completed line into a queue. The main thread never
 * blocks on the console: it polls the queue. This lets the model's
 * streaming output and the operator's typing coexist without any
 * synchronization between them beyond a plain mutex-guarded queue.
 *
 * Console input encoding
 * ----------------------
 * On Windows the console's internal representation is UTF-16. The
 * CRT's std::cin path performs an ANSI code page conversion that
 * mangles non-ASCII input on non-UTF-8 locales (for example CP936 on
 * Chinese Windows). The input thread therefore uses ReadConsoleW and
 * WideCharToMultiByte to produce UTF-8 directly. On non-Windows
 * platforms std::getline is used, which is already UTF-8 aware.
 *
 * Attachments and Structured Output
 * ---------------------------------
 * Both features are supported in one-shot and interactive modes.
 *
 *   Attachments are one-shot: after they are delivered with a turn,
 *   the pending lists are cleared automatically.
 *
 *   Structured Output is sticky: once a JSON Schema is loaded, it
 *   remains active on every subsequent turn until it is explicitly
 *   disabled with /schema off.
 *
 * The four possible combinations are dispatched as follows:
 *
 *   schema + attachments  ->  generateWithAttachmentsAndSchema
 *   schema only           ->  generateWithJsonSchema
 *   attachments only      ->  generateWithAttachments
 *   neither               ->  generate
 *
 * Command-line:
 *   --schema <file>        Load JSON Schema from file (enables SO)
 *   --schema-name <name>   Schema name (default: response_schema)
 *   --no-strict            Disable strict mode (default: strict is ON)
 *
 * Interactive:
 *   /schema <file>         Load JSON Schema from a file
 *   /schema off            Disable Structured Output
 *   /schema                Show the current schema
 *   /schema template       Load the built-in detector template
 *   /schema-name <name>    Set the schema name
 *   /strict on|off         Toggle strict mode
 *
 * Structured Output works against llm_proxy and llm_proxy_tool. It
 * does NOT work against llm_service.
 *
 * Turn lifecycle
 * --------------
 * The main loop tracks a single boolean, `turn_active`. While a turn
 * is active:
 *   - The "> " prompt is not printed.
 *   - Incoming user lines are answered with a short "please wait"
 *     notice and discarded.
 *   - The loop continues to pump events, so streaming output and the
 *     final finish event are delivered as normal.
 * When the finish event arrives, `turn_active` is cleared and the
 * prompt returns.
 *
 * All output is English. All comments are English.
 */

#define _CRT_SECURE_NO_WARNINGS

#include "llm_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

    // ============================================================================
    // Constants
    // ============================================================================

    constexpr const char* kDefaultEndpoint = "ipc:llm_service";
    constexpr const char* kDefaultServerApp = "LLM_Service";
    constexpr int kDefaultTimeoutMs = 30000;

    // Poll interval for the main event loop, in milliseconds.
    constexpr int kMainLoopPollMs = 15;

    const char* kStyleThink = "\033[2m";
    const char* kStyleReset = "\033[0m";

    // Default schema name used when --schema is given but --schema-name
    // is not, or when a schema file is loaded interactively.
    constexpr const char* kDefaultSchemaName = "response_schema";

    // Built-in detector template. Loaded with "/schema template".
    // The schema follows the same shape used by the Pascal demo:
    // label + normalized bbox + confidence, and an explicit
    // detections array at the top level.
    const char* kDetectorTemplate = R"JSON({
  "type": "object",
  "properties": {
    "detections": {
      "type": "array",
      "description": "List of detected objects with confidence scores",
      "items": {
        "type": "object",
        "properties": {
          "label": {
            "type": "string",
            "description": "Object class name, e.g. person, car, dog"
          },
          "bbox": {
            "type": "array",
            "description": "Normalized bbox [x_min, y_min, x_max, y_max], values in 0~1",
            "items": { "type": "number", "minimum": 0, "maximum": 1 },
            "minItems": 4,
            "maxItems": 4
          },
          "confidence": {
            "type": "number",
            "description": "Detection confidence, 0.0 to 1.0",
            "minimum": 0,
            "maximum": 1
          }
        },
        "required": ["label", "bbox", "confidence"]
      }
    }
  },
  "required": ["detections"]
})JSON";

    // ============================================================================
    // Console setup
    // ============================================================================

    void setupConsole() {
#ifdef _WIN32
        HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h_out != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(h_out, &mode)) {
                mode |= 0x0001 | 0x0004;  // ENABLE_PROCESSED_OUTPUT | VT
                SetConsoleMode(h_out, mode);
            }
        }
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
        std::setlocale(LC_CTYPE, ".UTF-8");
#else
        std::setlocale(LC_CTYPE, "");
#endif
    }

    bool useColor() {
        const char* no_color = std::getenv("NO_COLOR");
        return !(no_color != nullptr && no_color[0] != '\0');
    }

    // ============================================================================
    // UTF-8 console line reader (called from the input thread only)
    // ============================================================================

    bool readLineUtf8(std::string& out) {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }

        std::wstring buf;
        wchar_t ch = 0;
        DWORD read = 0;

        while (true) {
            if (!ReadConsoleW(h, &ch, 1, &read, nullptr)) {
                return false;
            }
            if (read == 0) {
                if (buf.empty()) {
                    return false;
                }
                break;
            }
            if (ch == L'\r') {
                continue;
            }
            if (ch == L'\n') {
                break;
            }
            buf.push_back(ch);
        }

        if (buf.empty()) {
            out.clear();
            return true;
        }

        const int n = WideCharToMultiByte(
            CP_UTF8, 0,
            buf.data(), static_cast<int>(buf.size()),
            nullptr, 0, nullptr, nullptr);
        if (n <= 0) {
            return false;
        }
        out.resize(static_cast<std::size_t>(n));
        WideCharToMultiByte(
            CP_UTF8, 0,
            buf.data(), static_cast<int>(buf.size()),
            &out[0], n, nullptr, nullptr);
        return true;
#else
        return static_cast<bool>(std::getline(std::cin, out));
#endif
    }

    // ============================================================================
    // Text file reader (for JSON Schema files)
    // ============================================================================

    bool readTextFile(const std::string& path,
        std::string& out,
        std::string& error) {
        out.clear();
        error.clear();

        std::ifstream f(path, std::ios::binary);
        if (!f) {
            error = "cannot open file: " + path;
            return false;
        }

        std::ostringstream ss;
        ss << f.rdbuf();
        if (f.bad()) {
            error = "read error: " + path;
            return false;
        }
        out = ss.str();
        return true;
    }

    // ============================================================================
    // Pending schema state
    // ============================================================================

    /**
     * @brief State of the Structured Output feature.
     *
     * When `active` is true and `body` is non-empty, every subsequent
     * turn is issued with options.response_format built from
     * (`name`, `body`, `strict`). The schema persists across turns
     * until the user disables it with "/schema off".
     */
    struct PendingSchema {
        bool active = false;
        std::string name = kDefaultSchemaName;
        std::string body;      // JSON Schema object as a raw text string
        bool strict = true;
    };

    // ============================================================================
    // Argument parsing
    // ============================================================================

    struct Options {
        std::string endpoint = kDefaultEndpoint;
        std::string server_app = kDefaultServerApp;
        int timeout_ms = kDefaultTimeoutMs;

        std::string content;
        bool content_set = false;
        std::string prompt;
        std::string session_id;
        std::string system_message;

        std::vector<std::string> text_files;
        std::vector<std::string> image_files;

        // Structured Output
        std::string schema_file;    // path to a JSON Schema file
        std::string schema_name = kDefaultSchemaName;
        bool strict = true;         // strict mode is ON by default

        bool keep = false;
        bool thinking = false;
        bool debug = false;

        bool help = false;
    };

    void printUsage(const char* prog) {
        std::printf(
            "Usage: %s [options]\n"
            "\n"
            "Interactive mode is entered when none of --content, --text,\n"
            "--image, or --schema is given. Otherwise a single turn is\n"
            "issued and the program exits.\n"
            "\n"
            "Connection:\n"
            "  --endpoint <addr>         LingoFuse endpoint (default: %s)\n"
            "  --server-app <name>       Server App name (default: %s)\n"
            "  --timeout <ms>            Call timeout in milliseconds "
            "(default: %d)\n"
            "\n"
            "One-shot mode:\n"
            "  --content <text>          Content to send\n"
            "  --prompt <text>           Extra prompt appended after content\n"
            "  --text <file>             Attach a text file (repeatable)\n"
            "  --image <file>            Attach an image file (repeatable)\n"
            "  --session-id <id>         Reuse an existing session\n"
            "  --system-message <text>   System message for a new session\n"
            "  --keep                    Keep the session alive after the turn\n"
            "  --thinking                Enable thinking (server-dependent)\n"
            "\n"
            "Structured Output (works with llm_proxy / llm_proxy_tool):\n"
            "  --schema <file>           Load JSON Schema from file\n"
            "  --schema-name <name>      Schema name (default: %s)\n"
            "  --no-strict               Disable strict mode (default: ON)\n"
            "\n"
            "Diagnostics:\n"
            "  --debug                   Print debug information to stderr\n"
            "  --help, -h                Show this help and exit\n"
            "\n"
            "Interactive commands:\n"
            "  /new                      Create a new session\n"
            "  /use <session_id>         Switch the current session\n"
            "  /sessions                 List sessions for this client\n"
            "  /close [id]               Close a session (default: current)\n"
            "  /cancel                   Cancel the current generation\n"
            "  /sys <message>            Update the global system message\n"
            "  /health                   Query the server health\n"
            "  /capabilities             Show the server capability matrix\n"
            "  /capabilities refresh     Force a fresh capability fetch\n"
            "  /thinking on|off          Toggle thinking mode\n"
            "  /text <file>              Attach a text file to the next turn\n"
            "  /image <file>             Attach an image file to the next turn\n"
            "  /attach                   List currently attached files\n"
            "  /clear                    Clear all attached files\n"
            "  /schema <file>            Load JSON Schema from file\n"
            "  /schema template          Load the built-in detector template\n"
            "  /schema off               Disable Structured Output\n"
            "  /schema                   Show the current schema\n"
            "  /schema-name <name>       Set the schema name\n"
            "  /strict on|off            Toggle strict mode\n"
            "  /help                     Show this help\n"
            "  /quit, /exit              Quit\n"
            "\n"
            "Anything else is sent to the current session as user input.\n"
            "Pending attachments are sent with that turn and then cleared.\n"
            "The active schema (if any) is applied to every subsequent\n"
            "turn until it is disabled with /schema off.\n"
            "\n"
            "Environment:\n"
            "  NO_COLOR=1                Disable ANSI styles\n",
            prog, kDefaultEndpoint, kDefaultServerApp, kDefaultTimeoutMs,
            kDefaultSchemaName);
    }

    bool parseArgs(int argc, char** argv, Options& opts, std::string& error) {
        auto need = [&](int i, const char* name) -> bool {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return false;
            }
            return true;
            };

        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--help" || a == "-h") {
                opts.help = true;
            }
            else if (a == "--endpoint") {
                if (!need(i, "--endpoint")) return false;
                opts.endpoint = argv[++i];
            }
            else if (a == "--server-app") {
                if (!need(i, "--server-app")) return false;
                opts.server_app = argv[++i];
            }
            else if (a == "--timeout") {
                if (!need(i, "--timeout")) return false;
                try {
                    opts.timeout_ms = std::stoi(argv[++i]);
                }
                catch (...) {
                    error = "invalid value for --timeout";
                    return false;
                }
            }
            else if (a == "--content") {
                if (!need(i, "--content")) return false;
                opts.content = argv[++i];
                opts.content_set = true;
            }
            else if (a == "--prompt") {
                if (!need(i, "--prompt")) return false;
                opts.prompt = argv[++i];
            }
            else if (a == "--text") {
                if (!need(i, "--text")) return false;
                opts.text_files.push_back(argv[++i]);
            }
            else if (a == "--image") {
                if (!need(i, "--image")) return false;
                opts.image_files.push_back(argv[++i]);
            }
            else if (a == "--session-id") {
                if (!need(i, "--session-id")) return false;
                opts.session_id = argv[++i];
            }
            else if (a == "--system-message") {
                if (!need(i, "--system-message")) return false;
                opts.system_message = argv[++i];
            }
            else if (a == "--schema") {
                if (!need(i, "--schema")) return false;
                opts.schema_file = argv[++i];
            }
            else if (a == "--schema-name") {
                if (!need(i, "--schema-name")) return false;
                opts.schema_name = argv[++i];
            }
            else if (a == "--no-strict") {
                opts.strict = false;
            }
            else if (a == "--keep") {
                opts.keep = true;
            }
            else if (a == "--thinking") {
                opts.thinking = true;
            }
            else if (a == "--debug") {
                opts.debug = true;
            }
            else {
                error = "unknown argument: " + a;
                return false;
            }
        }
        return true;
    }

    // ============================================================================
    // InputReader: a thread that blocks on the console, a queue the main
    //              thread polls
    // ============================================================================

    class InputReader {
    public:
        InputReader() = default;

        ~InputReader() {
            stop();
        }

        InputReader(const InputReader&) = delete;
        InputReader& operator=(const InputReader&) = delete;

        void start() {
            if (thread_.joinable()) {
                return;
            }
            running_.store(true);
            thread_ = std::thread([this] { loop(); });
        }

        void stop() {
            running_.store(false);
            if (thread_.joinable()) {
                // A blocking ReadConsoleW cannot be reliably cancelled
                // from another thread on all platforms. The safest
                // shutdown for a short-lived CLI is to detach and let the
                // OS clean up the thread at process exit.
                thread_.detach();
            }
        }

        bool tryPop(std::string& out) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.empty()) {
                return false;
            }
            out = std::move(queue_.front());
            queue_.pop();
            return true;
        }

        bool eofReached() const { return eof_.load(); }

    private:
        void loop() {
            std::string line;
            while (running_.load()) {
                if (!readLineUtf8(line)) {
                    eof_.store(true);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    queue_.push(std::move(line));
                }
            }
            running_.store(false);
        }

        std::thread thread_;
        std::atomic<bool> running_{ false };
        std::atomic<bool> eof_{ false };
        std::queue<std::string> queue_;
        std::mutex mtx_;
    };

    // ============================================================================
    // Shared state for the finish event
    // ============================================================================

    struct TurnState {
        bool finished = false;
        std::string reason;
    };

    // ============================================================================
    // Unified dispatch for the four generate combinations
    // ============================================================================
    //
    // The four combinations:
    //   schema + attachments  ->  generateWithAttachmentsAndSchema
    //   schema only           ->  generateWithJsonSchema
    //   attachments only      ->  generateWithAttachments
    //   neither               ->  generate
    //
    // The caller is responsible for clearing the pending attachments
    // after a successful send. The schema is not cleared here: it is
    // sticky.

    bool sendTurnCombined(llm::Client& client,
        const std::string& text,
        const llm::TextAttachments& texts,
        const llm::ImageAttachments& images,
        const PendingSchema& schema,
        std::string& session_id,
        std::string& error) {
        const bool has_attachments = !texts.empty() || !images.empty();
        const bool has_schema = schema.active && !schema.body.empty();

        if (has_schema && has_attachments) {
            return client.generateWithAttachmentsAndSchema(
                text, "", texts, images,
                schema.name, schema.body, schema.strict,
                session_id, error);
        }
        if (has_schema) {
            return client.generateWithJsonSchema(
                text, "", schema.name, schema.body, schema.strict,
                session_id, error);
        }
        if (has_attachments) {
            return client.generateWithAttachments(
                text, "", texts, images, session_id, error);
        }
        return client.generate(text, "", session_id, error);
    }

    // ============================================================================
    // Session summary parsing (for /sessions)
    // ============================================================================

    void printSessions(const std::string& sessions_json,
        const std::string& current_session_id) {
        try {
            auto j = llm::json::parse(sessions_json);
            if (!j.is_object() || !j.contains("sessions")
                || !j["sessions"].is_array()) {
                std::cout << "[Client] No sessions reported." << std::endl;
                return;
            }
            const auto& arr = j["sessions"];
            if (arr.empty()) {
                std::cout << "[Client] No sessions for this client." << std::endl;
                return;
            }
            std::cout << "[Client] " << arr.size() << " session(s):"
                << std::endl;
            for (const auto& s : arr) {
                if (!s.is_object()) continue;
                const std::string sid = s.value("session_id", std::string("?"));
                const std::string status = s.value("status", std::string("?"));
                const auto count = s.value("message_count", -1);
                const std::string marker =
                    (sid == current_session_id) ? " *" : "  ";
                std::cout << marker << " " << sid
                    << "  status=" << status
                    << "  messages=" << count << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cout << "[Client] Failed to parse sessions: "
                << e.what() << std::endl;
        }
    }

    // ============================================================================
    // Capability display
    // ============================================================================

    void printCapabilities(const llm::Client& c) {
        if (!c.hasCapabilityInfo()) {
            std::cout << "[Client] No capability information available."
                << std::endl;
            return;
        }
        std::cout << "[Client] Server kind: "
            << (c.serverKind().empty() ? "unknown" : c.serverKind())
            << std::endl;

        try {
            auto j = llm::json::parse(c.capabilitiesRawJson());
            if (!j.is_object() || !j.contains("capabilities")) {
                std::cout << "[Client] Capability matrix is empty." << std::endl;
                return;
            }
            std::vector<std::string> supported;
            std::vector<std::string> unsupported;
            for (auto it = j["capabilities"].begin();
                it != j["capabilities"].end(); ++it) {
                const int flag = it.value().is_number_integer()
                    ? it.value().get<int>() : 0;
                if (flag == 1) {
                    supported.push_back(it.key());
                }
                else {
                    unsupported.push_back(it.key());
                }
            }
            std::sort(supported.begin(), supported.end());
            std::sort(unsupported.begin(), unsupported.end());

            std::cout << "[Client] Supported APIs ("
                << supported.size() << "):" << std::endl;
            for (const auto& name : supported) {
                std::cout << "    [1] " << name << std::endl;
            }
            if (!unsupported.empty()) {
                std::cout << "[Client] Unsupported APIs ("
                    << unsupported.size() << "):" << std::endl;
                for (const auto& name : unsupported) {
                    std::cout << "    [0] " << name << std::endl;
                }
            }
        }
        catch (const std::exception& e) {
            std::cout << "[Client] Failed to parse capabilities: "
                << e.what() << std::endl;
        }
    }

    // ============================================================================
    // Schema helpers
    // ============================================================================

    /**
     * @brief Validate that `body` is a JSON object (not just any JSON).
     *
     * Returns true if the string parses as a JSON object. On failure,
     * `err` describes the problem.
     */
    bool validateSchemaObject(const std::string& body, std::string& err) {
        err.clear();
        if (body.empty()) {
            err = "schema body is empty";
            return false;
        }
        try {
            auto j = llm::json::parse(body);
            if (!j.is_object()) {
                err = "schema body is not a JSON object";
                return false;
            }
        }
        catch (const std::exception& e) {
            err = std::string("schema is not valid JSON: ") + e.what();
            return false;
        }
        return true;
    }

    void printSchemaStatus(const PendingSchema& schema) {
        if (!schema.active) {
            std::cout << "[Client] Structured Output: disabled." << std::endl;
            return;
        }
        std::cout << "[Client] Structured Output: enabled. "
            << "name=" << schema.name
            << ", strict=" << (schema.strict ? "on" : "off")
            << ", body=" << schema.body.size() << " chars." << std::endl;
    }

    // ============================================================================
    // Interactive command handler
    // ============================================================================
    //
    // Returns false if the caller should exit the loop.

    bool handleInteractiveCommand(llm::Client& client,
        const std::string& line,
        std::string& session_id,
        llm::TextAttachments& pending_texts,
        llm::ImageAttachments& pending_images,
        PendingSchema& schema) {
        // Split the command and argument.
        std::string cmd;
        std::string arg;
        const auto sp = line.find_first_of(" \t");
        if (sp == std::string::npos) {
            cmd = line.substr(1);
        }
        else {
            cmd = line.substr(1, sp - 1);
            arg = line.substr(sp + 1);
            const auto a = arg.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) {
                arg.clear();
            }
            else {
                arg = arg.substr(a);
            }
        }
        for (auto& c : cmd) {
            c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
        }

        if (cmd == "quit" || cmd == "exit") {
            return false;
        }

        if (cmd == "help") {
            std::cout <<
                "Commands:\n"
                "  /new                      Create a new session\n"
                "  /use <session_id>         Switch the current session\n"
                "  /sessions                 List sessions for this client\n"
                "  /close [id]               Close a session (default: current)\n"
                "  /cancel                   Cancel the current generation\n"
                "  /sys <message>            Update the global system message\n"
                "  /health                   Query the server health\n"
                "  /capabilities             Show the server capability matrix\n"
                "  /capabilities refresh     Force a fresh capability fetch\n"
                "  /thinking on|off          Toggle thinking mode\n"
                "  /text <file>              Attach a text file to the next turn\n"
                "  /image <file>             Attach an image file to the next turn\n"
                "  /attach                   List currently attached files\n"
                "  /clear                    Clear all attached files\n"
                "  /schema <file>            Load JSON Schema from file\n"
                "  /schema template          Load the built-in detector template\n"
                "  /schema off               Disable Structured Output\n"
                "  /schema                   Show the current schema\n"
                "  /schema-name <name>       Set the schema name\n"
                "  /strict on|off            Toggle strict mode\n"
                "  /help                     Show this help\n"
                "  /quit, /exit              Quit\n"
                "Anything else is sent to the current session as user input.\n"
                "Pending attachments are sent with that turn and then\n"
                "cleared. The active schema, if any, applies to every\n"
                "subsequent turn until disabled with /schema off."
                << std::endl;
            return true;
        }

        if (cmd == "new") {
            std::string sid;
            std::string err;
            if (!client.createSession(sid, err)) {
                std::cerr << "[Client] Failed to create session: "
                    << err << std::endl;
            }
            else {
                session_id = sid;
                // Switching to a fresh session discards pending
                // attachments. The schema is intentionally kept, since
                // Structured Output usually stays on across sessions.
                if (!pending_texts.empty() || !pending_images.empty()) {
                    pending_texts.clear();
                    pending_images.clear();
                    std::cout << "[Client] Pending attachments cleared."
                        << std::endl;
                }
                std::cout << "[Client] Created session " << sid << std::endl;
                if (schema.active) {
                    std::cout << "[Client] Structured Output remains "
                        "active (name=" << schema.name
                        << ", strict=" << (schema.strict ? "on" : "off")
                        << ")." << std::endl;
                }
            }
            return true;
        }

        if (cmd == "use") {
            if (arg.empty()) {
                std::cout << "[Client] Usage: /use <session_id>" << std::endl;
            }
            else {
                session_id = arg;
                client.setCurrentSessionId(session_id);
                std::cout << "[Client] Now using session "
                    << session_id << std::endl;
            }
            return true;
        }

        if (cmd == "sessions" || cmd == "list") {
            std::string sessions_json;
            std::string err;
            if (!client.listSessions(sessions_json, err)) {
                std::cerr << "[Client] Failed to list sessions: "
                    << err << std::endl;
            }
            else {
                printSessions(sessions_json, session_id);
            }
            return true;
        }

        if (cmd == "close") {
            const std::string target = arg.empty() ? session_id : arg;
            if (target.empty()) {
                std::cout << "[Client] No session to close." << std::endl;
            }
            else {
                std::string err;
                if (!client.closeSession(target, true, err)) {
                    std::cerr << "[Client] Failed to close session: "
                        << err << std::endl;
                }
                else {
                    std::cout << "[Client] Session " << target
                        << " closed." << std::endl;
                    if (target == session_id) {
                        session_id.clear();
                        if (!pending_texts.empty() || !pending_images.empty()) {
                            pending_texts.clear();
                            pending_images.clear();
                            std::cout << "[Client] Pending attachments cleared."
                                << std::endl;
                        }
                    }
                }
            }
            return true;
        }

        if (cmd == "cancel") {
            if (session_id.empty()) {
                std::cout << "[Client] No current session." << std::endl;
            }
            else {
                std::string err;
                if (!client.cancelSession(session_id, err)) {
                    std::cerr << "[Client] Cancel failed: " << err << std::endl;
                }
                else {
                    std::cout << "[Client] Cancel requested." << std::endl;
                }
            }
            return true;
        }

        if (cmd == "sys") {
            if (arg.empty()) {
                std::cout << "[Client] Usage: /sys <message>" << std::endl;
            }
            else {
                std::string err;
                if (!client.setSystemMessage(arg, err)) {
                    std::cerr << "[Client] Failed to set system message: "
                        << err << std::endl;
                }
                else {
                    std::cout << "[Client] Global default system message "
                        "updated." << std::endl;
                }
            }
            return true;
        }

        if (cmd == "health") {
            std::string hj;
            std::string err;
            if (!client.health(hj, err)) {
                std::cerr << "[Client] Health check failed: "
                    << err << std::endl;
            }
            else {
                std::cout << "[Client] Server health:" << std::endl;
                try {
                    auto j = llm::json::parse(hj);
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        std::cout << "    " << it.key() << ": "
                            << it.value().dump() << std::endl;
                    }
                }
                catch (...) {
                    std::cout << hj << std::endl;
                }
            }
            return true;
        }

        if (cmd == "capabilities" || cmd == "caps") {
            if (arg == "refresh" || arg == "reload") {
                std::string cj;
                std::string err;
                if (!client.getApiCapabilities(cj, err)) {
                    std::cerr << "[Client] Failed to refresh capabilities: "
                        << err << std::endl;
                }
                else {
                    std::cout << "[Client] Capability matrix refreshed."
                        << std::endl;
                }
            }
            printCapabilities(client);
            return true;
        }

        if (cmd == "thinking") {
            std::cout << "[Client] Thinking mode is controlled by the "
                "server. Use --thinking at startup or set the "
                "corresponding server option." << std::endl;
            return true;
        }

        // ---- Attachment commands ------------------------------------

        if (cmd == "text") {
            if (arg.empty()) {
                std::cout << "[Client] Usage: /text <file_path>" << std::endl;
                return true;
            }
            llm::TextAttachment att;
            std::string err;
            if (!llm::buildTextAttachmentFromFile(arg, att, err)) {
                std::cerr << "[Client] Failed to load text file: "
                    << err << std::endl;
                return true;
            }
            std::cout << "[Client] Attached text: " << att.name
                << " (" << att.text.size() << " chars, mime="
                << att.mime << ")" << std::endl;
            pending_texts.push_back(std::move(att));
            return true;
        }

        if (cmd == "image" || cmd == "img") {
            if (arg.empty()) {
                std::cout << "[Client] Usage: /image <file_path>" << std::endl;
                return true;
            }
            llm::ImageAttachment att;
            std::string err;
            if (!llm::buildImageAttachmentFromFile(arg, att, err)) {
                std::cerr << "[Client] Failed to load image file: "
                    << err << std::endl;
                return true;
            }
            std::cout << "[Client] Attached image: " << att.name
                << " (" << att.data_b64.size() << " base64 chars, mime="
                << att.mime << ")" << std::endl;
            pending_images.push_back(std::move(att));
            return true;
        }

        if (cmd == "attach" || cmd == "attachments") {
            if (pending_texts.empty() && pending_images.empty()
                && !schema.active) {
                std::cout << "[Client] No attachments or schema pending."
                    << std::endl;
                return true;
            }
            if (!pending_texts.empty() || !pending_images.empty()) {
                std::cout << "[Client] Pending attachments:" << std::endl;
                for (const auto& t : pending_texts) {
                    std::cout << "    [text]  " << t.name
                        << " (" << t.text.size() << " chars)" << std::endl;
                }
                for (const auto& im : pending_images) {
                    std::cout << "    [image] " << im.name
                        << " (" << im.data_b64.size() << " base64 chars)"
                        << std::endl;
                }
            }
            else {
                std::cout << "[Client] No pending attachments." << std::endl;
            }
            printSchemaStatus(schema);
            return true;
        }

        if (cmd == "clear") {
            const std::size_t n =
                pending_texts.size() + pending_images.size();
            pending_texts.clear();
            pending_images.clear();
            std::cout << "[Client] Cleared " << n
                << " pending attachment(s)." << std::endl;
            return true;
        }

        // ---- Structured Output commands -----------------------------

        if (cmd == "schema") {
            // "/schema"            -> show
            // "/schema off"        -> disable
            // "/schema template"   -> load built-in detector template
            // "/schema <file>"     -> load from file
            if (arg.empty() || arg == "show") {
                printSchemaStatus(schema);
                return true;
            }

            std::string arg_lower = arg;
            std::transform(arg_lower.begin(), arg_lower.end(),
                arg_lower.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });

            if (arg_lower == "off" || arg_lower == "disable"
                || arg_lower == "none") {
                schema.active = false;
                schema.body.clear();
                std::cout << "[Client] Structured Output disabled."
                    << std::endl;
                return true;
            }

            if (arg_lower == "template" || arg_lower == "detector") {
                schema.active = true;
                schema.name = "object_detection";
                schema.strict = true;
                schema.body = kDetectorTemplate;
                std::cout << "[Client] Loaded detector template: "
                    << "name=object_detection, strict=on, body="
                    << schema.body.size() << " chars." << std::endl;
                return true;
            }

            // Treat the argument as a file path.
            std::string body;
            std::string err;
            if (!readTextFile(arg, body, err)) {
                std::cerr << "[Client] Failed to read schema file: "
                    << err << std::endl;
                return true;
            }
            if (!validateSchemaObject(body, err)) {
                std::cerr << "[Client] Schema file rejected: "
                    << err << std::endl;
                return true;
            }
            schema.active = true;
            schema.body = std::move(body);
            std::cout << "[Client] Schema loaded: name=" << schema.name
                << ", strict=" << (schema.strict ? "on" : "off")
                << ", body=" << schema.body.size() << " chars." << std::endl;
            return true;
        }

        if (cmd == "schema-name") {
            if (arg.empty()) {
                std::cout << "[Client] Usage: /schema-name <name>"
                    << std::endl;
                return true;
            }
            schema.name = arg;
            std::cout << "[Client] Schema name set to: "
                << schema.name << std::endl;
            return true;
        }

        if (cmd == "strict") {
            if (arg.empty()) {
                std::cout << "[Client] Strict mode is currently "
                    << (schema.strict ? "ON" : "OFF")
                    << ". Usage: /strict on|off" << std::endl;
                return true;
            }
            std::string a = arg;
            std::transform(a.begin(), a.end(), a.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            if (a == "on" || a == "1" || a == "true" || a == "yes") {
                schema.strict = true;
            }
            else if (a == "off" || a == "0" || a == "false" || a == "no") {
                schema.strict = false;
            }
            else {
                std::cout << "[Client] Usage: /strict on|off" << std::endl;
                return true;
            }
            std::cout << "[Client] Strict mode set to: "
                << (schema.strict ? "ON" : "OFF") << std::endl;
            return true;
        }

        std::cout << "[Client] Unknown command: /" << cmd << std::endl;
        return true;
    }

    // ============================================================================
    // Interactive main loop
    // ============================================================================

    int runInteractive(llm::Client& client, const Options& opts) {
        std::string error;
        std::string session_id;

        if (!client.createSession(opts.system_message, session_id, error)) {
            std::cerr << "[Client] Failed to create session: "
                << error << std::endl;
            return 1;
        }
        client.setCurrentSessionId(session_id);
        std::cout << "[Client] Created session " << session_id << std::endl;

        // Initial schema state, populated from the command line if
        // --schema was given at startup.
        PendingSchema schema;
        if (!opts.schema_file.empty()) {
            std::string body;
            if (!readTextFile(opts.schema_file, body, error)) {
                std::cerr << "[Client] Failed to read schema file: "
                    << error << std::endl;
                return 1;
            }
            if (!validateSchemaObject(body, error)) {
                std::cerr << "[Client] Schema file rejected: "
                    << error << std::endl;
                return 1;
            }
            schema.active = true;
            schema.body = std::move(body);
            schema.name = opts.schema_name;
            schema.strict = opts.strict;
            std::cout << "[Client] Schema loaded from command line: name="
                << schema.name
                << ", strict=" << (schema.strict ? "on" : "off")
                << ", body=" << schema.body.size() << " chars."
                << std::endl;
        }

        std::cout << std::endl;
        std::cout << "Interactive mode. Type /help for commands, /quit to exit."
            << std::endl;
        std::cout << std::endl;

        InputReader input;
        input.start();

        TurnState turn_state;

        client.setOnFinish([&turn_state](const std::string&,
            const std::string& reason) {
                turn_state.finished = true;
                turn_state.reason = reason;
            });

        // Pending attachments. Populated by /text and /image, cleared
        // after a successful send.
        llm::TextAttachments pending_texts;
        llm::ImageAttachments pending_images;

        bool turn_active = false;
        bool need_prompt = true;
        bool running = true;

        while (running) {
            // ---- 1. Drain pending LLM events ----------------------------
            client.pumpEvents(0);

            // ---- 2. React to a finished turn ----------------------------
            if (turn_active && turn_state.finished) {
                turn_active = false;
                need_prompt = true;
                std::cout << std::endl;
                std::cout << "------------------------------------------------------------"
                    << std::endl;
                std::cout << "[Client] Turn finished (reason="
                    << (turn_state.reason.empty() ? "stop"
                        : turn_state.reason)
                    << ")" << std::endl;
                turn_state.finished = false;
                turn_state.reason.clear();
            }

            // ---- 3. Print the prompt if it is due -----------------------
            if (need_prompt && !turn_active) {
                std::cout << "> ";
                std::cout.flush();
                need_prompt = false;
            }

            // ---- 4. Process one user input line -------------------------
            std::string line;
            if (input.tryPop(line)) {
                std::cout << std::endl;

                const auto first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                    need_prompt = !turn_active;
                    continue;
                }
                line = line.substr(first);

                if (opts.debug) {
                    std::cerr << "[DEBUG] line bytes:";
                    for (unsigned char c : line) {
                        std::fprintf(stderr, " %02X", c);
                    }
                    std::cerr << std::endl;
                }

                // ---- Slash commands ------------------------------------
                if (!line.empty() && line[0] == '/') {
                    if (turn_active) {
                        std::cout << "[Client] A turn is running. "
                            "Please wait for it to finish."
                            << std::endl;
                        need_prompt = false;
                        continue;
                    }
                    if (!handleInteractiveCommand(client, line, session_id,
                        pending_texts, pending_images, schema)) {
                        running = false;
                        continue;
                    }
                    need_prompt = true;
                    continue;
                }

                // ---- Ordinary input: send a turn -----------------------
                if (turn_active) {
                    std::cout << "[Client] A turn is running. "
                        "Please wait for it to finish."
                        << std::endl;
                    need_prompt = false;
                    continue;
                }

                if (session_id.empty()) {
                    std::cout << "[Client] No current session. "
                        "Use /new to create one." << std::endl;
                    need_prompt = true;
                    continue;
                }

                turn_state.finished = false;
                turn_state.reason.clear();

                const bool had_attachments =
                    !pending_texts.empty() || !pending_images.empty();

                std::string err;
                const bool ok = sendTurnCombined(
                    client, line, pending_texts, pending_images, schema,
                    session_id, err);

                if (!ok) {
                    std::cout << "[Client] Failed to send: "
                        << err << std::endl;
                    need_prompt = true;
                }
                else {
                    if (had_attachments) {
                        const std::size_t n_text = pending_texts.size();
                        const std::size_t n_image = pending_images.size();
                        pending_texts.clear();
                        pending_images.clear();
                        std::cout << "[Client] Attachments cleared after "
                            "send (texts=" << n_text
                            << ", images=" << n_image << ")."
                            << std::endl;
                    }
                    turn_active = true;
                    need_prompt = false;
                    std::cout << "[Client] Session " << session_id
                        << " queued, streaming..." << std::endl;
                    std::cout << "------------------------------------------------------------"
                        << std::endl;
                }
                continue;
            }

            // ---- 5. Nothing to do: sleep briefly and re-loop ------------
            if (input.eofReached()) {
                running = false;
                continue;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(kMainLoopPollMs));
        }

        input.stop();
        return 0;
    }

    // ============================================================================
    // One-shot mode
    // ============================================================================

    int runOnce(llm::Client& client, const Options& opts) {
        llm::TextAttachments texts;
        llm::ImageAttachments images;

        std::string error;

        for (const auto& path : opts.text_files) {
            llm::TextAttachment att;
            if (!llm::buildTextAttachmentFromFile(path, att, error)) {
                std::cerr << "[Client] Failed to load text attachment: "
                    << error << std::endl;
                return 1;
            }
            std::cout << "[Client] Text attachment: " << att.name
                << " (" << att.text.size() << " chars)" << std::endl;
            texts.push_back(std::move(att));
        }

        for (const auto& path : opts.image_files) {
            llm::ImageAttachment att;
            if (!llm::buildImageAttachmentFromFile(path, att, error)) {
                std::cerr << "[Client] Failed to load image attachment: "
                    << error << std::endl;
                return 1;
            }
            std::cout << "[Client] Image attachment: " << att.name
                << " (" << att.data_b64.size() << " base64 chars)"
                << std::endl;
            images.push_back(std::move(att));
        }

        // Optional schema, loaded from the command line.
        PendingSchema schema;
        if (!opts.schema_file.empty()) {
            std::string body;
            if (!readTextFile(opts.schema_file, body, error)) {
                std::cerr << "[Client] Failed to read schema file: "
                    << error << std::endl;
                return 1;
            }
            if (!validateSchemaObject(body, error)) {
                std::cerr << "[Client] Schema file rejected: "
                    << error << std::endl;
                return 1;
            }
            schema.active = true;
            schema.body = std::move(body);
            schema.name = opts.schema_name;
            schema.strict = opts.strict;
            std::cout << "[Client] Schema loaded: name=" << schema.name
                << ", strict=" << (schema.strict ? "on" : "off")
                << ", body=" << schema.body.size() << " chars."
                << std::endl;
        }

        std::string text = opts.content;
        if (!opts.prompt.empty()) {
            if (!text.empty()) {
                text += "\n\n";
            }
            text += opts.prompt;
        }

        if (text.empty() && texts.empty() && images.empty()) {
            std::cerr << "[Client] Nothing to send: provide --content, "
                "--text, or --image." << std::endl;
            return 1;
        }

        std::string session_id = opts.session_id;

        if (session_id.empty()) {
            if (!client.createSession(opts.system_message, session_id, error)) {
                std::cerr << "[Client] Failed to create session: "
                    << error << std::endl;
                return 1;
            }
            std::cout << "[Client] Created session " << session_id
                << std::endl;
        }

        client.setCurrentSessionId(session_id);

        TurnState turn_state;
        client.setOnFinish([&turn_state](const std::string&,
            const std::string& reason) {
                turn_state.finished = true;
                turn_state.reason = reason;
            });

        if (!sendTurnCombined(client, text, texts, images, schema,
            session_id, error)) {
            std::cerr << "[Client] Failed to send turn: " << error
                << std::endl;
            return 1;
        }

        std::cout << "[Client] Session " << session_id
            << " queued, streaming..." << std::endl;
        std::cout << "------------------------------------------------------------"
            << std::endl;

        while (!turn_state.finished) {
            client.pumpEvents(500);
        }

        std::cout << std::endl;
        std::cout << "------------------------------------------------------------"
            << std::endl;
        std::cout << "[Client] Turn finished (reason="
            << (turn_state.reason.empty() ? "stop" : turn_state.reason)
            << ")" << std::endl;

        if (!opts.keep && !session_id.empty()) {
            std::string close_error;
            if (!client.closeSession(session_id, false, close_error)) {
                std::cerr << "[Client] Failed to close session: "
                    << close_error << std::endl;
            }
            else {
                std::cout << "[Client] Session closed." << std::endl;
            }
        }
        else if (!session_id.empty()) {
            std::cout << "[Client] Session " << session_id
                << " kept alive." << std::endl;
        }

        return 0;
    }

} // namespace

// ============================================================================
// Entry point
// ============================================================================

int main(int argc, char** argv) {
    setupConsole();

    Options opts;
    std::string parse_error;
    if (!parseArgs(argc, argv, opts, parse_error)) {
        std::cerr << "[Client] " << parse_error << std::endl;
        std::cerr << "Run with --help for usage." << std::endl;
        return 2;
    }
    if (opts.help) {
        printUsage(argv[0]);
        return 0;
    }

    const bool color = useColor();

    std::cout << "[Client] Connecting to " << opts.endpoint
        << " (server_app=" << opts.server_app << ") ..."
        << std::endl;

    std::unique_ptr<lingofuse::LibraryLoader> loader;
    try {
        loader = std::make_unique<lingofuse::LibraryLoader>();
    }
    catch (const std::exception& e) {
        std::cerr << "[Client] LF_LoadLibrary failed: " << e.what()
            << std::endl;
        std::cerr << "[Client] Place LingoFuse64.dll / liblingofuse.so "
            "next to this executable, or on the OS loader "
            "search path." << std::endl;
        return 1;
    }

    llm::Client client(opts.server_app, opts.endpoint, opts.timeout_ms);

    // Default is DispatchMode::MainThread: handlers are invoked on the
    // thread that calls pumpEvents(), which is the main thread. No
    // dispatcher thread is started.
    client.setOnChunk([](const std::string&, const std::string& t) {
        std::cout << t;
        std::cout.flush();
        });

    client.setOnThink([color](const std::string&, const std::string& t) {
        if (color) {
            std::cout << kStyleThink << t << kStyleReset;
        }
        else {
            std::cout << t;
        }
        std::cout.flush();
        });

    // The finish handler is installed by runOnce() and runInteractive()
    // respectively, because each of them owns a different TurnState.

    client.setOnError([](const std::string&, const std::string& msg) {
        std::cerr << "\n[Client] Server error: " << msg << std::endl;
        });

    client.setOnClosed([](const std::string& sid,
        const std::string& reason) {
            std::cout << "\n[Client] Session " << sid
                << " closed (reason=" << reason << ")" << std::endl;
        });

    std::string connect_error;
    if (!client.connect(connect_error)) {
        std::cerr << "[Client] Connect failed: " << connect_error
            << std::endl;
        return 1;
    }
    std::cout << "[Client] Connected (client_name="
        << client.clientName() << ")" << std::endl;

    // One-shot mode is triggered by any of the data-carrying options.
    // A bare --schema without --content is still treated as interactive
    // mode, so that an operator can load a schema and then type
    // prompts at the REPL.
    const bool one_shot =
        opts.content_set || !opts.text_files.empty()
        || !opts.image_files.empty();

    int rc = 0;
    if (one_shot) {
        rc = runOnce(client, opts);
    }
    else {
        rc = runInteractive(client, opts);
    }

    std::cout << std::endl;
    std::cout << "[Client] Shutting down..." << std::endl;
    client.disconnect();

    std::cout << "[Client] Done." << std::endl;
    return rc;
}