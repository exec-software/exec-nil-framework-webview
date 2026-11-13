// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
// GTK4 / WebKitGTK 6 only. Message provenance is deliberately unavailable.
#include "provider_shared.hpp"
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <jsc/jsc.h>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
using namespace nilwebview;
struct LinuxContext;

void cancel_download(WebKitNetworkSession *, WebKitDownload *download, gpointer) noexcept {
    // WebKit's unhandled default writes to the personal Downloads directory.
    // Downloads are outside this ABI; cancel before a destination is selected.
    webkit_download_cancel(download);
}

struct LinuxWindow final : Window {
    std::weak_ptr<LinuxContext> context;
    GtkWindow *shell = nullptr;
    WebKitWebView *view = nullptr;
    WebKitUserContentManager *manager = nullptr;
    GCancellable *script_cancel = nullptr;
    std::int64_t script_operation = 0;
    std::int64_t navigation_operation = 0;
    bool initializing = true;
    bool initial_policy = false;
    bool policy_cancelled = false;
    bool load_started = false;
    bool stopped = false;
    std::string requested_uri;

    void cancel_native() noexcept {
        if (stopped) return;
        stopped = true;
        initial_policy = false;
        if (script_cancel) g_cancellable_cancel(script_cancel);
        if (view) webkit_web_view_stop_loading(view);
    }
    void shutdown() noexcept {
        cancel_native();
        if (manager) {
            webkit_user_content_manager_unregister_script_message_handler(manager, "nilwebview", nullptr);
            webkit_user_content_manager_remove_all_scripts(manager);
        }
        if (shell) gtk_window_destroy(shell);
        if (script_cancel) { g_object_unref(script_cancel); script_cancel = nullptr; }
        if (view) { g_object_unref(view); view = nullptr; }
        if (shell) { g_object_unref(shell); shell = nullptr; }
        if (manager) { g_object_unref(manager); manager = nullptr; }
    }
    ~LinuxWindow() override { shutdown(); }
};

struct LinuxContext final : Context {
    WebKitNetworkSession *network = nullptr;
    std::array<std::weak_ptr<LinuxWindow>, NILWV_MAX_WINDOWS> native_windows;

    LinuxContext() : network(webkit_network_session_new_ephemeral()) {
        if (!network) throw std::runtime_error("WebKit ephemeral session creation failed");
        g_signal_connect(network, "download-started", G_CALLBACK(cancel_download), nullptr);
    }
    ~LinuxContext() override {
        if (network) g_object_unref(network);
    }
    void remember(const std::shared_ptr<LinuxWindow> &window) {
        for (auto &entry : native_windows) {
            if (entry.expired()) { entry = window; return; }
        }
        throw std::runtime_error("WebKit window inventory exhausted");
    }
    void cancel_failed() noexcept {
        for (auto &entry : native_windows) {
            if (auto window = entry.lock()) {
                if (window->failed || fatal()) window->cancel_native();
            }
        }
    }
};

bool process_main_thread() noexcept {
    return static_cast<pid_t>(syscall(SYS_gettid)) == getpid();
}

// A native engine allocation may precede this conversion; provider-owned text
// copies and retained events are always capped and UTF-8 checked.
bool copy_native(const char *value, std::string &out) {
    if (!value) { out.clear(); return true; }
    const auto length = strnlen(value, static_cast<std::size_t>(NILWV_MAX_TEXT_BYTES) + 1);
    if (length > NILWV_MAX_TEXT_BYTES) return false;
    std::string copy(value, length);
    if (!utf8(copy)) return false;
    out = std::move(copy);
    return true;
}

std::string native_detail(const GError *error, const char *fallback) {
    std::string text;
    if (error && copy_native(error->message, text) && !text.empty()) return text;
    return fallback;
}

bool copy_script_string(JSCValue *value, std::string &out,
                        std::uint64_t maximum = NILWV_MAX_TEXT_BYTES) {
    if (!value || !jsc_value_is_string(value)) return false;
    GBytes *bytes = jsc_value_to_string_as_bytes(value);
    if (!bytes) return false;
    std::unique_ptr<GBytes, decltype(&g_bytes_unref)> held(bytes, g_bytes_unref);
    gsize size = 0;
    const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
    if (size > maximum || (!data && size)) return false;
    out.assign(data ? data : "", size);
    return utf8(out);
}

using WeakWindow = std::weak_ptr<LinuxWindow>;
void delete_signal_state(gpointer data, GClosure *) noexcept {
    delete static_cast<WeakWindow *>(data);
}

// Toolkit callbacks retain a weak window state, never a raw context pointer.
// Every handler catches allocation/validation failures before returning to C.
template <typename Function>
void callback(gpointer data, Function &&function) noexcept {
    auto window = static_cast<WeakWindow *>(data)->lock();
    if (!window) return;
    auto state = window->context.lock();
    if (!state || !state->owner_thread() || !state->live(window) || window->failed) return;
    try { function(*state, window); }
    catch (...) { state->callback_failed(); }
}

void connect_signal(gpointer object, const char *name, GCallback function,
                    const std::shared_ptr<LinuxWindow> &window) {
    auto state = std::make_unique<WeakWindow>(window);
    const auto id = g_signal_connect_data(object, name, function, state.get(), delete_signal_state,
                                         static_cast<GConnectFlags>(0));
    if (!id) throw std::runtime_error("WebKit signal connection failed");
    state.release();
}

void load_changed(WebKitWebView *, WebKitLoadEvent event, gpointer data) noexcept {
    callback(data, [&](LinuxContext &state, const auto &window) {
        if (event == WEBKIT_LOAD_STARTED) { window->load_started = true; return; }
        if (event != WEBKIT_LOAD_FINISHED || !window->load_started) return;
        window->load_started = false;
        window->initial_policy = false;
        if (window->initializing) {
            window->initializing = false;
            state.initialized(window);
        } else if (window->navigation_operation) {
            const auto operation = window->navigation_operation;
            window->navigation_operation = 0;
            std::string uri;
            if (!copy_native(webkit_web_view_get_uri(window->view), uri)) {
                state.navigation_finished(window, operation, false, {}, "Navigation URL exceeds the provider text bound");
            } else {
                state.navigation_finished(window, operation, !window->policy_cancelled, uri,
                    window->policy_cancelled ? "Navigation requires explicit application authorization" : "",
                    window->policy_cancelled ? NILWV_FAILURE_CANCELLED : NILWV_FAILURE_NATIVE);
            }
        }
    });
}

gboolean load_failed(WebKitWebView *, WebKitLoadEvent, const char *, GError *error, gpointer data) noexcept {
    callback(data, [&](LinuxContext &state, const auto &window) {
        window->load_started = false;
        window->initial_policy = false;
        const auto detail = native_detail(error, "WebKit navigation failed");
        if (window->initializing) {
            window->initializing = false;
            state.initialization_failed(window, detail);
        } else if (window->navigation_operation) {
            const auto operation = window->navigation_operation;
            window->navigation_operation = 0;
            state.navigation_finished(window, operation, false, {}, detail,
                window->policy_cancelled ? NILWV_FAILURE_CANCELLED : NILWV_FAILURE_NATIVE);
        }
    });
    return TRUE; // Suppress WebKit's alternate error-page navigation.
}

gboolean decide_policy(WebKitWebView *, WebKitPolicyDecision *decision,
                       WebKitPolicyDecisionType type, gpointer data) noexcept {
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    bool allowed = false;
    bool cancel_active_navigation = false;
    callback(data, [&](LinuxContext &state, const auto &window) {
        auto *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        auto *request = action ? webkit_navigation_action_get_request(action) : nullptr;
        std::string uri;
        if (!request || !copy_native(webkit_uri_request_get_uri(request), uri)) {
            state.provider_failed(window, "Navigation request has invalid or oversized text");
            return;
        }
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            if (window->initial_policy && uri == window->requested_uri) {
                window->initial_policy = false;
                allowed = true;
            }
        }
        if (!allowed) {
            // GTK6's baseline action API cannot reliably identify the target
            // frame. Cancel uninitiated child-frame actions too; frame remains
            // unknown. Target URL is a request, never sender provenance.
            state.navigation_requested(window, uri);
            if (window->navigation_operation) {
                window->policy_cancelled = true;
                window->initial_policy = false;
                cancel_active_navigation = true;
            }
        }
    });
    if (allowed) webkit_policy_decision_use(decision);
    else webkit_policy_decision_ignore(decision);
    if (cancel_active_navigation) {
        callback(data, [](LinuxContext &, const auto &window) { webkit_web_view_stop_loading(window->view); });
    }
    return TRUE;
}

void page_message(WebKitUserContentManager *, JSCValue *value, gpointer data) noexcept {
    callback(data, [&](LinuxContext &state, const auto &window) {
        std::string json;
        if (!copy_script_string(value, json)) {
            state.provider_failed(window, "Page message must be bounded UTF-8 JSON text");
            return;
        }
        // No sender URL/origin/frame is exposed by this native GTK6 signal.
        state.message(window, json, {});
    });
}

gboolean script_dialog(WebKitWebView *, WebKitScriptDialog *dialog, gpointer) noexcept {
    // Do not enter WebKitGTK's default native-dialog handler. These synchronous
    // defaults match the other providers and cannot block the host poll loop.
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        webkit_script_dialog_confirm_set_confirmed(dialog, FALSE);
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        webkit_script_dialog_prompt_set_text(dialog, nullptr);
        break;
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    }
    // No extra reference is retained; WebKit closes the dialog when signal
    // emission releases it. Returning TRUE suppresses its native default UI.
    return TRUE;
}

gboolean close_requested(GtkWindow *, gpointer data) noexcept {
    callback(data, [&](LinuxContext &state, const auto &window) { state.close_requested(window); });
    return TRUE; // The application decides when to invalidate its window.
}

void script_close(WebKitWebView *, gpointer data) noexcept {
    callback(data, [&](LinuxContext &state, const auto &window) { state.close_requested(window); });
}

void web_process_terminated(WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) noexcept {
    callback(data, [&](LinuxContext &state, const auto &window) {
        if (window->initializing) {
            window->initializing = false;
            state.initialization_failed(window, "WebKit process terminated during initialization");
        } else {
            state.provider_failed(window, "WebKit web process terminated");
        }
        window->cancel_native();
    });
}

struct ScriptCompletion {
    WeakWindow window;
    std::int64_t operation;
};

void script_completed(GObject *object, GAsyncResult *result, gpointer data) noexcept {
    std::unique_ptr<ScriptCompletion> completion(static_cast<ScriptCompletion *>(data));
    GError *error = nullptr;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    auto window = completion->window.lock();
    auto state = window ? window->context.lock() : nullptr;
    if (state && state->owner_thread() && state->live(window)) {
        if (window->script_operation == completion->operation) {
            window->script_operation = 0;
            if (window->script_cancel) { g_object_unref(window->script_cancel); window->script_cancel = nullptr; }
        }
        try {
            if (error || !value) {
                state->script_failed(window, completion->operation,
                    native_detail(error, "WebKit script evaluation failed"));
            } else {
                std::string encoded;
                if (!copy_script_string(value, encoded, NILWV_MAX_TEXT_BYTES + 2)) {
                    state->script_failed(window, completion->operation,
                        "Script result violates the provider envelope bound", NILWV_FAILURE_PAYLOAD_LIMIT);
                } else {
                    state->script_finished(window, completion->operation, encoded);
                }
            }
        } catch (...) { state->callback_failed(); }
    }
    if (value) g_object_unref(value);
    if (error) g_error_free(error);
}

std::shared_ptr<LinuxWindow> get_window(LinuxContext &state, std::int64_t id,
                                     nilwv_result_v1 *out, bool ready = true) {
    return std::static_pointer_cast<LinuxWindow>(state.window(id, out, ready));
}
} // namespace

struct nilwv_context_v1 {
    std::shared_ptr<LinuxContext> state;
};

namespace {
void NILWV_CALL probe(nilwv_result_v1 *out) noexcept {
    success(out); // Linking already resolved the GTK4/WebKitGTK6 ABI; no display is touched.
}

void NILWV_CALL context_create(nilwv_context_v1 **created, nilwv_result_v1 *out) noexcept {
    if (!out || out->size != sizeof(nilwv_result_v1)) return;
    if (created) *created = nullptr;
    if (!created) { static_error(out, NILWV_E_ARGUMENT, "Missing WebView context output"); return; }
    if (!process_main_thread()) { static_error(out, NILWV_E_THREAD, "GTK WebView requires the process main thread"); return; }
    if (g_main_context_get_thread_default() && g_main_context_get_thread_default() != g_main_context_default()) {
        static_error(out, NILWV_E_THREAD, "GTK WebView requires the default UI main context"); return;
    }
    if (!gtk_init_check()) { static_error(out, NILWV_E_UNAVAILABLE, "GTK4 display initialization failed"); return; }
    try {
        auto context = std::make_unique<nilwv_context_v1>();
        context->state = std::make_shared<LinuxContext>();
        *created = context.release();
        success(out);
    } catch (...) { static_error(out, NILWV_E_NATIVE, "WebKit context initialization failed"); }
}

void NILWV_CALL context_destroy(nilwv_context_v1 *context) noexcept {
    if (!context || !context->state || !context->state->owner_thread()) return;
    auto state = context->state;
    for (auto &base : state->clear()) if (base) std::static_pointer_cast<LinuxWindow>(base)->shutdown();
    delete context;
}

void NILWV_CALL window_create(nilwv_context_v1 *context, nilwv_text_v1 title,
                             std::int64_t width, std::int64_t height, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        std::string copied_title;
        if (!state.read_text(title, copied_title, out)) return;
        if (width < 1 || height < 1 || width > 16384 || height > 16384) {
            state.fail(out, NILWV_E_ARGUMENT, "Window dimensions must be between 1 and 16384"); return;
        }
        auto window = std::make_shared<LinuxWindow>();
        window->context = context->state;
        if (!state.add_window(window, out)) return;
        try {
            state.remember(window);
            window->shell = GTK_WINDOW(gtk_window_new());
            if (!window->shell) throw std::runtime_error("GTK window creation failed");
            g_object_ref_sink(window->shell);
            gtk_window_set_title(window->shell, copied_title.c_str());
            gtk_window_set_default_size(window->shell, static_cast<int>(width), static_cast<int>(height));
            window->manager = webkit_user_content_manager_new();
            if (!window->manager) throw std::runtime_error("WebKit message manager creation failed");
            connect_signal(window->manager, "script-message-received::nilwebview", G_CALLBACK(page_message), window);
            if (!webkit_user_content_manager_register_script_message_handler(window->manager, "nilwebview", nullptr))
                throw std::runtime_error("WebKit message channel registration failed");
            auto *script = webkit_user_script_new(message_script().c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
            if (!script) throw std::runtime_error("WebKit message bridge creation failed");
            webkit_user_content_manager_add_script(window->manager, script);
            webkit_user_script_unref(script);
            window->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", state.network,
                                                       "user-content-manager", window->manager, nullptr));
            if (!window->view) throw std::runtime_error("WebKit view creation failed");
            g_object_ref_sink(window->view);
            gtk_window_set_child(window->shell, GTK_WIDGET(window->view));
            connect_signal(window->shell, "close-request", G_CALLBACK(close_requested), window);
            connect_signal(window->view, "close", G_CALLBACK(script_close), window);
            connect_signal(window->view, "script-dialog", G_CALLBACK(script_dialog), window);
            connect_signal(window->view, "load-changed", G_CALLBACK(load_changed), window);
            connect_signal(window->view, "load-failed", G_CALLBACK(load_failed), window);
            connect_signal(window->view, "decide-policy", G_CALLBACK(decide_policy), window);
            connect_signal(window->view, "web-process-terminated", G_CALLBACK(web_process_terminated), window);
            window->requested_uri = "about:blank";
            window->initial_policy = true;
            webkit_web_view_load_html(window->view, "<!doctype html><meta charset=utf-8><title></title>", nullptr);
        } catch (...) {
            state.initialization_failed(window, "GTK4/WebKitGTK6 window initialization failed");
            window->shutdown();
        }
        success(out, window->id);
    });
}

void NILWV_CALL show(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        if (auto window = get_window(state, id, out)) { gtk_window_present(window->shell); success(out); }
    });
}

void NILWV_CALL navigate(nilwv_context_v1 *context, std::int64_t id,
                        nilwv_text_v1 input, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        auto window = get_window(state, id, out);
        std::string uri;
        if (!window || !state.read_text(input, uri, out, false)) return;
        if (!navigation_uri(uri)) { state.fail(out, NILWV_E_ARGUMENT, "Unsupported navigation URI"); return; }
        // Encode non-ASCII UTF-8 octets and spaces while preserving URI syntax
        // and existing escapes. The engine then canonicalizes the same request
        // object used for both authorization matching and the actual load.
        gchar *escaped = g_uri_escape_string(uri.c_str(), ":/?#[]@!$&'()*+,;=%", FALSE);
        if (!escaped) { state.fail(out, NILWV_E_NATIVE, "URI encoding failed"); return; }
        auto *request = webkit_uri_request_new(escaped);
        g_free(escaped);
        if (!request) { state.fail(out, NILWV_E_NATIVE, "URI request creation failed"); return; }
        std::unique_ptr<WebKitURIRequest, decltype(&g_object_unref)> held(request, g_object_unref);
        if (!copy_native(webkit_uri_request_get_uri(request), uri) || uri.empty()) {
            state.fail(out, NILWV_E_ARGUMENT, "Navigation URI cannot be represented within the text bound"); return;
        }
        const auto operation = state.start(window, Operation::navigation, out);
        if (!operation) return;
        window->navigation_operation = operation;
        window->requested_uri = std::move(uri);
        window->initial_policy = true;
        window->policy_cancelled = false;
        window->load_started = false;
        webkit_web_view_load_request(window->view, request);
    });
}

void NILWV_CALL load_html(nilwv_context_v1 *context, std::int64_t id,
                         nilwv_text_v1 input, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        auto window = get_window(state, id, out);
        std::string html;
        if (!window || !state.read_text(input, html, out)) return;
        std::string requested_uri = "about:blank";
        const auto operation = state.start(window, Operation::navigation, out);
        if (!operation) return;
        window->navigation_operation = operation;
        window->requested_uri = std::move(requested_uri);
        window->initial_policy = true;
        window->policy_cancelled = false;
        window->load_started = false;
        webkit_web_view_load_html(window->view, html.c_str(), nullptr);
    });
}

void NILWV_CALL evaluate_function(nilwv_context_v1 *context, std::int64_t id,
                                 nilwv_text_v1 input, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        auto window = get_window(state, id, out);
        std::string body;
        if (!window || !state.read_text(input, body, out)) return;
        auto script = evaluation_script(body);
        auto completion = std::make_unique<ScriptCompletion>();
        completion->window = window;
        auto *cancel = g_cancellable_new();
        if (!cancel) { state.fail(out, NILWV_E_NATIVE, "Script cancellation state creation failed"); return; }
        std::unique_ptr<GCancellable, decltype(&g_object_unref)> held(cancel, g_object_unref);
        const auto operation = state.start(window, Operation::script, out);
        if (!operation) return;
        completion->operation = operation;
        window->script_operation = operation;
        window->script_cancel = held.release();
        webkit_web_view_evaluate_javascript(window->view, script.c_str(), static_cast<gssize>(script.size()),
                                           nullptr, nullptr, window->script_cancel, script_completed, completion.release());
    });
}

void NILWV_CALL poll(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        if (!get_window(state, id, out, false)) return;
        const auto until = Clock::now() + std::chrono::milliseconds(2);
        for (unsigned iteration = 0; iteration < 64 && Clock::now() < until; ++iteration) {
            if (!g_main_context_iteration(nullptr, FALSE)) break;
            if (state.fatal()) break;
        }
        state.expire_operations();
        state.cancel_failed();
        state.poll(id, out);
    });
}

void NILWV_CALL event_number(nilwv_context_v1 *context, std::int64_t id,
                            nilwv_text_v1 field, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        std::string name;
        if (state.read_text(field, name, out, false)) state.event_number(id, name, out);
    }, true);
}

void NILWV_CALL event_text(nilwv_context_v1 *context, std::int64_t id,
                          nilwv_text_v1 field, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        std::string name;
        if (state.read_text(field, name, out, false)) state.event_text(id, name, out);
    }, true);
}

void NILWV_CALL release_event(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) { state.release_event(id, out); }, true);
}

void NILWV_CALL close(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    invoke(context ? context->state : nullptr, out, [&](LinuxContext &state) {
        if (auto window = state.close(id, out)) std::static_pointer_cast<LinuxWindow>(window)->shutdown();
    }, true);
}

const nilwv_api_v1 api = {
    NILWV_ABI_V1, sizeof(nilwv_api_v1), 0, "gtk4-webkitgtk6", "1",
    probe, context_create, context_destroy, window_create, show, navigate, load_html,
    evaluate_function, poll, event_number, event_text, release_event, close
};
} // namespace

extern "C" NILWV_EXPORT const nilwv_api_v1 *NILWV_CALL
nilwebview_provider_v1_get_api(std::uint32_t version, std::uint32_t minimum_size) {
    if (version != NILWV_ABI_V1 || minimum_size > sizeof(nilwv_api_v1)) return nullptr;
    return &api;
}
