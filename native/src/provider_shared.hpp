// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
#pragma once
#include "nilwebview/provider_v1.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nilwebview {
using Clock = std::chrono::steady_clock;
enum class Operation { none, navigation, script };

struct Window {
    virtual ~Window() = default;
    std::int64_t id = 0;
    bool ready = false;
    bool closed = false;
    bool failed = false;
    Operation operation_kind = Operation::none;
    std::int64_t operation = 0;
    Clock::time_point deadline;
};

struct Provenance {
    std::int64_t source_kind = NILWV_SOURCE_UNKNOWN;
    std::int64_t frame_kind = NILWV_FRAME_UNKNOWN;
    std::string url;
    std::string origin;
};

struct Event {
    std::int64_t window = 0;
    std::int64_t kind = 0;
    std::int64_t sequence = 0;
    std::int64_t operation = 0;
    std::int64_t value_kind = NILWV_VALUE_NONE;
    std::int64_t failure_kind = NILWV_FAILURE_NONE;
    Provenance source;
    std::string payload_json;
    std::string detail;
};

// Helpers never borrow input beyond the call. JSON validation is bounded and
// preserves serialized text rather than converting it through floating point.
bool utf8(const std::string &) noexcept;
bool json_valid(const std::string &) noexcept;
std::string json_string(const std::string &);
std::string evaluation_script(const std::string &function_body);
const std::string &message_script();
bool input_text(nilwv_text_v1, std::string &, bool allow_empty = true) noexcept;
bool navigation_uri(const std::string &) noexcept;
void success(nilwv_result_v1 *, std::int64_t number = 1) noexcept;
void static_error(nilwv_result_v1 *, std::int32_t, const char *) noexcept;

// Provider subclasses retain toolkit objects; this class owns common handles,
// snapshots, limits and operation state. Providers pump their event loop before
// poll(), then call expire_operations(). Callbacks must catch exceptions and use
// callback_failed(), never unwind into a toolkit or enter application code.
class Context {
public:
    Context();
    virtual ~Context();
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    bool begin(nilwv_result_v1 *, bool allow_fatal = false) noexcept;
    void fail(nilwv_result_v1 *, std::int32_t, const std::string &) noexcept;
    void text(nilwv_result_v1 *, const std::string &);
    bool read_text(nilwv_text_v1, std::string &, nilwv_result_v1 *, bool allow_empty = true);

    bool add_window(const std::shared_ptr<Window> &, nilwv_result_v1 *);
    std::shared_ptr<Window> window(std::int64_t, nilwv_result_v1 *, bool require_ready = true);
    std::shared_ptr<Window> close(std::int64_t, nilwv_result_v1 *);
    // Call before closing toolkit objects; late callbacks then become inert.
    // Fixed storage makes teardown independent of another heap allocation.
    std::array<std::shared_ptr<Window>, NILWV_MAX_WINDOWS> clear() noexcept;
    bool live(const std::shared_ptr<Window> &) const noexcept;

    std::int64_t start(const std::shared_ptr<Window> &, Operation, nilwv_result_v1 *);
    void reject(const std::shared_ptr<Window> &, std::int64_t, nilwv_result_v1 *, const std::string &);
    void initialized(const std::shared_ptr<Window> &);
    void initialization_failed(const std::shared_ptr<Window> &, const std::string &);
    void navigation_finished(const std::shared_ptr<Window> &, std::int64_t,
                             bool okay, const std::string &url, const std::string &detail = {},
                             std::int64_t failure_kind = NILWV_FAILURE_NATIVE);
    // The encoded string is the result of evaluation_script(), not raw engine
    // JSON. Engine exceptions/errors should call script_failed() instead.
    void script_finished(const std::shared_ptr<Window> &, std::int64_t, const std::string &encoded);
    void script_failed(const std::shared_ptr<Window> &, std::int64_t, const std::string &,
                       std::int64_t failure_kind = NILWV_FAILURE_SCRIPT_EXCEPTION);
    void message(const std::shared_ptr<Window> &, const std::string &json, Provenance = {});
    void navigation_requested(const std::shared_ptr<Window> &, const std::string &url);
    void close_requested(const std::shared_ptr<Window> &);
    void provider_failed(const std::shared_ptr<Window> &, const std::string &);
    void callback_failed() noexcept;
    void expire_operations();

    void poll(std::int64_t window, nilwv_result_v1 *);
    void event_number(std::int64_t snapshot, const std::string &field, nilwv_result_v1 *);
    void event_text(std::int64_t snapshot, const std::string &field, nilwv_result_v1 *);
    void release_event(std::int64_t snapshot, nilwv_result_v1 *);

    bool owner_thread() const noexcept;
    bool fatal() const noexcept { return fatal_code_ != 0; }
    bool destroyed() const noexcept { return destroyed_; }

private:
    void enqueue(Event);
    bool matches(const std::shared_ptr<Window> &, std::int64_t, Operation) const noexcept;
    const Event *snapshot(std::int64_t, nilwv_result_v1 *);
    void fatal_error(std::int32_t) noexcept;
    static std::uint64_t bytes(const Event &) noexcept;
    std::thread::id owner_;
    std::unordered_map<std::int64_t, std::shared_ptr<Window>> windows_;
    std::deque<Event> queue_;
    std::unordered_map<std::int64_t, Event> snapshots_;
    std::uint64_t queued_bytes_ = 0;
    std::int32_t fatal_code_ = 0;
    bool destroyed_ = false;
    std::string result_text_;
};

template <typename State, typename Function>
void invoke(const std::shared_ptr<State> &state, nilwv_result_v1 *out,
            Function &&function, bool allow_fatal = false) noexcept {
    if (!out || out->size != sizeof(nilwv_result_v1)) return;
    if (!state) { static_error(out, NILWV_E_HANDLE, "WebView context is unavailable"); return; }
    if (!state->begin(out, allow_fatal)) return;
    try { function(*state); }
    catch (...) { state->callback_failed(); static_error(out, NILWV_E_NATIVE, "WebView provider operation failed"); }
}
} // namespace nilwebview
