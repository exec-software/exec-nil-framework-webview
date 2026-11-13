// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
// Compile with ARC. No provider callback enters Matrix, and all toolkit objects
// belong to the creating process main thread.
#include "provider_shared.hpp"
#include <algorithm>
#include <memory>
#include <utility>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <WebKit/WebKit.h>

@class NilWebViewDelegate;
namespace nilwebview::macos {
struct MacWindow final : Window {
    NSWindow *native_window = nil;
    WKWebView *view = nil;
    NilWebViewDelegate *delegate = nil;
    WKNavigation *navigation = nil;
    std::int64_t navigation_operation = 0;
    bool bootstrap = true;
    bool admit_request = false;
    bool policy_cancelled = false;
    bool fragment_navigation = false;
    NSURL *requested_url = nil;
};
struct MacContext final : Context {
    // The store is shared only by windows in this context, never with Safari or
    // another framework context. WebKit owns its temporary process resources.
    WKWebsiteDataStore *store = nil;
    std::vector<std::weak_ptr<MacWindow>> native_windows;
};
NSString *to_ns(const std::string &value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}
bool from_ns(NSString *value, std::string &output, std::size_t limit = NILWV_MAX_TEXT_BYTES) {
    if (!value) { output.clear(); return true; }
    const auto length = [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    if (length > limit) return false;
    const char *data = [value UTF8String];
    if (!data) return false;
    output.assign(data, length); return utf8(output);
}
void dispose(const std::shared_ptr<MacWindow> &window) {
    if (!window) return;
    window->admit_request = false;
    window->fragment_navigation = false;
    [window->view.configuration.userContentController removeScriptMessageHandlerForName:@"nilwebview"];
    window->view.navigationDelegate = nil;
    window->view.UIDelegate = nil;
    [window->view stopLoading];
    window->native_window.delegate = nil;
    [window->native_window orderOut:nil];
    [window->native_window close];
    window->native_window.contentView = nil;
    window->navigation = nil; window->requested_url = nil;
    window->view = nil; window->delegate = nil; window->native_window = nil;
}
template<class F>
void callback(const std::weak_ptr<MacContext> &weak_context,
              const std::weak_ptr<MacWindow> &weak_window, F &&function) noexcept {
    auto context = weak_context.lock(); auto window = weak_window.lock();
    if (!context || !window || !context->owner_thread() || !context->live(window)) return;
    @try { try { function(*context, window); } catch (...) { context->callback_failed(); } }
    @catch (NSException *) { context->callback_failed(); }
}
} // namespace nilwebview::macos

@interface NilWebViewDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler, NSWindowDelegate> {
@public
    std::weak_ptr<nilwebview::macos::MacContext> context;
    std::weak_ptr<nilwebview::macos::MacWindow> window;
}
@end

@implementation NilWebViewDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    nilwebview::macos::callback(context, window, [](auto &state, const auto &value) { state.close_requested(value); });
    return NO;
}
- (void)webViewDidClose:(WKWebView *)webView {
    (void)webView;
    nilwebview::macos::callback(context, window, [](auto &state, const auto &value) { state.close_requested(value); });
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    (void)webView;
    nilwebview::macos::callback(context, window, [](auto &state, const auto &value) {
        state.provider_failed(value, "WebKit content process terminated");
    });
}
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    (void)webView;
    WKNavigationActionPolicy policy = WKNavigationActionPolicyCancel;
    bool stop_navigation = false;
    nilwebview::macos::callback(context, window, [&](auto &state, const auto &value) {
        if (value->failed) return;
        // WKNavigationAction on the supported macOS versions has no public
        // navigation ID or HTTP-redirect flag. Admit exactly the host request;
        // an unrelated page navigation cannot inherit an in-flight operation.
        if (value->admit_request && action.targetFrame.isMainFrame &&
            [action.request.URL isEqual:value->requested_url]) {
            value->admit_request = false; policy = WKNavigationActionPolicyAllow;
        } else {
            std::string url;
            if (!nilwebview::macos::from_ns(action.request.URL.absoluteString, url)) {
                state.provider_failed(value, "WebKit navigation URI exceeds the text contract"); return;
            }
            state.navigation_requested(value, url);
            if (action.targetFrame.isMainFrame && value->navigation) {
                // A policy cancellation need not produce a later navigation
                // delegate error. Complete the owned operation here and clear
                // its native identity first, so late callbacks cannot finish
                // a newer application navigation.
                value->policy_cancelled = true;
                value->fragment_navigation = false;
                value->admit_request = false;
                value->navigation = nil;
                const auto operation = value->navigation_operation;
                value->navigation_operation = 0;
                if (value->bootstrap) {
                    value->bootstrap = false;
                    state.initialization_failed(value, "WebKit initial navigation cancelled by application policy");
                } else if (operation) {
                    state.navigation_finished(value, operation, false, "",
                        "WebKit navigation cancelled by application policy", NILWV_FAILURE_CANCELLED);
                }
                stop_navigation = true;
            }
        }
    });
    decisionHandler(policy);
    if (stop_navigation) {
        nilwebview::macos::callback(context, window, [](auto &, const auto &value) {
            [value->view stopLoading];
        });
    }
}
- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    (void)webView;
    nilwebview::macos::callback(context, window, [&](auto &, const auto &value) {
        if (navigation == value->navigation) value->fragment_navigation = false;
    });
}
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    nilwebview::macos::callback(context, window, [&](auto &state, const auto &value) {
        if (navigation != value->navigation || value->failed) return;
        value->admit_request = false; value->navigation = nil; value->fragment_navigation = false;
        if (value->bootstrap) { value->bootstrap = false; state.initialized(value); return; }
        std::string url;
        if (!nilwebview::macos::from_ns(webView.URL.absoluteString, url)) {
            state.provider_failed(value, "WebKit completed URI exceeds the text contract"); return;
        }
        state.navigation_finished(value, value->navigation_operation, true, url);
        value->navigation_operation = 0;
    });
}
- (void)failedNavigation:(WKNavigation *)navigation {
    nilwebview::macos::callback(context, window, [&](auto &state, const auto &value) {
        if (navigation != value->navigation || value->failed) return;
        value->admit_request = false; value->navigation = nil; value->fragment_navigation = false;
        if (value->bootstrap) { state.initialization_failed(value, "WebKit initial page failed"); return; }
        state.navigation_finished(value, value->navigation_operation, false, "",
            value->policy_cancelled ? "WebKit navigation cancelled by application policy" : "WebKit navigation failed",
            value->policy_cancelled ? NILWV_FAILURE_CANCELLED : NILWV_FAILURE_NATIVE);
        value->navigation_operation = 0;
    });
}
- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)error; [self failedNavigation:navigation];
}
- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)error; [self failedNavigation:navigation];
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    nilwebview::macos::callback(context, window, [&](auto &state, const auto &value) {
        if (![message.body isKindOfClass:[NSString class]]) {
            state.provider_failed(value, "WebKit message must contain serialized JSON text"); return;
        }
        std::string payload;
        if (!nilwebview::macos::from_ns((NSString *)message.body, payload)) {
            state.provider_failed(value, "WebKit message exceeds the text contract"); return;
        }
        nilwebview::Provenance source;
        const auto frame = message.frameInfo;
        source.frame_kind = !frame ? NILWV_FRAME_UNKNOWN : frame.isMainFrame ? NILWV_FRAME_MAIN : NILWV_FRAME_CHILD;
        if (!nilwebview::macos::from_ns(frame.request.URL.absoluteString, source.url)) {
            state.provider_failed(value, "WebKit sender URL exceeds the text contract"); return;
        }
        if (!source.url.empty()) source.source_kind = NILWV_SOURCE_NATIVE_URL;
        // Serialize the native security origin, never an origin parsed from the
        // current page URL. Opaque origins remain explicitly unknown.
        WKSecurityOrigin *origin = frame.securityOrigin;
        NSString *scheme = origin.protocol;
        if (([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"]) && origin.host.length) {
            std::string protocol, host;
            if (!nilwebview::macos::from_ns(scheme, protocol) || !nilwebview::macos::from_ns(origin.host, host)) {
                state.provider_failed(value, "WebKit native origin exceeds the text contract"); return;
            }
            if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";
            source.origin = protocol + "://" + host;
            if (origin.port && !((protocol == "https" && origin.port == 443) || (protocol == "http" && origin.port == 80)))
                source.origin += ":" + std::to_string(origin.port);
            source.source_kind = NILWV_SOURCE_NATIVE_ORIGIN;
        }
        state.message(value, payload, std::move(source));
    });
}
// No modal dialogs or unmanaged browser windows may block the owning thread.
- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completionHandler {
    (void)webView; (void)message; (void)frame; completionHandler();
}
- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))completionHandler {
    (void)webView; (void)message; (void)frame; completionHandler(NO);
}
- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
    defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame
    completionHandler:(void (^)(NSString *))completionHandler {
    (void)webView; (void)prompt; (void)defaultText; (void)frame; completionHandler(nil);
}
- (WKWebView *)webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
    forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    (void)webView; (void)configuration; (void)action; (void)features; return nil;
}
@end

struct nilwv_context_v1 { std::shared_ptr<nilwebview::macos::MacContext> state; };
namespace nilwebview::macos {
namespace {
template<class F> void call(nilwv_context_v1 *context, nilwv_result_v1 *out, F &&function, bool allow_fatal = false) noexcept {
    if (!out || out->size != sizeof(*out)) return;
    if (![NSThread isMainThread]) { static_error(out, NILWV_E_THREAD, "WebKit requires the process main thread"); return; }
    @autoreleasepool { @try {
        invoke(context ? context->state : nullptr, out, std::forward<F>(function), allow_fatal);
    } @catch (NSException *) {
        if (context && context->state) context->state->callback_failed();
        static_error(out, NILWV_E_NATIVE, "WebKit operation failed");
    } }
}
void probe(nilwv_result_v1 *out) { success(out); }
void create(nilwv_context_v1 **output, nilwv_result_v1 *out) {
    if (!out || out->size != sizeof(*out)) return;
    if (output) *output = nullptr;
    if (!output) { static_error(out, NILWV_E_ARGUMENT, "WebView context output is required"); return; }
    if (![NSThread isMainThread]) { static_error(out, NILWV_E_THREAD, "WebKit requires the process main thread"); return; }
    @autoreleasepool { @try { try {
        auto value = std::make_unique<nilwv_context_v1>(); value->state = std::make_shared<MacContext>();
        value->state->store = [WKWebsiteDataStore nonPersistentDataStore];
        if (!value->state->store) { static_error(out, NILWV_E_UNAVAILABLE, "WebKit private data store unavailable"); return; }
        *output = value.release(); success(out);
    } catch (...) { static_error(out, NILWV_E_NATIVE, "WebKit context allocation failed"); }
    } @catch (NSException *) { static_error(out, NILWV_E_NATIVE, "WebKit context initialization failed"); } }
}
void destroy(nilwv_context_v1 *context) {
    // Runtime owns the opaque pointer and must honor its creation thread. A bad
    // foreign-thread destroy cannot safely release AppKit resources.
    if (!context || ![NSThread isMainThread] || !context->state->owner_thread()) return;
    @autoreleasepool { @try {
        const auto windows = context->state->clear();
        for (const auto &window : windows) if (window) dispose(std::static_pointer_cast<MacWindow>(window));
    } @catch (NSException *) {} }
    delete context;
}
void window(nilwv_context_v1 *context, nilwv_text_v1 title, std::int64_t width, std::int64_t height, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) {
        std::string text; if (!state.read_text(title, text, out)) return;
        if (width < 1 || height < 1 || width > 16384 || height > 16384) { state.fail(out, NILWV_E_ARGUMENT, "WebView dimensions must be between 1 and 16384"); return; }
        if (!CGMainDisplayID()) { state.fail(out, NILWV_E_UNAVAILABLE, "WebKit requires an active macOS window server session"); return; }
        static bool application_initialized = false;
        if (!application_initialized) { [NSApplication sharedApplication]; [NSApp finishLaunching]; application_initialized = true; }
        auto value = std::make_shared<MacWindow>();
        if (!state.add_window(value, out)) return;
        state.native_windows.erase(std::remove_if(state.native_windows.begin(), state.native_windows.end(), [](const auto &item) { return item.expired(); }), state.native_windows.end());
        state.native_windows.push_back(value);
        value->delegate = [[NilWebViewDelegate alloc] init]; value->delegate->context = context->state; value->delegate->window = value;
        WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
        configuration.websiteDataStore = state.store;
        [configuration.userContentController addScriptMessageHandler:value->delegate name:@"nilwebview"];
        [configuration.userContentController addUserScript:[[WKUserScript alloc] initWithSource:to_ns(message_script()) injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
        value->native_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
            backing:NSBackingStoreBuffered defer:NO];
        value->native_window.releasedWhenClosed = NO; value->native_window.title = to_ns(text);
        value->native_window.delegate = value->delegate;
        value->view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, width, height) configuration:configuration];
        value->view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        value->view.navigationDelegate = value->delegate; value->view.UIDelegate = value->delegate;
        value->native_window.contentView = value->view;
        if (!value->native_window || !value->view || !value->delegate) { state.initialization_failed(value, "WebKit window initialization failed"); return; }
        value->admit_request = true; value->requested_url = [NSURL URLWithString:@"about:blank"];
        value->navigation = [value->view loadHTMLString:@"<!doctype html><html></html>" baseURL:nil];
        if (!value->navigation) state.initialization_failed(value, "WebKit initial navigation unavailable");
    });
}
void show(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) { auto value = state.window(id, out); if (!value) return;
        [std::static_pointer_cast<MacWindow>(value)->native_window makeKeyAndOrderFront:nil]; success(out); });
}
void navigate_common(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 input, nilwv_result_v1 *out, bool html) {
    call(context, out, [&](MacContext &state) {
        std::string data; if (!state.read_text(input, data, out, html)) return;
        auto base = state.window(id, out); if (!base) return; auto value = std::static_pointer_cast<MacWindow>(base);
        NSURL *url = html ? [NSURL URLWithString:@"about:blank"] : [NSURL URLWithString:to_ns(data)];
        if (!html && (!navigation_uri(data) || !url || (![url.scheme isEqualToString:@"about"] && !url.host.length))) {
            state.fail(out, NILWV_E_ARGUMENT, "WebView navigation requires an HTTP(S) URI or about:blank"); return;
        }
        std::string current_uri, requested_uri;
        const bool native_uris = !html && from_ns(value->view.URL.absoluteString, current_uri) &&
            from_ns(url.absoluteString, requested_uri);
        const auto fragment = requested_uri.find('#');
        const bool fragment_navigation = native_uris && !value->view.loading &&
            fragment != std::string::npos &&
            current_uri.compare(0, current_uri.find('#'), requested_uri, 0, fragment) == 0;
        const auto operation = state.start(value, Operation::navigation, out); if (!operation) return;
        value->admit_request = true; value->policy_cancelled = false; value->requested_url = url; value->navigation_operation = operation;
        value->fragment_navigation = fragment_navigation;
        value->navigation = html ? [value->view loadHTMLString:to_ns(data) baseURL:nil] : [value->view loadRequest:[NSURLRequest requestWithURL:url]];
        if (!value->navigation) {
            value->admit_request = false; value->navigation_operation = 0; value->fragment_navigation = false;
            state.reject(value, operation, out, "WebKit rejected the navigation");
        }
    });
}
void navigate(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 input, nilwv_result_v1 *out) { navigate_common(context, id, input, out, false); }
void load_html(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 input, nilwv_result_v1 *out) { navigate_common(context, id, input, out, true); }
void evaluate(nilwv_context_v1 *context, std::int64_t window_id, nilwv_text_v1 input, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) {
        std::string data; if (!state.read_text(input, data, out)) return;
        auto base = state.window(window_id, out); if (!base) return; auto value = std::static_pointer_cast<MacWindow>(base);
        const auto operation = state.start(value, Operation::script, out); if (!operation) return;
        std::weak_ptr<MacContext> weak_context = context->state; std::weak_ptr<MacWindow> weak_window = value;
        [value->view evaluateJavaScript:to_ns(evaluation_script(data)) completionHandler:^(id answer, NSError *error) {
            callback(weak_context, weak_window, [&](auto &owner, const auto &live_window) {
                if (error) { owner.script_failed(live_window, operation, "WebKit script raised an exception"); return; }
                std::string encoded;
                if (![answer isKindOfClass:[NSString class]]) {
                    owner.script_failed(live_window, operation, "WebKit script envelope was not text", NILWV_FAILURE_SERIALIZATION); return;
                }
                if (!from_ns((NSString *)answer, encoded, NILWV_MAX_TEXT_BYTES + 2)) {
                    owner.script_failed(live_window, operation, "WebKit script result exceeds the text contract", NILWV_FAILURE_PAYLOAD_LIMIT); return;
                }
                owner.script_finished(live_window, operation, encoded);
            });
        }];
    });
}
void poll(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) {
        if (!state.window(id, out, false)) return;
        const auto deadline = Clock::now() + std::chrono::milliseconds(2);
        for (unsigned iteration = 0; iteration < 64 && Clock::now() < deadline; ++iteration) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
            if (event) [NSApp sendEvent:event];
            const auto run = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
            if (!event && run != kCFRunLoopRunHandledSource) break;
        }
        state.expire_operations();
        for (const auto &weak : state.native_windows) if (auto value = weak.lock(); value && state.live(value)) {
            if (value->failed) {
                value->admit_request = false; value->fragment_navigation = false; [value->view stopLoading];
            } else if (value->fragment_navigation && value->navigation_operation > 0 && value->navigation &&
                       value->operation_kind == Operation::navigation && value->operation == value->navigation_operation &&
                       !value->bootstrap && !value->admit_request && !value->policy_cancelled &&
                       !value->view.loading && [value->view.URL isEqual:value->requested_url]) {
                // Same-document navigation has no public didFinish callback.
                // loading stays true while the API request is pending; pairing
                // its completion with the preclassified exact fragment URI
                // avoids treating a provisional URL as a finished document.
                std::string uri;
                if (!from_ns(value->view.URL.absoluteString, uri)) {
                    state.provider_failed(value, "WebKit completed URI exceeds the text contract");
                    continue;
                }
                const auto operation = value->navigation_operation;
                value->navigation_operation = 0; value->navigation = nil; value->fragment_navigation = false;
                state.navigation_finished(value, operation, true, uri);
            }
        }
        state.poll(id, out);
    });
}
void event_number(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 field, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) { std::string name; if (state.read_text(field, name, out)) state.event_number(id, name, out); });
}
void event_text(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 field, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) { std::string name; if (state.read_text(field, name, out)) state.event_text(id, name, out); });
}
void release_event(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) { state.release_event(id, out); }, true);
}
void close(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) {
    call(context, out, [&](MacContext &state) { auto value = state.close(id, out); if (value) dispose(std::static_pointer_cast<MacWindow>(value)); }, true);
}
const nilwv_api_v1 api = {NILWV_ABI_V1, sizeof(nilwv_api_v1), NILWV_CAP_KNOWN_MASK,
    "WKWebView", "nilwebview-v1", probe, create, destroy, window, show, navigate, load_html,
    evaluate, poll, event_number, event_text, release_event, close};
} // namespace
} // namespace nilwebview::macos
extern "C" NILWV_EXPORT const nilwv_api_v1 *NILWV_CALL
nilwebview_provider_v1_get_api(std::uint32_t version, std::uint32_t size) {
    if (version != NILWV_ABI_V1 || size > sizeof(nilwv_api_v1)) return nullptr;
    return &nilwebview::macos::api;
}
