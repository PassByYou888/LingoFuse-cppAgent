#ifndef LLM_CLIENT_HPP_INCLUDED
#define LLM_CLIENT_HPP_INCLUDED

/**
 * @file llm_client.hpp
 * @brief C++17 client library for the LingoFuse LLM services.
 *
 * This header declares a high-level, event-driven client for the
 * LingoFuse LLM services. It is the C++ counterpart of llm_client_v3.pas
 * and speaks the structured streaming protocol defined by
 * llm_service.py / llm_proxy.py / llm_proxy_tool.py.
 *
 * Three backends, one protocol
 * ----------------------------
 *   llm_service.py     (server_kind = "service")
 *       Local inference server (llama.cpp / transformers).
 *
 *   llm_proxy.py       (server_kind = "proxy")
 *       Stateless forwarder to an OpenAI-compatible HTTP backend.
 *
 *   llm_proxy_tool.py  (server_kind = "proxy", tools=1)
 *       Same as llm_proxy.py plus server-side MCP tool execution.
 *
 * This client works transparently against all three. All Call API
 * methods behave identically EXCEPT setSystemMessage(), which is
 * unsupported on the two proxy siblings.
 *
 * Capability discovery
 * --------------------
 * The client fetches the server's API capability matrix through the
 * get_api_capabilities Call API and caches it. Use:
 *
 *   hasCapabilityInfo()      // true once the matrix has been fetched
 *   llmSupported(name)       // true if the named API is supported
 *   isToolBridge()           // true for llm_proxy_tool (LTB)
 *   hasVision()              // reserved: currently always false
 *   hasAttachments()         // server accepts the attachments field
 *
 * Do NOT hard-code assumptions about the server kind. Always query.
 *
 * Event dispatch model
 * --------------------
 * LingoFuse delivers notifications on a background worker thread that
 * is owned by the LingoFuse runtime. Calling user handlers directly on
 * that thread forces every handler to be thread-safe against the main
 * thread, which is a common source of subtle bugs (interleaved writes
 * to stdout, torn state, UI access from the wrong thread).
 *
 * Three dispatch modes are available. The default is MainThread, which
 * is the right choice for any program that owns a main loop (a CLI, a
 * GUI application, an embedded service host).
 *
 *   DispatchMode::MainThread   (DEFAULT)
 *       LingoFuse's notification callback copies the raw payload into
 *       an internal queue and returns immediately. The user's main
 *       thread drains that queue by calling pumpEvents(). Handlers
 *       are invoked inside pumpEvents(), on the calling thread, one
 *       at a time, in arrival order. This is the "synchronize to main
 *       thread" model: handlers always run on the main thread, and
 *       they see a consistent world.
 *
 *   DispatchMode::Queued
 *       Same as MainThread, except a dedicated dispatcher thread
 *       owned by the Client drains the queue and invokes the
 *       handlers. Use this only if the program has no main loop and
 *       wants the library to drive handler delivery.
 *
 *   DispatchMode::Direct
 *       Handlers are invoked directly on LingoFuse's notification
 *       thread. Only use this when you genuinely want
 *       notification-thread semantics and are prepared to make every
 *       handler thread-safe against the main thread.
 *
 * In MainThread and Queued modes, ioMutex() is held during each
 * handler invocation. Callers that produce output from the main
 * thread should acquire ioMutex() around their writes as well, so
 * that main-thread output and handler output never interleave. In
 * MainThread mode this is usually unnecessary because the handlers
 * already run on the main thread, but it is still correct to do so.
 *
 * In Direct mode ioMutex() is also held during handler invocation.
 *
 * The dispatcher thread (Queued mode only) is started by
 * setDispatchMode(DispatchMode::Queued) and stopped by
 * setDispatchMode(any other mode) and by disconnect() / the
 * destructor.
 *
 * Threading model
 * ---------------
 * Call API methods (connect, generate, createSession, ...) are
 * synchronous and block the calling thread. Multiple threads may call
 * different Client instances concurrently. A single Client instance is
 * NOT designed for concurrent Call API invocations from multiple
 * threads; serialize them if needed.
 *
 * Structured Output
 * -----------------
 * The client offers byte-safe entry points that produce a `generate`
 * request carrying options.response_format:
 *
 *   generateStructured                complete response_format JSON
 *   generateWithJsonSchema            auto-builds the outer envelope
 *   generateWithImageFileAndSchema    image file + JSON Schema
 *   generateWithAttachmentsAndSchema  full attachments + JSON Schema
 *
 * Structured Output works against llm_proxy and llm_proxy_tool. It
 * does NOT work against llm_service (local llama.cpp path).
 *
 * JSON safety
 * -----------
 * All JSON I/O goes through lingofuse::io (lf_io.hpp), which is the
 * single source of truth for the toolchain-wide JSON policy
 * (ensure_ascii = false, error_handler_t::replace).
 *
 * Every JSON document is built as a fresh nlohmann::json tree and
 * serialized once. No structured JSON is ever constructed by string
 * concatenation.
 *
 * Dependencies
 * ------------
 *   LingoFuse.h        C ABI declarations and C wrapper functions
 *   LingoFuse.hpp      C++17 RAII wrappers (DataHandle, App, ...)
 *   lf_io.hpp          Unified JSON and string I/O
 *   json.hpp           nlohmann::json
 *
 * The dependency direction is strictly one-way:
 *   LingoFuse.h -> lf_io.hpp -> LingoFuse.hpp -> llm_client.hpp
 *
 * All comments, log messages, and API names are in English.
 */

#include "LingoFuse.hpp"
#include "lf_io.hpp"
#include "json.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace llm {

    using json = nlohmann::json;

    // ============================================================================
    // Attachment records
    // ============================================================================

    struct TextAttachment {
        std::string name;
        std::string mime;
        std::string text;
    };

    struct ImageAttachment {
        std::string name;
        std::string mime;
        std::string data_b64;
    };

    using TextAttachments = std::vector<TextAttachment>;
    using ImageAttachments = std::vector<ImageAttachment>;

    // ============================================================================
    // Event handler types
    // ============================================================================
    //
    // Under DispatchMode::MainThread (the default) and DispatchMode::Queued,
    // all handlers are invoked serially, in arrival order. Under
    // DispatchMode::Direct they are invoked on LingoFuse's notification
    // worker thread.
    //
    // In all modes the ioMutex() is held during the invocation. Handlers
    // therefore do not need to lock it again to produce output. They MUST
    // NOT call any Call API on the same Client (that would deadlock: the
    // Call API may block waiting for a finish event which can only be
    // delivered via the same code path).

    using ChunkHandler = std::function<void(const std::string& session_id,
        const std::string& text)>;
    using ThinkHandler = std::function<void(const std::string& session_id,
        const std::string& text)>;
    using FinishHandler = std::function<void(const std::string& session_id,
        const std::string& reason)>;
    using ErrorHandler = std::function<void(const std::string& session_id,
        const std::string& error_message)>;
    using ClosedHandler = std::function<void(const std::string& session_id,
        const std::string& reason)>;

    // ============================================================================
    // API name constants
    // ============================================================================

    namespace api {
        inline constexpr const char* kStream = "llm_stream";
        inline constexpr const char* kGenerate = "generate";
        inline constexpr const char* kCreateSession = "create_session";
        inline constexpr const char* kCloseSession = "close_session";
        inline constexpr const char* kCancelSession = "cancel_session";
        inline constexpr const char* kListSessions = "list_sessions";
        inline constexpr const char* kSetSystemMessage = "set_system_message";
        inline constexpr const char* kGetCapabilities = "get_api_capabilities";
        inline constexpr const char* kHealth = "health";
    } // namespace api

    // ============================================================================
    // Attachment size limits
    // ============================================================================

    namespace limits {
        constexpr std::size_t kMaxTextBytesPerFile = 256 * 1024;
        constexpr std::size_t kMaxTextBytesTotal = 512 * 1024;
        constexpr std::size_t kMaxImageB64PerFile = 8 * 1024 * 1024;
        constexpr std::size_t kMaxImageB64Total = 16 * 1024 * 1024;
        constexpr std::size_t kMaxNameLen = 256;
    } // namespace limits

    // ============================================================================
    // MIME defaults
    // ============================================================================

    inline constexpr const char* kDefaultTextMime = "text/plain";
    inline constexpr const char* kDefaultImageMime = "image/png";
    inline constexpr const char* kDefaultAttachmentName = "unnamed";

    // ============================================================================
    // Client
    // ============================================================================

    class Client {
    public:
        // ------------------------------------------------------------------
        // Event dispatch mode
        // ------------------------------------------------------------------

        enum class DispatchMode {
            /**
             * Handlers are invoked on the thread that calls pumpEvents(),
             * which is normally the program's main thread. This is the
             * default and the recommended mode for any program that owns
             * a main loop.
             */
            MainThread,

            /**
             * Handlers are invoked on a dedicated dispatcher thread owned
             * by the Client. Use this only if the program has no main
             * loop and wants the library to drive handler delivery.
             */
            Queued,

            /**
             * Handlers are invoked directly on LingoFuse's notification
             * worker thread. Only use this when you genuinely want
             * notification-thread semantics.
             */
            Direct,
        };

        // ------------------------------------------------------------------
        // Construction / destruction
        // ------------------------------------------------------------------

        Client(const std::string& server_app,
            const std::string& endpoint,
            int timeout_ms = 10000);

        ~Client();

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        bool connect(std::string& error);
        void disconnect();

        // ------------------------------------------------------------------
        // Event dispatch configuration and drive
        // ------------------------------------------------------------------

        /**
         * @brief Set the dispatch mode.
         *
         * Must be called before connect(). Changing the mode while
         * connected is not supported; the previous mode is retained.
         *
         * On the default MainThread mode, no dispatcher thread exists.
         * Calling setDispatchMode(DispatchMode::Queued) starts one;
         * calling setDispatchMode with any other value stops it.
         */
        void setDispatchMode(DispatchMode mode);

        DispatchMode dispatchMode() const { return dispatch_mode_; }

        /**
         * @brief Drain and dispatch all pending events.
         *
         * Only meaningful in DispatchMode::MainThread. In that mode, the
         * notification thread pushes raw payloads into an internal queue;
         * this function pops them one by one, parses each one, and
         * invokes the matching user handler on the calling thread.
         *
         * The ioMutex() is held during each handler invocation, matching
         * the contract documented at the top of this header.
         *
         * @param timeout_ms
         *   If 0 (default), the function returns immediately after
         *   draining whatever is already queued.
         *
         *   If > 0 and the queue is empty when the function is called,
         *   it blocks up to timeout_ms waiting for the first event. After
         *   the first event arrives, it drains the rest without waiting.
         *
         * @return Number of events dispatched. 0 means "nothing to do".
         */
        std::size_t pumpEvents(int timeout_ms = 0);

        /**
         * @brief Return the mutex that guards handler invocations.
         *
         * Callers that produce output from a thread that is NOT the one
         * invoking handlers (which is only the case in Queued and Direct
         * modes) should acquire this mutex around their writes, so that
         * main-thread output and handler output never interleave.
         *
         * In the default MainThread mode, handlers run on the same thread
         * as the caller of pumpEvents(), so no external locking is needed
         * for output. The mutex is still exposed for symmetry.
         *
         * WARNING: Do NOT call any Call API (generate, createSession, ...)
         * while holding this mutex. A Call API may block waiting for a
         * finish event, and the finish handler needs the same mutex;
         * holding it would deadlock.
         */
        std::mutex& ioMutex() { return io_mutex_; }

        // ------------------------------------------------------------------
        // Session management
        // ------------------------------------------------------------------

        bool createSession(std::string& session_id, std::string& error);

        bool createSession(const std::string& system_message,
            std::string& session_id,
            std::string& error);

        bool closeSession(const std::string& session_id,
            bool cancel_running,
            std::string& error);

        bool cancelSession(const std::string& session_id, std::string& error);

        bool listSessions(std::string& sessions_json, std::string& error);

        // ------------------------------------------------------------------
        // Generation
        // ------------------------------------------------------------------

        bool generate(const std::string& content,
            const std::string& prompt,
            std::string& session_id,
            std::string& error);

        bool generateWithAttachments(const std::string& content,
            const std::string& prompt,
            const TextAttachments& texts,
            const ImageAttachments& images,
            std::string& session_id,
            std::string& error);

        bool generateWithTextFile(const std::string& content,
            const std::string& prompt,
            const std::string& file_path,
            std::string& session_id,
            std::string& error);

        bool generateWithImageFile(const std::string& content,
            const std::string& prompt,
            const std::string& file_path,
            std::string& session_id,
            std::string& error);

        bool generateCurrent(const std::string& content,
            const std::string& prompt,
            std::string& error);

        // ------------------------------------------------------------------
        // Structured Output
        // ------------------------------------------------------------------

        bool generateStructured(const std::string& content,
            const std::string& prompt,
            const std::string& response_format_json,
            std::string& session_id,
            std::string& error);

        bool generateWithJsonSchema(const std::string& content,
            const std::string& prompt,
            const std::string& schema_name,
            const std::string& schema_json,
            bool strict,
            std::string& session_id,
            std::string& error);

        bool generateWithImageFileAndSchema(const std::string& content,
            const std::string& prompt,
            const std::string& file_path,
            const std::string& schema_name,
            const std::string& schema_json,
            bool strict,
            std::string& session_id,
            std::string& error);

        bool generateWithAttachmentsAndSchema(const std::string& content,
            const std::string& prompt,
            const TextAttachments& texts,
            const ImageAttachments& images,
            const std::string& schema_name,
            const std::string& schema_json,
            bool strict,
            std::string& session_id,
            std::string& error);

        // ------------------------------------------------------------------
        // Server-wide settings
        // ------------------------------------------------------------------

        bool setSystemMessage(const std::string& message, std::string& error);

        bool health(std::string& health_json, std::string& error);

        // ------------------------------------------------------------------
        // Capability discovery
        // ------------------------------------------------------------------

        bool getApiCapabilities(std::string& capabilities_json,
            std::string& error);

        bool hasCapabilityInfo() const { return has_capability_info_.load(); }

        bool llmSupported(const std::string& api_name) const;

        bool isToolBridge() const;
        bool hasVision() const;
        bool hasAttachments() const;

        // ------------------------------------------------------------------
        // Event handlers
        // ------------------------------------------------------------------
        //
        // Set these BEFORE connect(). They are read from the dispatch
        // thread (or, in MainThread mode, from the thread that calls
        // pumpEvents()) and must not be swapped while connected.

        void setOnChunk(ChunkHandler h) { on_chunk_ = std::move(h); }
        void setOnThink(ThinkHandler h) { on_think_ = std::move(h); }
        void setOnFinish(FinishHandler h) { on_finish_ = std::move(h); }
        void setOnError(ErrorHandler h) { on_error_ = std::move(h); }
        void setOnClosed(ClosedHandler h) { on_closed_ = std::move(h); }

        // ------------------------------------------------------------------
        // Accessors
        // ------------------------------------------------------------------

        bool connected() const { return connected_.load(); }
        const std::string& clientName() const { return client_name_; }
        const std::string& serverKind() const { return server_kind_; }
        const std::string& currentSessionId() const { return current_session_id_; }
        void setCurrentSessionId(const std::string& id) { current_session_id_ = id; }
        const std::string& capabilitiesRawJson() const { return capabilities_raw_; }

        std::string takeLastException();

    private:
        // ------------------------------------------------------------------
        // Internal helpers
        // ------------------------------------------------------------------

        bool callApi(const std::string& api_name,
            const std::string& request_bytes,
            std::string& response_bytes,
            std::string& error);

        void onNotify(TDataHnd input);
        void dispatchPayload(const std::string& payload);
        void cleanupPartialConnect(bool exit_main_thread);
        void resetCapabilityState();

        static std::string buildSchemaResponseFormat(
            const std::string& schema_name,
            const std::string& schema_json,
            bool strict,
            std::string& error);

        static bool populateAttachments(
            const TextAttachments& texts,
            const ImageAttachments& images,
            json& attachments_array,
            std::string& error);

        bool sendGenerateCombined(const std::string& content,
            const std::string& prompt,
            const TextAttachments& texts,
            const ImageAttachments& images,
            const std::string& response_format_json,
            std::string& session_id,
            std::string& error);

        static void LF_CDECL notifyTrampoline(void* trigger, void* input);

        // ---- Dispatcher (Queued mode only) ----------------------------------

        void startDispatcher();
        void stopDispatcher();
        void dispatcherLoop();
        void postEvent(std::string payload);

        // ---- State ----------------------------------------------------------

        std::unique_ptr<lingofuse::App> app_;
        std::string server_app_;
        std::string endpoint_;
        int timeout_ms_;

        std::atomic<bool> prepared_{ false };
        std::atomic<bool> connected_{ false };

        std::string client_name_;
        std::string current_session_id_;

        json capabilities_;
        std::string capabilities_raw_;
        std::string server_kind_;
        std::atomic<bool> has_capability_info_{ false };

        mutable std::mutex exception_mutex_;
        std::string last_exception_;

        ChunkHandler on_chunk_;
        ThinkHandler on_think_;
        FinishHandler on_finish_;
        ErrorHandler on_error_;
        ClosedHandler on_closed_;

        DispatchMode dispatch_mode_ = DispatchMode::MainThread;

        // Queue shared by all non-Direct modes.
        std::queue<std::string> dispatch_queue_;
        std::mutex dispatch_mutex_;
        std::condition_variable dispatch_cv_;

        // Dispatcher thread, present only in Queued mode.
        std::thread dispatcher_thread_;
        bool dispatcher_running_ = false;

        // Held during every handler invocation.
        std::mutex io_mutex_;
    };

    // ============================================================================
    // Utility functions
    // ============================================================================

    bool buildImageAttachmentFromFile(const std::string& file_path,
        ImageAttachment& out,
        std::string& error);

    bool buildTextAttachmentFromFile(const std::string& file_path,
        TextAttachment& out,
        std::string& error);

    std::string guessImageMimeByExtension(const std::string& file_path);

    std::string base64Encode(const std::vector<std::uint8_t>& data);

} // namespace llm

#endif // LLM_CLIENT_HPP_INCLUDED