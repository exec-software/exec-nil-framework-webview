// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
#include "provider_shared.hpp"
#include "win32_navigation.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

using namespace nilwebview;
namespace {
unsigned checks = 0;
void check(bool yes, const char *name) {
    ++checks;
    if (!yes) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
nilwv_result_v1 result() { return {sizeof(nilwv_result_v1), 0, 0, {nullptr, 0}}; }
std::shared_ptr<Window> add(Context &context, bool ready = true) {
    auto value = std::make_shared<Window>(); auto out = result();
    check(context.add_window(value, &out) && out.number == value->id, "allocate window");
    if (ready) { context.initialized(value); context.poll(value->id, &out); context.release_event(out.number, &out); }
    return value;
}
std::int64_t number(Context &context, std::int64_t event, const char *field) {
    auto out = result(); context.event_number(event, field, &out);
    check(out.code == 0, "numeric snapshot field"); return out.number;
}
std::string text(Context &context, std::int64_t event, const char *field) {
    auto out = result(); context.event_text(event, field, &out);
    check(out.code == 0, "text snapshot field"); return std::string(out.text.data ? out.text.data : "", out.text.size);
}
std::int64_t event(Context &context, std::int64_t window) {
    auto out = result(); context.poll(window, &out);
    check(out.code == 0 && out.number > 0, "poll event"); return out.number;
}
void release(Context &context, std::int64_t id) { auto out = result(); context.release_event(id, &out); check(out.code == 0, "release event"); }
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--script-fixtures") {
        std::cout << "{\"message\":" << json_string(message_script()) << ",\"evaluation\":"
                  << json_string(evaluation_script("/*NILWEBVIEW_BODY*/")) << "}\n";
        return 0;
    }
    const std::string html_prefix = "data:text/html;charset=utf-8;base64,";
    const std::pair<std::string, std::string> html_vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
        {"\xCE\xBB", "zrs="}, {"<p>\xCE\xBB</p>", "PHA+zrs8L3A+"},
        {"\xF0\x9F\x98\x80", "8J+YgA=="},
    };
    for (const auto &[html, encoded] : html_vectors)
        check(win32::html_navigation_uri(html) == html_prefix + encoded, "owned HTML exact navigation URI");
    const auto largest_html_uri = win32::html_navigation_uri(std::string(NILWV_MAX_TEXT_BYTES, 'x'));
    check(largest_html_uri.size() == html_prefix.size() + ((NILWV_MAX_TEXT_BYTES + 2) / 3) * 4 &&
          largest_html_uri.compare(0, html_prefix.size(), html_prefix) == 0 &&
          largest_html_uri.substr(largest_html_uri.size() - 4) == "eA==", "owned HTML navigation size boundary");
    bool html_overflow_rejected = false;
    try { win32::html_navigation_uri(std::string(NILWV_MAX_TEXT_BYTES + 1, 'x')); }
    catch (const std::length_error &) { html_overflow_rejected = true; }
    check(html_overflow_rejected, "owned HTML navigation overflow rejected");
    for (const auto &value : {"", "plain", "λ", "😀"}) check(utf8(value), "UTF-8 accepted");
    for (const auto &value : {std::string("x\0y", 3), std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3),
                              std::string("\xf4\x90\x80\x80", 4), std::string("\xc2", 1)}) check(!utf8(value), "UTF-8 rejected");
    for (const auto &value : {"null", "true", "false", "-1.25e+3", "[]", "{}", "[\"λ\",{\"x\":1}]", "\"\\u0000\""})
        check(json_valid(value), "JSON accepted");
    for (const auto &value : {"", "+1", "01", "1.", "1e", "[1,]", "{\"x\":}", "{x:1}", "null x", "\"\\q\"", "\"\n\""})
        check(!json_valid(value), "JSON rejected");
    check(json_valid(std::string(64, '[') + "0" + std::string(64, ']')), "JSON depth boundary");
    check(json_valid(std::string(33, '[') + "0" + std::string(33, ']')), "JSON exceeds configuration parser depth");
    check(json_valid("\"\\ud800\"") && json_valid("\"\\udfff\""), "JSON.stringify lone surrogate escapes preserved");
    check(!json_valid(std::string(65, '[') + "0" + std::string(65, ']')), "JSON depth rejection");
    check(json_string("a\n\"\\") == "\"a\\u000a\\\"\\\\\"", "JSON string escaping");
    std::string copied; check(input_text({nullptr, 0}, copied) && copied.empty(), "empty text");
    check(!input_text({nullptr, 1}, copied), "invalid pointer length");
    check(!input_text({"", NILWV_MAX_TEXT_BYTES + 1}, copied), "input length checked before copy");
    check(!input_text({nullptr, 0}, copied, false), "required text");
    for (const auto &value : {"http://localhost/", "https://example.org/a", "about:blank"}) check(navigation_uri(value), "navigation scheme accepted");
    for (const auto &value : {"file:///etc/passwd", "javascript:1", "data:text/html,x", "https:///host", "http://", "https://a\nb", "https://a\\b"})
        check(!navigation_uri(value), "navigation scheme rejected");

    Context context, other; auto out = result(); auto pending = add(context, false);
    check(!context.window(pending->id, &out) && out.code == NILWV_E_NOT_READY, "pending window not ready");
    check(!other.window(pending->id, &out) && out.code == NILWV_E_HANDLE, "foreign window rejected");
    std::string ready_detail = "owned runtime identity";
    context.initialized(pending, ready_detail); ready_detail.clear(); context.initialized(pending, "duplicate");
    const auto ready = event(context, pending->id);
    check(number(context, ready, "kind") == NILWV_EVENT_READY && text(context, ready, "detail") == "owned runtime identity", "ready event owns its diagnostic");
    check(number(context, ready, "window") == pending->id, "ready window identity");
    context.poll(pending->id, &out); check(out.code == 0 && out.number == 0, "no duplicate ready");
    other.event_number(ready, "kind", &out); check(out.code == NILWV_E_HANDLE, "foreign snapshot rejected");
    context.event_number(pending->id, "kind", &out); check(out.code == NILWV_E_HANDLE, "wrong handle kind rejected");
    context.event_number(ready, "missing", &out); check(out.code == NILWV_E_ARGUMENT, "unknown numeric field rejected");
    context.event_text(ready, "missing", &out); check(out.code == NILWV_E_ARGUMENT, "unknown text field rejected");
    release(context, ready); context.release_event(ready, &out); check(out.code == NILWV_E_HANDLE, "double release rejected");
    bool wrong_thread = false;
    std::thread foreign([&] { auto local = result(); wrong_thread = !context.begin(&local) && local.code == NILWV_E_THREAD; }); foreign.join();
    check(wrong_thread, "owner thread enforced");

    const auto operation = context.start(pending, Operation::navigation, &out);
    check(operation > 0, "navigation operation allocated");
    check(context.start(pending, Operation::script, &out) == 0 && out.code == NILWV_E_BUSY, "overlap rejected");
    context.navigation_finished(pending, operation + 1, true, "https://wrong.example/");
    context.poll(pending->id, &out); check(out.number == 0, "wrong operation completion ignored");
    context.navigation_finished(pending, operation, true, "https://example.org/\"x");
    const auto navigation = event(context, pending->id);
    check(number(context, navigation, "operation") == operation, "operation identity retained");
    check(text(context, navigation, "payload_json") == "\"https://example.org/\\\"x\"", "navigation JSON string");
    check(text(context, navigation, "source_url").empty(), "navigation not sender provenance");
    release(context, navigation);
    context.navigation_finished(pending, operation, true, "https://example.org/");
    context.poll(pending->id, &out); check(out.number == 0, "duplicate completion ignored");
    const auto cancelled_op = context.start(pending, Operation::navigation, &out);
    context.navigation_requested(pending, "https://redirect.example/");
    context.navigation_finished(pending, cancelled_op, false, "", "policy cancelled", NILWV_FAILURE_CANCELLED);
    const auto request = event(context, pending->id);
    check(number(context, request, "kind") == NILWV_EVENT_NAVIGATION_REQUESTED && number(context, request, "operation") == 0, "request separate from host completion");
    release(context, request);
    const auto cancelled = event(context, pending->id);
    check(number(context, cancelled, "operation") == cancelled_op && number(context, cancelled, "failure_kind") == NILWV_FAILURE_CANCELLED, "policy cancellation completion identified");
    release(context, cancelled);

    for (const auto &encoded : {"j:null", "u:", "j:{\"n\":9007199254740993}", "e:async", "e:serialization", "e:limit", "j:bad"}) {
        const auto op = context.start(pending, Operation::script, &out);
        context.script_finished(pending, op, encoded); const auto snap = event(context, pending->id);
        const bool okay = std::string(encoded).compare(0, 2, "j:") == 0 && json_valid(std::string(encoded).substr(2));
        check(number(context, snap, "kind") == ((okay || std::string(encoded) == "u:") ? NILWV_EVENT_SCRIPT_COMPLETED : NILWV_EVENT_SCRIPT_FAILED), "script completion classification");
        if (okay) check(text(context, snap, "payload_json") == std::string(encoded).substr(2), "serialized JSON preserved");
        if (std::string(encoded) == "u:") check(number(context, snap, "value_kind") == NILWV_VALUE_UNDEFINED, "undefined distinct");
        if (std::string(encoded) == "e:async") check(number(context, snap, "failure_kind") == NILWV_FAILURE_ASYNC_RESULT, "Promise rejection explicit");
        release(context, snap);
    }
    Provenance proof; proof.source_kind = NILWV_SOURCE_NATIVE_ORIGIN; proof.frame_kind = NILWV_FRAME_MAIN;
    proof.url = "https://example.org/page"; proof.origin = "https://example.org";
    context.message(pending, "{\"x\":1}", proof); const auto message = event(context, pending->id);
    check(text(context, message, "source_origin") == proof.origin, "native origin preserved");
    check(number(context, message, "frame_kind") == NILWV_FRAME_MAIN, "native frame preserved");
    context.message(pending, "2"); const auto unknown = event(context, pending->id);
    check(number(context, unknown, "source_kind") == NILWV_SOURCE_UNKNOWN && text(context, unknown, "source_url").empty(), "unknown provenance explicit");
    check(text(context, message, "payload_json") == "{\"x\":1}", "snapshot immutable across other delivery");
    context.close(pending->id, &out); check(out.code == 0 && pending->closed, "close succeeds");
    context.event_text(message, "payload_json", &out); check(out.code == NILWV_E_HANDLE, "close invalidates snapshots");
    context.close(pending->id, &out); check(out.code == NILWV_E_HANDLE, "double close rejected");
    context.message(pending, "1"); context.initialized(pending); check(!context.fatal(), "late callbacks inert");

    for (const auto operation_kind : {Operation::none, Operation::navigation, Operation::script}) {
        Context timed; auto window = add(timed, operation_kind != Operation::none);
        const auto op = operation_kind == Operation::none ? 0 : timed.start(window, operation_kind, &out);
        window->deadline = Clock::now() - std::chrono::milliseconds(1); timed.expire_operations();
        const auto timeout = event(timed, window->id);
        check(number(timed, timeout, "failure_kind") == NILWV_FAILURE_TIMEOUT && window->failed, "timeout terminal");
        check(number(timed, timeout, "operation") == op, "timeout operation identity");
        check(!timed.window(window->id, &out) && out.code == NILWV_E_NOT_READY, "timed window cannot restart");
        timed.initialized(window); timed.navigation_finished(window, op, true, "about:blank"); timed.script_finished(window, op, "j:1");
        timed.poll(window->id, &out); check(out.number == 0, "late timeout callbacks inert");
    }
    { Context bounded; for (unsigned i = 0; i < NILWV_MAX_WINDOWS; ++i) add(bounded); auto excess = std::make_shared<Window>();
      check(!bounded.add_window(excess, &out) && out.code == NILWV_E_LIMIT, "window bound"); }
    { Context bounded; auto window = add(bounded); for (unsigned i = 0; i < NILWV_MAX_EVENTS; ++i) bounded.message(window, "0");
      check(!bounded.fatal(), "event count boundary"); const auto retained = event(bounded, window->id); (void)retained;
      bounded.message(window, "1"); check(!bounded.begin(&out) && out.code == NILWV_E_QUEUE_OVERFLOW, "retained event counts against bound");
      check(bounded.begin(&out, true), "fatal context allows cleanup"); bounded.close(window->id, &out); check(out.code == 0, "fatal close"); }
    { Context bounded; auto window = add(bounded); const std::string large = "\"" + std::string(NILWV_MAX_TEXT_BYTES - 2, 'x') + "\"";
      for (unsigned i = 0; i < 7; ++i) bounded.message(window, large);
      check(!bounded.fatal(), "queue byte bound lower edge"); bounded.message(window, large);
      check(!bounded.begin(&out) && out.code == NILWV_E_QUEUE_OVERFLOW, "queue byte bound includes metadata"); }
    { Context invalid; auto window = add(invalid); Provenance lie; lie.url = "https://example.org"; invalid.message(window, "0", lie);
      check(!invalid.begin(&out) && out.code == NILWV_E_PROTOCOL, "untyped provenance rejected"); }
    { Context invalid; auto window = add(invalid); invalid.message(window, "invalid"); const auto failed = event(invalid, window->id);
      check(window->failed && number(invalid, failed, "kind") == NILWV_EVENT_PROVIDER_FAILED, "malformed message explicit failure"); }
    { auto owned = std::make_shared<Context>(); auto window = add(*owned); invoke(owned, &out, [](Context &) { throw 1; });
      check(out.code == NILWV_E_NATIVE && owned->fatal(), "exception contained and fatal"); const auto invalidated = owned->clear();
      check(invalidated[0] == window && window->closed && !owned->live(window), "clear invalidates ownership");
      check(!owned->begin(&out) && out.code == NILWV_E_HANDLE, "destroyed context rejected"); }
    std::cout << checks << " WebView shared checks passed\n";
}
