// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
#include "provider_shared.hpp"
#include "nilwebview/json_v1.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace nilwebview {
namespace {
std::atomic<std::int64_t> next_handle{1};
std::int64_t handle() noexcept {
    auto current = next_handle.load(std::memory_order_relaxed);
    while (current < std::numeric_limits<std::int64_t>::max()) {
        if (next_handle.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) return current;
    }
    return 0;
}


Event event_for(const std::shared_ptr<Window> &window, std::int64_t kind) {
    Event event; event.window = window->id; event.kind = kind; return event;
}
} // namespace

bool utf8(const std::string &text) noexcept { return json_v1::valid_utf8(text); }
bool json_valid(const std::string &text) noexcept { return json_v1::valid_json(text); }

std::string json_string(const std::string &text) {
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char byte : text) {
        if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte < 0x20) { result += "\\u00"; result += hex[byte >> 4]; result += hex[byte & 15]; }
        else result += static_cast<char>(byte);
    }
    return result + '"';
}

std::string evaluation_script(const std::string &body) {
    return "(()=>{const value=(function(){\n" + body +
        "\n})();if(value===undefined)return 'u:';try{"
        "if(value!==null&&(typeof value==='object'||typeof value==='function')"
        "&&typeof value.then==='function')return 'e:async';"
        "const encoded=JSON.stringify(value);if(typeof encoded!=='string')return 'e:serialization';"
        "if(encoded.length>1048576)return 'e:limit';return 'j:'+encoded;"
        "}catch(error){return 'e:serialization';}})()";
}

const std::string &message_script() {
    static const std::string script =
        "(()=>{if(window!==window.top)return;const encode=JSON.stringify;"
        "const post=value=>{const text=encode(value);if(typeof text!=='string')"
        "throw new TypeError('WebView message must serialize to JSON');"
        "if(text.length>1048576)throw new RangeError('WebView message exceeds limit');"
        "if(window.chrome&&window.chrome.webview)window.chrome.webview.postMessage(text);"
        "else window.webkit.messageHandlers.nilwebview.postMessage(text);};"
        "Object.defineProperty(window,'nilwebview',{value:Object.freeze({post}),"
        "writable:false,configurable:false});})()";
    return script;
}

bool input_text(nilwv_text_v1 input, std::string &output, bool allow_empty) noexcept {
    if (input.size > NILWV_MAX_TEXT_BYTES || (!input.data && input.size) || (!allow_empty && !input.size)) return false;
    try { output.assign(input.data ? input.data : "", static_cast<std::size_t>(input.size)); }
    catch (...) { return false; }
    return utf8(output);
}

bool navigation_uri(const std::string &uri) noexcept {
    if (uri == "about:blank") return true;
    std::size_t start = 0;
    if (uri.compare(0, 7, "http://") == 0) start = 7;
    else if (uri.compare(0, 8, "https://") == 0) start = 8;
    else return false;
    if (start == uri.size() || uri[start] == '/' || uri[start] == '?' || uri[start] == '#') return false;
    return std::none_of(uri.begin(), uri.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f || c == '\\'; });
}

void success(nilwv_result_v1 *out, std::int64_t number) noexcept {
    if (!out || out->size != sizeof(*out)) return;
    out->code = 0; out->number = number; out->text = {nullptr, 0};
}
void static_error(nilwv_result_v1 *out, std::int32_t code, const char *message) noexcept {
    if (!out || out->size != sizeof(*out)) return;
    out->code = code; out->number = 0; out->text = {message, std::strlen(message)};
}

Context::Context() : owner_(std::this_thread::get_id()) {}
Context::~Context() { clear(); }
bool Context::owner_thread() const noexcept { return owner_ == std::this_thread::get_id(); }

bool Context::begin(nilwv_result_v1 *out, bool allow_fatal) noexcept {
    if (!out || out->size != sizeof(*out)) return false;
    success(out);
    if (!owner_thread()) { static_error(out, NILWV_E_THREAD, "WebView calls require their owning UI thread"); return false; }
    result_text_.clear();
    if (destroyed_) { static_error(out, NILWV_E_HANDLE, "WebView context is closed"); return false; }
    if (fatal_code_ && !allow_fatal) {
        static_error(out, fatal_code_, "WebView context failed; close its resources and configure a new context");
        return false;
    }
    return true;
}
void Context::fail(nilwv_result_v1 *out, std::int32_t code, const std::string &message) noexcept {
    if (!out || out->size != sizeof(*out)) return;
    try {
        result_text_ = message;
        out->code = code; out->number = 0; out->text = {result_text_.data(), result_text_.size()};
    } catch (...) { static_error(out, code, "WebView provider operation failed"); }
}
void Context::text(nilwv_result_v1 *out, const std::string &value) {
    result_text_ = value;
    success(out, 0); out->text = {result_text_.data(), result_text_.size()};
}
bool Context::read_text(nilwv_text_v1 input, std::string &output, nilwv_result_v1 *out, bool allow_empty) {
    if (input_text(input, output, allow_empty)) return true;
    fail(out, NILWV_E_ARGUMENT, "WebView text must be bounded UTF-8 without embedded NUL"); return false;
}

bool Context::add_window(const std::shared_ptr<Window> &window, nilwv_result_v1 *out) {
    if (!window || window->id || destroyed_ || fatal()) { fail(out, NILWV_E_HANDLE, "Invalid new WebView window"); return false; }
    if (windows_.size() >= NILWV_MAX_WINDOWS) { fail(out, NILWV_E_LIMIT, "WebView window limit reached"); return false; }
    const auto id = handle();
    if (!id) { fail(out, NILWV_E_LIMIT, "WebView handle space exhausted"); return false; }
    window->id = id; window->deadline = Clock::now() + std::chrono::milliseconds(NILWV_OPERATION_TIMEOUT_MS);
    windows_.emplace(id, window); success(out, id); return true;
}
std::shared_ptr<Window> Context::window(std::int64_t id, nilwv_result_v1 *out, bool require_ready) {
    const auto found = windows_.find(id);
    if (found == windows_.end() || found->second->closed) { fail(out, NILWV_E_HANDLE, "Invalid or closed WebView window"); return {}; }
    if (require_ready && (!found->second->ready || found->second->failed)) {
        fail(out, NILWV_E_NOT_READY, "WebView window is not ready; failed windows must be closed"); return {};
    }
    return found->second;
}
bool Context::live(const std::shared_ptr<Window> &window) const noexcept {
    if (!window || destroyed_ || window->closed) return false;
    const auto found = windows_.find(window->id);
    return found != windows_.end() && found->second == window;
}
std::uint64_t Context::bytes(const Event &event) noexcept {
    return sizeof(Event) + event.payload_json.size() + event.detail.size() + event.source.url.size() + event.source.origin.size();
}
std::shared_ptr<Window> Context::close(std::int64_t id, nilwv_result_v1 *out) {
    auto value = window(id, out, false);
    if (!value) return {};
    value->closed = true; value->operation = 0; value->operation_kind = Operation::none;
    windows_.erase(id);
    for (auto entry = queue_.begin(); entry != queue_.end();) {
        if (entry->window == id) { queued_bytes_ -= bytes(*entry); entry = queue_.erase(entry); }
        else ++entry;
    }
    for (auto entry = snapshots_.begin(); entry != snapshots_.end();) {
        if (entry->second.window == id) { queued_bytes_ -= bytes(entry->second); entry = snapshots_.erase(entry); }
        else ++entry;
    }
    success(out); return value;
}
std::array<std::shared_ptr<Window>, NILWV_MAX_WINDOWS> Context::clear() noexcept {
    std::array<std::shared_ptr<Window>, NILWV_MAX_WINDOWS> values;
    destroyed_ = true;
    std::size_t index = 0;
    for (auto &entry : windows_) {
        entry.second->closed = true; entry.second->operation = 0; entry.second->operation_kind = Operation::none;
        values[index++] = std::move(entry.second);
    }
    windows_.clear(); queue_.clear(); snapshots_.clear(); queued_bytes_ = 0;
    return values;
}

std::int64_t Context::start(const std::shared_ptr<Window> &window, Operation kind, nilwv_result_v1 *out) {
    if (!live(window) || !window->ready || window->failed || fatal()) {
        fail(out, NILWV_E_NOT_READY, "WebView window is not ready"); return 0;
    }
    if (window->operation) { fail(out, NILWV_E_BUSY, "WebView already has a pending operation"); return 0; }
    const auto id = handle();
    if (!id || kind == Operation::none) { fail(out, NILWV_E_LIMIT, "WebView operation cannot be started"); return 0; }
    window->operation = id; window->operation_kind = kind;
    window->deadline = Clock::now() + std::chrono::milliseconds(NILWV_OPERATION_TIMEOUT_MS);
    success(out, id); return id;
}
void Context::reject(const std::shared_ptr<Window> &window, std::int64_t id, nilwv_result_v1 *out, const std::string &detail) {
    if (live(window) && window->operation == id) { window->operation = 0; window->operation_kind = Operation::none; }
    fail(out, NILWV_E_NATIVE, detail);
}
bool Context::matches(const std::shared_ptr<Window> &window, std::int64_t id, Operation kind) const noexcept {
    return live(window) && !window->failed && id > 0 && window->operation == id && window->operation_kind == kind;
}
void Context::initialized(const std::shared_ptr<Window> &window) {
    if (!live(window) || window->ready || window->failed) return;
    window->ready = true; enqueue(event_for(window, NILWV_EVENT_READY));
}
void Context::initialization_failed(const std::shared_ptr<Window> &window, const std::string &detail) {
    if (!live(window) || window->ready || window->failed) return;
    window->failed = true;
    auto event = event_for(window, NILWV_EVENT_INITIALIZATION_FAILED);
    event.failure_kind = NILWV_FAILURE_NATIVE; event.detail = detail; enqueue(std::move(event));
}
void Context::navigation_finished(const std::shared_ptr<Window> &window, std::int64_t id,
                                  bool okay, const std::string &url, const std::string &detail, std::int64_t failure) {
    if (!matches(window, id, Operation::navigation)) return;
    window->operation = 0; window->operation_kind = Operation::none;
    auto event = event_for(window, okay ? NILWV_EVENT_NAVIGATION_COMPLETED : NILWV_EVENT_NAVIGATION_FAILED);
    event.operation = id; event.value_kind = NILWV_VALUE_JSON; event.payload_json = json_string(url);
    if (!okay) { event.failure_kind = failure; event.detail = detail; }
    enqueue(std::move(event));
}
void Context::script_failed(const std::shared_ptr<Window> &window, std::int64_t id, const std::string &detail, std::int64_t failure) {
    if (!matches(window, id, Operation::script)) return;
    window->operation = 0; window->operation_kind = Operation::none;
    auto event = event_for(window, NILWV_EVENT_SCRIPT_FAILED);
    event.operation = id; event.failure_kind = failure; event.detail = detail; enqueue(std::move(event));
}
void Context::script_finished(const std::shared_ptr<Window> &window, std::int64_t id, const std::string &encoded) {
    if (!matches(window, id, Operation::script)) return;
    if (encoded.size() > NILWV_MAX_TEXT_BYTES + 2 || encoded == "e:limit") {
        script_failed(window, id, "WebView script result exceeds payload limit", NILWV_FAILURE_PAYLOAD_LIMIT); return;
    }
    if (encoded == "e:async") {
        script_failed(window, id, "WebView v1 does not await Promise or thenable results", NILWV_FAILURE_ASYNC_RESULT); return;
    }
    if (encoded != "u:" && (encoded.compare(0, 2, "j:") != 0 || !json_valid(encoded.substr(2)))) {
        script_failed(window, id, "WebView script result could not be serialized to JSON", NILWV_FAILURE_SERIALIZATION); return;
    }
    window->operation = 0; window->operation_kind = Operation::none;
    auto event = event_for(window, NILWV_EVENT_SCRIPT_COMPLETED); event.operation = id;
    event.value_kind = encoded == "u:" ? NILWV_VALUE_UNDEFINED : NILWV_VALUE_JSON;
    if (event.value_kind == NILWV_VALUE_JSON) event.payload_json = encoded.substr(2);
    enqueue(std::move(event));
}
void Context::message(const std::shared_ptr<Window> &window, const std::string &json, Provenance source) {
    if (!live(window) || window->failed) return;
    if (!json_valid(json)) { provider_failed(window, "WebView page message is not bounded valid JSON"); return; }
    auto event = event_for(window, NILWV_EVENT_PAGE_MESSAGE);
    event.value_kind = NILWV_VALUE_JSON; event.payload_json = json; event.source = std::move(source); enqueue(std::move(event));
}
void Context::navigation_requested(const std::shared_ptr<Window> &window, const std::string &url) {
    if (!live(window) || window->failed) return;
    auto event = event_for(window, NILWV_EVENT_NAVIGATION_REQUESTED);
    event.value_kind = NILWV_VALUE_JSON; event.payload_json = json_string(url); enqueue(std::move(event));
}
void Context::close_requested(const std::shared_ptr<Window> &window) {
    if (live(window)) enqueue(event_for(window, NILWV_EVENT_CLOSE_REQUESTED));
}
void Context::provider_failed(const std::shared_ptr<Window> &window, const std::string &detail) {
    if (!live(window) || window->failed) return;
    if (!window->ready) { initialization_failed(window, detail); return; }
    if (window->operation_kind == Operation::script) script_failed(window, window->operation, detail, NILWV_FAILURE_NATIVE);
    else if (window->operation_kind == Operation::navigation) navigation_finished(window, window->operation, false, "", detail);
    window->failed = true;
    auto event = event_for(window, NILWV_EVENT_PROVIDER_FAILED);
    event.failure_kind = NILWV_FAILURE_NATIVE; event.detail = detail; enqueue(std::move(event));
}
void Context::fatal_error(std::int32_t code) noexcept {
    if (!fatal_code_) fatal_code_ = code;
    queue_.clear(); snapshots_.clear(); queued_bytes_ = 0;
}
void Context::callback_failed() noexcept { fatal_error(NILWV_E_NATIVE); }
void Context::expire_operations() {
    const auto now = Clock::now();
    for (const auto &entry : windows_) {
        const auto &window = entry.second;
        if (window->closed || window->failed || now < window->deadline || (window->ready && !window->operation)) continue;
        auto event = event_for(window, !window->ready ? NILWV_EVENT_INITIALIZATION_FAILED :
            window->operation_kind == Operation::script ? NILWV_EVENT_SCRIPT_FAILED : NILWV_EVENT_NAVIGATION_FAILED);
        event.operation = window->operation; event.failure_kind = NILWV_FAILURE_TIMEOUT;
        event.detail = "WebView operation timed out; close and recreate the window";
        window->failed = true; window->operation = 0; window->operation_kind = Operation::none;
        enqueue(std::move(event));
    }
}
void Context::enqueue(Event event) {
    if (destroyed_ || fatal()) return;
    for (const auto *text : {&event.payload_json, &event.detail, &event.source.url, &event.source.origin}) {
        if (text->size() > NILWV_MAX_TEXT_BYTES || !utf8(*text)) { fatal_error(NILWV_E_PROTOCOL); return; }
    }
    if ((event.source.source_kind == NILWV_SOURCE_UNKNOWN && (!event.source.url.empty() || !event.source.origin.empty())) ||
        (event.source.source_kind == NILWV_SOURCE_NATIVE_URL && (event.source.url.empty() || !event.source.origin.empty())) ||
        (event.source.source_kind == NILWV_SOURCE_NATIVE_ORIGIN && event.source.origin.empty()) ||
        event.source.source_kind < 0 || event.source.source_kind > 2 || event.source.frame_kind < 0 || event.source.frame_kind > 2) {
        fatal_error(NILWV_E_PROTOCOL); return;
    }
    const auto size = bytes(event);
    if (queue_.size() + snapshots_.size() >= NILWV_MAX_EVENTS || size > NILWV_MAX_QUEUED_BYTES - queued_bytes_) {
        fatal_error(NILWV_E_QUEUE_OVERFLOW); return;
    }
    event.sequence = handle();
    if (!event.sequence) { fatal_error(NILWV_E_LIMIT); return; }
    queue_.push_back(std::move(event)); queued_bytes_ += size;
}
void Context::poll(std::int64_t id, nilwv_result_v1 *out) {
    if (fatal()) { fail(out, fatal_code_, "WebView event queue failed; context teardown is required"); return; }
    if (!window(id, out, false)) return;
    const auto found = std::find_if(queue_.begin(), queue_.end(), [id](const Event &event) { return event.window == id; });
    if (found == queue_.end()) { success(out, 0); return; }
    const auto snapshot_id = handle();
    if (!snapshot_id) { fail(out, NILWV_E_LIMIT, "WebView snapshot handle space exhausted"); return; }
    snapshots_.emplace(snapshot_id, std::move(*found)); queue_.erase(found); success(out, snapshot_id);
}
const Event *Context::snapshot(std::int64_t id, nilwv_result_v1 *out) {
    const auto found = snapshots_.find(id);
    if (found == snapshots_.end()) { fail(out, NILWV_E_HANDLE, "Invalid or released WebView event snapshot"); return nullptr; }
    return &found->second;
}
void Context::event_number(std::int64_t id, const std::string &field, nilwv_result_v1 *out) {
    const auto *event = snapshot(id, out); if (!event) return;
    if (field == "window") success(out, event->window);
    else if (field == "kind") success(out, event->kind);
    else if (field == "sequence") success(out, event->sequence);
    else if (field == "operation") success(out, event->operation);
    else if (field == "value_kind") success(out, event->value_kind);
    else if (field == "source_kind") success(out, event->source.source_kind);
    else if (field == "frame_kind") success(out, event->source.frame_kind);
    else if (field == "failure_kind") success(out, event->failure_kind);
    else fail(out, NILWV_E_ARGUMENT, "Unknown numeric WebView event field");
}
void Context::event_text(std::int64_t id, const std::string &field, nilwv_result_v1 *out) {
    const auto *event = snapshot(id, out); if (!event) return;
    if (field == "payload_json") text(out, event->payload_json);
    else if (field == "source_url") text(out, event->source.url);
    else if (field == "source_origin") text(out, event->source.origin);
    else if (field == "detail") text(out, event->detail);
    else fail(out, NILWV_E_ARGUMENT, "Unknown text WebView event field");
}
void Context::release_event(std::int64_t id, nilwv_result_v1 *out) {
    const auto found = snapshots_.find(id);
    if (found == snapshots_.end()) { fail(out, NILWV_E_HANDLE, "Invalid or released WebView event snapshot"); return; }
    queued_bytes_ -= bytes(found->second); snapshots_.erase(found); success(out);
}
} // namespace nilwebview
