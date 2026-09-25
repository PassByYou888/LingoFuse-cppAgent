/**
 * @file llm_client.cpp
 * @brief Implementation of the C++ LLM client library.
 */

#include "llm_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace llm {

    // ============================================================================
    // Module-local helpers
    // ============================================================================

    namespace {

        constexpr const char* kLogPrefix = "[llm_client] ";

        void logInfo(const std::string& msg) {
            std::fprintf(stderr, "%s%s\n", kLogPrefix, msg.c_str());
        }

        std::string toLowerCopy(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            return s;
        }

    } // namespace

    // ============================================================================
    // Base64 encoding
    // ============================================================================

    std::string base64Encode(const std::vector<std::uint8_t>& data) {
        static const char* table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);

        std::size_t i = 0;
        const std::size_t n = data.size();

        while (i + 3 <= n) {
            const std::uint32_t v =
                (static_cast<std::uint32_t>(data[i]) << 16) |
                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                static_cast<std::uint32_t>(data[i + 2]);
            out.push_back(table[(v >> 18) & 0x3F]);
            out.push_back(table[(v >> 12) & 0x3F]);
            out.push_back(table[(v >> 6) & 0x3F]);
            out.push_back(table[v & 0x3F]);
            i += 3;
        }

        if (i + 1 == n) {
            const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
            out.push_back(table[(v >> 18) & 0x3F]);
            out.push_back(table[(v >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (i + 2 == n) {
            const std::uint32_t v =
                (static_cast<std::uint32_t>(data[i]) << 16) |
                (static_cast<std::uint32_t>(data[i + 1]) << 8);
            out.push_back(table[(v >> 18) & 0x3F]);
            out.push_back(table[(v >> 12) & 0x3F]);
            out.push_back(table[(v >> 6) & 0x3F]);
            out.push_back('=');
        }

        return out;
    }

    // ============================================================================
    // Utility: image MIME by extension
    // ============================================================================

    std::string guessImageMimeByExtension(const std::string& file_path) {
        const auto dot = file_path.find_last_of('.');
        if (dot == std::string::npos) {
            return kDefaultImageMime;
        }
        const std::string ext = toLowerCopy(file_path.substr(dot));
        if (ext == ".png")  return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".webp") return "image/webp";
        return kDefaultImageMime;
    }

    // ============================================================================
    // Utility: read raw bytes
    // ============================================================================

    namespace {

        bool readBinaryFile(const std::string& path,
            std::vector<std::uint8_t>& out,
            std::string& error) {
            out.clear();
            error.clear();

            std::ifstream f(path, std::ios::binary);
            if (!f) {
                error = "cannot open file: " + path;
                return false;
            }

            f.seekg(0, std::ios::end);
            const auto size = f.tellg();
            if (size < 0) {
                error = "cannot determine file size: " + path;
                return false;
            }
            f.seekg(0, std::ios::beg);

            out.resize(static_cast<std::size_t>(size));
            if (size > 0) {
                f.read(reinterpret_cast<char*>(out.data()),
                    static_cast<std::streamsize>(size));
                if (!f) {
                    out.clear();
                    error = "read failed: " + path;
                    return false;
                }
            }
            return true;
        }

        std::string decodeTextBytes(const std::vector<std::uint8_t>& bytes) {
            if (bytes.empty()) return {};

            auto is_valid_utf8 = [](const std::vector<std::uint8_t>& b) -> bool {
                std::size_t i = 0;
                const std::size_t n = b.size();
                while (i < n) {
                    const std::uint8_t c = b[i];
                    std::size_t extra = 0;
                    if (c < 0x80) {
                        ++i;
                        continue;
                    }
                    else if ((c & 0xE0) == 0xC0) {
                        extra = 1;
                    }
                    else if ((c & 0xF0) == 0xE0) {
                        extra = 2;
                    }
                    else if ((c & 0xF8) == 0xF0) {
                        extra = 3;
                    }
                    else {
                        return false;
                    }
                    if (i + extra >= n) return false;
                    for (std::size_t k = 1; k <= extra; ++k) {
                        if ((b[i + k] & 0xC0) != 0x80) return false;
                    }
                    i += extra + 1;
                }
                return true;
                };

            if (is_valid_utf8(bytes)) {
                return std::string(reinterpret_cast<const char*>(bytes.data()),
                    bytes.size());
            }
            return std::string(reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }

    } // namespace

    // ============================================================================
    // Attachment builders
    // ============================================================================

    bool buildImageAttachmentFromFile(const std::string& file_path,
        ImageAttachment& out,
        std::string& error) {
        out = ImageAttachment{};
        error.clear();

        if (file_path.empty()) {
            error = "empty file path";
            return false;
        }

        std::vector<std::uint8_t> raw;
        if (!readBinaryFile(file_path, raw, error)) {
            return false;
        }

        if (raw.empty()) {
            error = "image file is empty (0 bytes): " + file_path;
            return false;
        }

        const std::size_t b64_estimate = ((raw.size() + 2) / 3) * 4;
        if (b64_estimate > limits::kMaxImageB64PerFile) {
            std::ostringstream oss;
            oss << "image file would encode to about " << b64_estimate
                << " base64 chars, exceeding per-file limit "
                << limits::kMaxImageB64PerFile;
            error = oss.str();
            return false;
        }

        std::string name = file_path;
        const auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        if (name.size() > limits::kMaxNameLen) {
            name = name.substr(0, limits::kMaxNameLen);
        }

        out.name = name;
        out.mime = guessImageMimeByExtension(file_path);
        out.data_b64 = base64Encode(raw);
        return true;
    }

    bool buildTextAttachmentFromFile(const std::string& file_path,
        TextAttachment& out,
        std::string& error) {
        out = TextAttachment{};
        error.clear();

        if (file_path.empty()) {
            error = "empty file path";
            return false;
        }

        std::vector<std::uint8_t> raw;
        if (!readBinaryFile(file_path, raw, error)) {
            return false;
        }

        if (raw.size() > limits::kMaxTextBytesPerFile) {
            std::ostringstream oss;
            oss << "text file is " << raw.size()
                << " bytes, exceeding per-file limit "
                << limits::kMaxTextBytesPerFile;
            error = oss.str();
            return false;
        }

        std::string name = file_path;
        const auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        if (name.size() > limits::kMaxNameLen) {
            name = name.substr(0, limits::kMaxNameLen);
        }

        out.name = name;
        out.mime = kDefaultTextMime;
        out.text = decodeTextBytes(raw);
        return true;
    }

    // ============================================================================
    // Client: construction and destruction
    // ============================================================================

    Client::Client(const std::string& server_app,
        const std::string& endpoint,
        int timeout_ms)
        : server_app_(server_app),
        endpoint_(endpoint),
        timeout_ms_(timeout_ms) {
        // The default dispatch mode is MainThread: no dispatcher thread
        // is started. Callers that want Queued mode must call
        // setDispatchMode(DispatchMode::Queued) before connect().
    }

    Client::~Client() {
        try {
            disconnect();
        }
        catch (...) {
        }
        try {
            stopDispatcher();
        }
        catch (...) {
        }
    }

    // ============================================================================
    // Client: dispatch mode
    // ============================================================================

    void Client::setDispatchMode(DispatchMode mode) {
        if (connected_.load()) {
            // Changing the mode while connected is not supported: events
            // already in flight would be delivered through a mix of the
            // two paths. Retain the previous mode.
            return;
        }

        dispatch_mode_ = mode;

        if (mode == DispatchMode::Queued) {
            startDispatcher();
        }
        else {
            stopDispatcher();
        }
    }

    // ============================================================================
    // Client: dispatcher thread (Queued mode only)
    // ============================================================================

    void Client::startDispatcher() {
        if (dispatcher_thread_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(dispatch_mutex_);
            dispatcher_running_ = true;
        }
        dispatcher_thread_ = std::thread([this] { dispatcherLoop(); });
    }

    void Client::stopDispatcher() {
        {
            std::lock_guard<std::mutex> lk(dispatch_mutex_);
            if (!dispatcher_running_) {
                return;
            }
            dispatcher_running_ = false;
        }
        dispatch_cv_.notify_all();
        if (dispatcher_thread_.joinable()) {
            dispatcher_thread_.join();
        }
    }

    void Client::dispatcherLoop() {
        while (true) {
            std::string payload;
            {
                std::unique_lock<std::mutex> lk(dispatch_mutex_);
                dispatch_cv_.wait(lk, [this] {
                    return !dispatch_queue_.empty() || !dispatcher_running_;
                    });

                // If the mode changed while we were waiting, hand control
                // back to the main thread.
                if (dispatch_mode_ != DispatchMode::Queued) {
                    if (!dispatcher_running_) {
                        return;
                    }
                    lk.unlock();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                    continue;
                }

                if (dispatch_queue_.empty()) {
                    if (!dispatcher_running_) {
                        return;
                    }
                    continue;
                }

                payload = std::move(dispatch_queue_.front());
                dispatch_queue_.pop();
            }

            {
                std::lock_guard<std::mutex> io_lk(io_mutex_);
                dispatchPayload(payload);
            }
        }
    }

    void Client::postEvent(std::string payload) {
        {
            std::lock_guard<std::mutex> lk(dispatch_mutex_);
            dispatch_queue_.push(std::move(payload));
        }
        dispatch_cv_.notify_one();
    }

    // ============================================================================
    // Client: pumpEvents
    // ============================================================================

    std::size_t Client::pumpEvents(int timeout_ms) {
        std::size_t count = 0;

        // If a timeout was requested and the queue is currently empty,
        // wait for the first event. After the first event arrives we
        // drain the rest without waiting.
        if (timeout_ms > 0) {
            std::unique_lock<std::mutex> lk(dispatch_mutex_);
            if (dispatch_queue_.empty()) {
                dispatch_cv_.wait_for(
                    lk,
                    std::chrono::milliseconds(timeout_ms),
                    [this] { return !dispatch_queue_.empty(); });
            }
        }

        while (true) {
            std::string payload;
            {
                std::lock_guard<std::mutex> lk(dispatch_mutex_);
                if (dispatch_queue_.empty()) {
                    break;
                }
                payload = std::move(dispatch_queue_.front());
                dispatch_queue_.pop();
            }

            // Dispatch on the caller's thread. The ioMutex is held so
            // that main-thread output and handler output never
            // interleave. In MainThread mode this is usually redundant
            // (caller == handler thread), but it is still correct.
            {
                std::lock_guard<std::mutex> io_lk(io_mutex_);
                dispatchPayload(payload);
            }
            ++count;
        }

        return count;
    }

    // ============================================================================
    // Client: connect / disconnect
    // ============================================================================

    bool Client::connect(std::string& error) {
        error.clear();

        if (connected_.load()) {
            return true;
        }

        if (prepared_.load() || app_) {
            cleanupPartialConnect(false);
        }

        try {
            lingofuse::resetPrepare();
            lingofuse::setOption("Wait_Connection_ReadyOk", "True");
            lingofuse::setOption("Overlap_Connection", "True");

            if (lingofuse::prepareClient(endpoint_, nullptr) == -1) {
                error = "LF_PrepareClient returned -1 for endpoint " + endpoint_;
                cleanupPartialConnect(false);
                return false;
            }

            if (lingofuse::prepareDone() != 1) {
                error = "LF_PrepareDone failed";
                cleanupPartialConnect(false);
                return false;
            }

            prepared_.store(true);

            client_name_ = lingofuse::generateAppName();
            if (client_name_.empty()) {
                error = "generateAppName returned an empty string";
                cleanupPartialConnect(true);
                return false;
            }

            app_ = std::make_unique<lingofuse::App>(
                client_name_, "C++ LLM Client");

            if (!app_->registerNotify(
                std::string(api::kStream),
                "LLM stream callback",
                static_cast<void*>(this),
                &Client::notifyTrampoline)) {
                error = "failed to register notify callback for "
                    + std::string(api::kStream);
                cleanupPartialConnect(true);
                return false;
            }

            const int bound = app_->bind();
            if (bound == 0) {
                error = "LF_BindApp returned 0 (no free client available)";
                cleanupPartialConnect(true);
                return false;
            }

            connected_.store(true);

            std::string caps_json;
            std::string caps_error;
            if (!getApiCapabilities(caps_json, caps_error)) {
                logInfo("capability probe failed: " + caps_error);
            }

            logInfo("connected: client=" + client_name_
                + ", kind=" + server_kind_);
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("unexpected exception in connect: ") + e.what();
            cleanupPartialConnect(prepared_.load());
            return false;
        }
        catch (...) {
            error = "unexpected unknown exception in connect";
            cleanupPartialConnect(prepared_.load());
            return false;
        }
    }

    void Client::disconnect() {
        if (!prepared_.load() && !connected_.load() && !app_) {
            return;
        }
        logInfo("disconnecting");
        cleanupPartialConnect(true);
    }

    void Client::cleanupPartialConnect(bool exit_main_thread) {
        if (app_) {
            try {
                app_.reset();
            }
            catch (...) {
            }
        }

        if (exit_main_thread && prepared_.load()) {
            try {
                lingofuse::exitMainThread();
            }
            catch (...) {
            }
        }

        resetCapabilityState();

        prepared_.store(false);
        connected_.store(false);
        client_name_.clear();
        current_session_id_.clear();
    }

    void Client::resetCapabilityState() {
        capabilities_.clear();
        capabilities_raw_.clear();
        server_kind_.clear();
        has_capability_info_.store(false);
    }

    // ============================================================================
    // Client: notify trampoline and dispatcher
    // ============================================================================

    void LF_CDECL Client::notifyTrampoline(void* trigger, void* input) {
        auto* self = static_cast<Client*>(trigger);
        if (self == nullptr) {
            return;
        }
        try {
            self->onNotify(static_cast<TDataHnd>(input));
        }
        catch (...) {
        }
    }

    void Client::onNotify(TDataHnd input) {
        if (input == nullptr) {
            return;
        }

        std::string payload;
        try {
            const std::vector<std::uint8_t> raw =
                lingofuse::io::read_string_bytes(input);
            if (raw.empty()) {
                return;
            }
            payload.assign(reinterpret_cast<const char*>(raw.data()),
                raw.size());
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(exception_mutex_);
            last_exception_ = std::string("notify read: ") + e.what();
            return;
        }
        catch (...) {
            std::lock_guard<std::mutex> lk(exception_mutex_);
            last_exception_ = "notify read: unknown exception";
            return;
        }

        if (dispatch_mode_ == DispatchMode::Direct) {
            std::lock_guard<std::mutex> io_lk(io_mutex_);
            dispatchPayload(payload);
        }
        else {
            // MainThread and Queued both enqueue. In MainThread mode the
            // user's main thread will drain via pumpEvents(). In Queued
            // mode the dispatcher thread will drain.
            postEvent(std::move(payload));
        }
    }

    void Client::dispatchPayload(const std::string& payload) {
        try {
            json j = json::parse(payload);
            if (!j.is_object() || !j.contains("type")) {
                return;
            }

            const std::string msg_type = j.value("type", std::string());
            const std::string session_id = j.value("session_id", std::string());

            if (msg_type == "closed") {
                const std::string reason = j.value("reason", std::string());
                if (on_closed_) {
                    on_closed_(session_id, reason);
                }
            }
            else if (msg_type == "chunk") {
                const std::string t = j.value("text", std::string());
                if (!t.empty() && on_chunk_) {
                    on_chunk_(session_id, t);
                }
            }
            else if (msg_type == "think") {
                const std::string t = j.value("text", std::string());
                if (!t.empty() && on_think_) {
                    on_think_(session_id, t);
                }
            }
            else if (msg_type == "finish") {
                const std::string reason = j.value("reason", std::string());
                if (on_finish_) {
                    on_finish_(session_id, reason);
                }
            }
            else if (msg_type == "error") {
                const std::string msg = j.value("message", std::string());
                if (on_error_) {
                    on_error_(session_id, msg);
                }
            }
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(exception_mutex_);
            last_exception_ = std::string("dispatch: ") + e.what();
        }
        catch (...) {
            std::lock_guard<std::mutex> lk(exception_mutex_);
            last_exception_ = "dispatch: unknown exception";
        }
    }

    std::string Client::takeLastException() {
        std::lock_guard<std::mutex> lk(exception_mutex_);
        std::string s = last_exception_;
        last_exception_.clear();
        return s;
    }

    // ============================================================================
    // Client: low-level call API
    // ============================================================================

    bool Client::callApi(const std::string& api_name,
        const std::string& request_bytes,
        std::string& response_bytes,
        std::string& error) {
        response_bytes.clear();
        error.clear();

        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        try {
            lingofuse::DataHandle param(api_name);
            lingofuse::io::write_string_bytes(
                param.get(), request_bytes.data(), request_bytes.size());

            TDataHnd raw = LF_Call(
                server_app_.c_str(),
                param.get(),
                static_cast<std::uint64_t>(timeout_ms_));

            if (raw == nullptr) {
                error = "LF_Call returned a null handle for API " + api_name;
                return false;
            }

            lingofuse::DataHandle response(raw, true);

            if (response.size() == 0) {
                error = "empty response from API " + api_name
                    + " (timeout or target unreachable)";
                return false;
            }

            response.seek(0);
            const std::vector<std::uint8_t> bytes =
                lingofuse::io::read_string_bytes(response.get());

            if (bytes.empty()) {
                error = "empty response payload from API " + api_name;
                return false;
            }

            response_bytes.assign(
                reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("callApi(") + api_name + "): " + e.what();
            return false;
        }
        catch (...) {
            error = std::string("callApi(") + api_name
                + "): unknown exception";
            return false;
        }
    }

    // ============================================================================
    // Client: session management
    // ============================================================================

    bool Client::createSession(std::string& session_id, std::string& error) {
        return createSession(std::string(), session_id, error);
    }

    bool Client::createSession(const std::string& system_message,
        std::string& session_id,
        std::string& error) {
        session_id.clear();
        error.clear();

        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        try {
            json req;
            req["client_name"] = client_name_;
            if (!system_message.empty()) {
                req["system_message"] = system_message;
            }

            const std::string req_bytes =
                lingofuse::io::dumps_json(req);

            std::string resp_bytes;
            if (!callApi(api::kCreateSession, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "create_session response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value(
                    "error",
                    std::string("create_session failed"));
                return false;
            }

            const std::string new_id = resp.value("session_id", std::string());
            if (new_id.empty()) {
                error = "server did not return session_id";
                return false;
            }

            session_id = new_id;
            current_session_id_ = new_id;
            logInfo("session created: " + new_id
                + " (system_message="
                + std::to_string(system_message.size()) + " chars)");
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("createSession: ") + e.what();
            return false;
        }
    }

    bool Client::closeSession(const std::string& session_id,
        bool cancel_running,
        std::string& error) {
        error.clear();

        if (session_id.empty()) {
            error = "closeSession: empty session_id";
            return false;
        }

        try {
            json req;
            req["session_id"] = session_id;
            req["cancel_running"] = cancel_running;

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kCloseSession, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "close_session response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error",
                    std::string("close_session failed"));
                return false;
            }

            if (current_session_id_ == session_id) {
                current_session_id_.clear();
            }
            logInfo("session closed: " + session_id);
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("closeSession: ") + e.what();
            return false;
        }
    }

    bool Client::cancelSession(const std::string& session_id,
        std::string& error) {
        error.clear();

        if (session_id.empty()) {
            error = "cancelSession: empty session_id";
            return false;
        }

        try {
            json req;
            req["session_id"] = session_id;

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kCancelSession, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "cancel_session response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error",
                    std::string("cancel_session failed"));
                return false;
            }

            logInfo("cancel requested for session: " + session_id);
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("cancelSession: ") + e.what();
            return false;
        }
    }

    bool Client::listSessions(std::string& sessions_json,
        std::string& error) {
        sessions_json.clear();
        error.clear();

        try {
            json req;
            req["client_name"] = client_name_;

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kListSessions, req_bytes, resp_bytes, error)) {
                return false;
            }

            sessions_json = resp_bytes;
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("listSessions: ") + e.what();
            return false;
        }
    }

    // ============================================================================
    // Client: attachment helpers (static)
    // ============================================================================

    bool Client::populateAttachments(const TextAttachments& texts,
        const ImageAttachments& images,
        json& attachments_array,
        std::string& error) {
        error.clear();
        attachments_array = json::array();

        std::size_t total_text = 0;
        for (const auto& t : texts) {
            const std::size_t n = t.text.size();
            if (n > limits::kMaxTextBytesPerFile) {
                std::ostringstream oss;
                oss << "text attachment '" << t.name << "' is " << n
                    << " bytes, exceeding per-file limit "
                    << limits::kMaxTextBytesPerFile;
                error = oss.str();
                return false;
            }
            total_text += n;
        }
        if (total_text > limits::kMaxTextBytesTotal) {
            std::ostringstream oss;
            oss << "cumulative text size " << total_text
                << " exceeds total limit " << limits::kMaxTextBytesTotal;
            error = oss.str();
            return false;
        }

        std::size_t total_b64 = 0;
        for (const auto& im : images) {
            const std::size_t n = im.data_b64.size();
            if (n == 0) {
                error = "image attachment '" + im.name
                    + "' has empty data_b64";
                return false;
            }
            if (n > limits::kMaxImageB64PerFile) {
                std::ostringstream oss;
                oss << "image attachment '" << im.name << "' is " << n
                    << " base64 chars, exceeding per-file limit "
                    << limits::kMaxImageB64PerFile;
                error = oss.str();
                return false;
            }
            total_b64 += n;
        }
        if (total_b64 > limits::kMaxImageB64Total) {
            std::ostringstream oss;
            oss << "cumulative image size " << total_b64
                << " exceeds total limit " << limits::kMaxImageB64Total;
            error = oss.str();
            return false;
        }

        for (const auto& t : texts) {
            json entry;
            entry["kind"] = "text";
            entry["name"] = t.name.empty()
                ? std::string(kDefaultAttachmentName) : t.name;
            entry["mime"] = t.mime.empty()
                ? std::string(kDefaultTextMime) : t.mime;
            entry["text"] = t.text;
            attachments_array.push_back(std::move(entry));
        }

        for (const auto& im : images) {
            json entry;
            entry["kind"] = "image";
            entry["name"] = im.name.empty()
                ? std::string(kDefaultAttachmentName) : im.name;
            entry["mime"] = im.mime.empty()
                ? std::string(kDefaultImageMime) : im.mime;
            entry["data_b64"] = im.data_b64;
            attachments_array.push_back(std::move(entry));
        }

        return true;
    }

    // ============================================================================
    // Client: schema envelope builder (static)
    // ============================================================================

    std::string Client::buildSchemaResponseFormat(
        const std::string& schema_name,
        const std::string& schema_json,
        bool strict,
        std::string& error) {
        error.clear();

        if (schema_name.empty()) {
            error = "buildSchemaResponseFormat: empty schema name";
            return {};
        }
        if (schema_json.empty()) {
            error = "buildSchemaResponseFormat: empty schema JSON";
            return {};
        }

        try {
            json schema_body = json::parse(schema_json);

            json envelope;
            envelope["type"] = "json_schema";
            envelope["json_schema"]["name"] = schema_name;
            envelope["json_schema"]["strict"] = strict;
            envelope["json_schema"]["schema"] = std::move(schema_body);

            return lingofuse::io::dumps_json(envelope);
        }
        catch (const std::exception& e) {
            error = std::string("buildSchemaResponseFormat: ")
                + e.what();
            return {};
        }
    }

    // ============================================================================
    // Client: generate (no attachments, no schema)
    // ============================================================================

    bool Client::generate(const std::string& content,
        const std::string& prompt,
        std::string& session_id,
        std::string& error) {
        error.clear();

        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        try {
            json req;
            req["content"] = content;
            req["prompt"] = prompt;

            if (!session_id.empty()) {
                req["session_id"] = session_id;
            }
            else if (!current_session_id_.empty()) {
                req["session_id"] = current_session_id_;
            }
            else {
                req["client_name"] = client_name_;
            }

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kGenerate, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "generate response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error", std::string("generate failed"));
                return false;
            }

            const std::string new_id = resp.value("session_id", std::string());
            if (new_id.empty()) {
                error = "server did not return session_id";
                return false;
            }

            session_id = new_id;
            current_session_id_ = new_id;
            logInfo("generate queued: session=" + new_id);
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("generate: ") + e.what();
            return false;
        }
    }

    // ============================================================================
    // Client: generate with attachments
    // ============================================================================

    bool Client::generateWithAttachments(const std::string& content,
        const std::string& prompt,
        const TextAttachments& texts,
        const ImageAttachments& images,
        std::string& session_id,
        std::string& error) {
        error.clear();

        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        if (texts.empty() && images.empty()) {
            return generate(content, prompt, session_id, error);
        }

        try {
            json req;
            req["content"] = content;
            req["prompt"] = prompt;

            if (!session_id.empty()) {
                req["session_id"] = session_id;
            }
            else if (!current_session_id_.empty()) {
                req["session_id"] = current_session_id_;
            }
            else {
                req["client_name"] = client_name_;
            }

            json attachments = json::array();
            if (!populateAttachments(texts, images, attachments, error)) {
                return false;
            }
            req["attachments"] = std::move(attachments);

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kGenerate, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "generate response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error", std::string("generate failed"));
                return false;
            }

            const std::string new_id = resp.value("session_id", std::string());
            if (new_id.empty()) {
                error = "server did not return session_id";
                return false;
            }

            session_id = new_id;
            current_session_id_ = new_id;
            logInfo("generate queued with attachments: session=" + new_id
                + ", texts=" + std::to_string(texts.size())
                + ", images=" + std::to_string(images.size()));
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("generateWithAttachments: ") + e.what();
            return false;
        }
    }

    // ============================================================================
    // Client: generate with a single text file
    // ============================================================================

    bool Client::generateWithTextFile(const std::string& content,
        const std::string& prompt,
        const std::string& file_path,
        std::string& session_id,
        std::string& error) {
        TextAttachment att;
        if (!buildTextAttachmentFromFile(file_path, att, error)) {
            return false;
        }

        TextAttachments texts;
        texts.push_back(std::move(att));
        return generateWithAttachments(content, prompt, texts,
            ImageAttachments{}, session_id, error);
    }

    // ============================================================================
    // Client: generate with a single image file
    // ============================================================================

    bool Client::generateWithImageFile(const std::string& content,
        const std::string& prompt,
        const std::string& file_path,
        std::string& session_id,
        std::string& error) {
        ImageAttachment att;
        if (!buildImageAttachmentFromFile(file_path, att, error)) {
            return false;
        }

        ImageAttachments images;
        images.push_back(std::move(att));
        return generateWithAttachments(content, prompt, TextAttachments{},
            images, session_id, error);
    }

    // ============================================================================
    // Client: generate with the current session
    // ============================================================================

    bool Client::generateCurrent(const std::string& content,
        const std::string& prompt,
        std::string& error) {
        std::string sid = current_session_id_;
        return generate(content, prompt, sid, error);
    }

    // ============================================================================
    // Client: unified combined sender
    // ============================================================================

    bool Client::sendGenerateCombined(const std::string& content,
        const std::string& prompt,
        const TextAttachments& texts,
        const ImageAttachments& images,
        const std::string& response_format_json,
        std::string& session_id,
        std::string& error) {
        error.clear();

        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        try {
            json req;
            req["content"] = content;
            req["prompt"] = prompt;

            if (!session_id.empty()) {
                req["session_id"] = session_id;
            }
            else if (!current_session_id_.empty()) {
                req["session_id"] = current_session_id_;
            }
            else {
                req["client_name"] = client_name_;
            }

            if (!texts.empty() || !images.empty()) {
                json attachments = json::array();
                if (!populateAttachments(texts, images, attachments, error)) {
                    return false;
                }
                req["attachments"] = std::move(attachments);
            }

            if (!response_format_json.empty()) {
                req["options"]["response_format"] =
                    json::parse(response_format_json);
            }

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kGenerate, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "generate response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error", std::string("generate failed"));
                return false;
            }

            const std::string new_id = resp.value("session_id", std::string());
            if (new_id.empty()) {
                error = "server did not return session_id";
                return false;
            }

            session_id = new_id;
            current_session_id_ = new_id;
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("sendGenerateCombined: ") + e.what();
            return false;
        }
    }

    // ============================================================================
    // Client: Structured Output
    // ============================================================================

    bool Client::generateStructured(const std::string& content,
        const std::string& prompt,
        const std::string& response_format_json,
        std::string& session_id,
        std::string& error) {
        if (response_format_json.empty()) {
            error = "generateStructured: empty response_format";
            return false;
        }
        return sendGenerateCombined(content, prompt,
            TextAttachments{}, ImageAttachments{},
            response_format_json,
            session_id, error);
    }

    bool Client::generateWithJsonSchema(const std::string& content,
        const std::string& prompt,
        const std::string& schema_name,
        const std::string& schema_json,
        bool strict,
        std::string& session_id,
        std::string& error) {
        const std::string envelope = buildSchemaResponseFormat(
            schema_name, schema_json, strict, error);
        if (envelope.empty()) {
            return false;
        }
        return sendGenerateCombined(content, prompt,
            TextAttachments{}, ImageAttachments{},
            envelope, session_id, error);
    }

    bool Client::generateWithImageFileAndSchema(const std::string& content,
        const std::string& prompt,
        const std::string& file_path,
        const std::string& schema_name,
        const std::string& schema_json,
        bool strict,
        std::string& session_id,
        std::string& error) {
        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        const std::string envelope = buildSchemaResponseFormat(
            schema_name, schema_json, strict, error);
        if (envelope.empty()) {
            return false;
        }

        ImageAttachment att;
        if (!buildImageAttachmentFromFile(file_path, att, error)) {
            return false;
        }

        ImageAttachments images;
        images.push_back(std::move(att));
        return sendGenerateCombined(content, prompt,
            TextAttachments{}, images,
            envelope, session_id, error);
    }

    bool Client::generateWithAttachmentsAndSchema(
        const std::string& content,
        const std::string& prompt,
        const TextAttachments& texts,
        const ImageAttachments& images,
        const std::string& schema_name,
        const std::string& schema_json,
        bool strict,
        std::string& session_id,
        std::string& error) {
        if (!connected_.load()) {
            error = "not connected to LingoFuse service";
            return false;
        }

        const std::string envelope = buildSchemaResponseFormat(
            schema_name, schema_json, strict, error);
        if (envelope.empty()) {
            return false;
        }

        return sendGenerateCombined(content, prompt, texts, images,
            envelope, session_id, error);
    }

    // ============================================================================
    // Client: server-wide settings
    // ============================================================================

    bool Client::setSystemMessage(const std::string& message,
        std::string& error) {
        error.clear();

        if (hasCapabilityInfo() && !llmSupported(api::kSetSystemMessage)) {
            const std::string kind =
                server_kind_.empty() ? "unknown" : server_kind_;
            error = "set_system_message is not supported by this server "
                "(kind=" + kind + "). The system message is fixed at "
                "session creation time. Pass it to createSession, or "
                "close the current session and create a new one with "
                "the desired system message.";
            return false;
        }

        try {
            json req;
            req["content"] = message;

            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kSetSystemMessage, req_bytes, resp_bytes,
                error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "set_system_message response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error",
                    std::string("set_system_message failed"));
                return false;
            }

            logInfo("global system message updated");
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("setSystemMessage: ") + e.what();
            return false;
        }
    }

    bool Client::health(std::string& health_json, std::string& error) {
        health_json.clear();
        error.clear();

        try {
            json req = json::object();
            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kHealth, req_bytes, resp_bytes, error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "health response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error", std::string("health failed"));
                return false;
            }

            health_json = resp_bytes;
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("health: ") + e.what();
            return false;
        }
    }

    // ============================================================================
    // Client: capability discovery
    // ============================================================================

    bool Client::getApiCapabilities(std::string& capabilities_json,
        std::string& error) {
        capabilities_json.clear();
        error.clear();

        resetCapabilityState();

        try {
            json req = json::object();
            const std::string req_bytes = lingofuse::io::dumps_json(req);
            std::string resp_bytes;
            if (!callApi(api::kGetCapabilities, req_bytes, resp_bytes,
                error)) {
                return false;
            }

            json resp = json::parse(resp_bytes);
            if (!resp.is_object() || !resp.contains("code")) {
                error = "get_api_capabilities response missing 'code'";
                return false;
            }
            if (resp.value("code", -1) != 0) {
                error = resp.value("error",
                    std::string("get_api_capabilities failed"));
                return false;
            }

            if (!resp.contains("capabilities")
                || !resp["capabilities"].is_object()) {
                error = "get_api_capabilities response missing "
                    "'capabilities' object";
                return false;
            }

            capabilities_ = resp["capabilities"];
            server_kind_ = resp.value("server_kind", std::string());
            capabilities_raw_ = resp_bytes;
            has_capability_info_.store(true);

            capabilities_json = resp_bytes;
            return true;
        }
        catch (const std::exception& e) {
            error = std::string("getApiCapabilities: ") + e.what();
            return false;
        }
    }

    bool Client::llmSupported(const std::string& api_name) const {
        if (!has_capability_info_.load()) {
            return false;
        }
        if (!capabilities_.is_object()) {
            return false;
        }
        if (!capabilities_.contains(api_name)) {
            return false;
        }
        const auto& v = capabilities_[api_name];
        if (!v.is_number_integer()) {
            return false;
        }
        return v.get<int>() == 1;
    }

    bool Client::isToolBridge() const {
        return llmSupported("tools") && llmSupported("tool_calls");
    }

    bool Client::hasVision() const {
        return llmSupported("vision");
    }

    bool Client::hasAttachments() const {
        return llmSupported("attachments");
    }

} // namespace llm