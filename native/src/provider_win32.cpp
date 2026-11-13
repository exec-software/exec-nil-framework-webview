// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
// WebView2 runs on the creating UI STA. Callbacks never enter Matrix/the VM.
#include "provider_shared.hpp"
#include "win32_navigation.hpp"
#include <windows.h>
#include <urlmon.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <new>
#include <stdexcept>
#include <utility>

namespace nilwebview::win32 {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
struct WinContext;
struct WinWindow;
struct WideText {
    LPWSTR value = nullptr;
    ~WideText() { CoTaskMemFree(value); }
};
std::wstring wide(const std::string &text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count) != count)
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}
bool narrow(LPCWSTR text, std::string &result, std::size_t limit = NILWV_MAX_TEXT_BYTES) {
    if (!text) { result.clear(); return true; }
    std::size_t length = 0;
    while (length <= limit && text[length]) ++length;
    if (length > limit) return false;
    if (!length) { result.clear(); return true; }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (count <= 0 || static_cast<std::size_t>(count) > limit) return false;
    result.resize(static_cast<std::size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), result.data(), count, nullptr, nullptr) == count && utf8(result);
}

// Profiles are retained by their context until closed browser processes and
// asynchronous completions have released them. No detached cleanup worker may
// outlive the Matrix invocation that owns the private directory.
struct Profile {
    std::wstring directory;
    HANDLE browser = nullptr;
    ComPtr<ICoreWebView2Environment5> exit_environment;
    EventRegistrationToken exit_token{};
    std::atomic<unsigned> pending{0};
    bool exit_registered = false;
    bool await_browser_exit = false;
    bool browser_resources_released = false;
    bool retired = false;

    void release_monitor() noexcept {
        if (exit_registered && exit_environment)
            exit_environment->remove_BrowserProcessExited(exit_token);
        exit_registered = false;
        exit_environment.Reset();
    }
    ~Profile() {
        release_monitor();
        if (browser) CloseHandle(browser);
        // An unfinished profile is deliberately retained as failure evidence.
        // Only the owning context performs directory cleanup on its STA.
    }
};

// One ticket per asynchronous completion. A failed synchronous API call or a
// released callback also finishes the ticket. Tickets own only independent
// profile state; they never keep a destroyed context/window alive.
struct Completion {
    std::shared_ptr<Profile> profile;
    std::atomic<bool> finished{false};
    explicit Completion(std::shared_ptr<Profile> value) : profile(std::move(value)) {
        profile->pending.fetch_add(1, std::memory_order_relaxed);
    }
    void finish() noexcept {
        if (!finished.exchange(true, std::memory_order_acq_rel))
            profile->pending.fetch_sub(1, std::memory_order_release);
    }
    ~Completion() { finish(); }
};
struct FinishCompletion {
    const std::shared_ptr<Completion> &completion;
    ~FinishCompletion() { completion->finish(); }
};

// Never traverse a reparse point, search the process table, or terminate a
// browser. File count and a common deadline bound each cleanup pass.
bool remove_owned_tree(const std::wstring &path, Clock::time_point until, unsigned &remaining) {
    if (!remaining || Clock::now() >= until) return false;
    --remaining;
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &entry);
        if (search != INVALID_HANDLE_VALUE) {
            bool okay = true;
            do {
                if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
                if (!remove_owned_tree(path + L"\\" + entry.cFileName, until, remaining)) { okay = false; break; }
            } while (FindNextFileW(search, &entry));
            const auto error = GetLastError();
            FindClose(search);
            if (!okay || error != ERROR_NO_MORE_FILES) return false;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) != FALSE : DeleteFileW(path.c_str()) != FALSE;
}
bool observe_browser_exit(const std::shared_ptr<Profile> &profile, ICoreWebView2Environment *environment) noexcept {
    try {
        if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&profile->exit_environment)))) return false;
        const std::weak_ptr<Profile> weak_profile = profile;
        const auto result = profile->exit_environment->add_BrowserProcessExited(
            Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                [weak_profile](ICoreWebView2Environment *, ICoreWebView2BrowserProcessExitedEventArgs *) -> HRESULT {
                    if (auto value = weak_profile.lock()) value->browser_resources_released = true;
                    return S_OK;
                }).Get(), &profile->exit_token);
        profile->exit_registered = SUCCEEDED(result);
        if (!profile->exit_registered) return false;

        // A context can be closed while environment creation is pending. Even
        // without a retained controller, inspect only this environment's own
        // process collection and keep an exact handle if its browser started.
        ComPtr<ICoreWebView2Environment8> process_environment;
        ComPtr<ICoreWebView2ProcessInfoCollection> processes;
        UINT count = 0;
        if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&process_environment))) ||
            FAILED(process_environment->GetProcessInfos(&processes)) ||
            FAILED(processes->get_Count(&count)) || count > 128) return false;
        profile->await_browser_exit = count != 0;
        for (UINT index = 0; index < count; ++index) {
            ComPtr<ICoreWebView2ProcessInfo> info;
            COREWEBVIEW2_PROCESS_KIND kind{};
            INT32 pid = 0;
            if (FAILED(processes->GetValueAtIndex(index, &info)) ||
                FAILED(info->get_Kind(&kind)) || FAILED(info->get_ProcessId(&pid))) return false;
            if (kind == COREWEBVIEW2_PROCESS_KIND_BROWSER && pid > 0 && !profile->browser)
                profile->browser = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        }
        return true;
    } catch (...) { return false; }
}
std::shared_ptr<Profile> create_profile() {
    wchar_t parent[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(parent)), parent);
    if (!length || length >= std::size(parent)) throw std::runtime_error("temporary directory unavailable");
    auto profile = std::make_shared<Profile>();
    for (unsigned attempt = 0; attempt != 4; ++attempt) {
        GUID guid{}; wchar_t name[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, name, static_cast<int>(std::size(name)))) break;
        auto path = std::wstring(parent, length) + L"nilwebview-" + name;
        if (CreateDirectoryW(path.c_str(), nullptr)) { profile->directory = std::move(path); return profile; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    throw std::runtime_error("private WebView profile unavailable");
}
struct Binding { std::weak_ptr<WinContext> context; std::weak_ptr<WinWindow> window; };
struct WinWindow final : Window {
    HWND handle = nullptr;
    Binding binding;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    ComPtr<ICoreWebView2_21> modern;
    std::shared_ptr<Profile> profile;
    ComPtr<IUri> requested_uri;
    std::wstring requested_html_uri;
    std::wstring script_id;
    std::string runtime_detail;
    std::uint64_t navigation_id = 0;
    std::int64_t navigation_operation = 0;
    bool bootstrap = true;
    bool admit_request = false;
    bool policy_cancelled = false;
    bool fragment_request = false;
    bool fragment_repeat = false;
};
struct WinContext final : Context {
    bool com_initialized = false;
    std::vector<std::weak_ptr<WinWindow>> native_windows;
    // Closed profiles occupy a slot until their browser exits and cleanup
    // finishes. This also bounds repeated open/close while initialization is
    // delayed; window() and poll() opportunistically reclaim retired slots.
    std::array<std::shared_ptr<Profile>, NILWV_MAX_WINDOWS> profiles;
    ~WinContext() override { if (com_initialized) CoUninitialize(); }
};
template<class F> void callback(const std::weak_ptr<WinContext> &weak_context,
                               const std::weak_ptr<WinWindow> &weak_window, F &&function) noexcept {
    auto state = weak_context.lock(); auto value = weak_window.lock();
    if (!state || !value || !state->owner_thread() || !state->live(value)) return;
    try { function(*state, value); } catch (...) { state->callback_failed(); }
}
void dispose(const std::shared_ptr<WinWindow> &value) noexcept {
    if (!value) return;
    value->admit_request = false;
    // All registered handlers capture weak state only. Close releases the
    // controller's handlers; pending completions remain independently safe.
    if (value->view) value->view->Stop();
    if (value->controller) value->controller->Close();
    value->modern.Reset(); value->view.Reset(); value->controller.Reset(); value->environment.Reset();
    value->requested_uri.Reset();
    value->requested_html_uri.clear();
    value->fragment_request = false;
    value->fragment_repeat = false;
    if (value->handle) { DestroyWindow(value->handle); value->handle = nullptr; }
    if (value->profile) value->profile->retired = true;
    value->profile.reset();
}

bool collect_profiles(WinContext &state, Clock::time_point until, unsigned budget) noexcept {
    bool complete = true;
    for (auto &profile : state.profiles) {
        if (!profile) continue;
        complete = false;
        if (!profile->retired || profile->pending.load(std::memory_order_acquire)) continue;
        if (profile->await_browser_exit && !profile->browser_resources_released) continue;
        if (profile->browser && WaitForSingleObject(profile->browser, 0) != WAIT_OBJECT_0) continue;
        profile->release_monitor();
        try {
            if (!remove_owned_tree(profile->directory, until, budget)) continue;
            profile->directory.clear();
            if (profile->browser) { CloseHandle(profile->browser); profile->browser = nullptr; }
            profile.reset();
        } catch (...) {}
    }
    if (!complete)
        complete = std::all_of(state.profiles.begin(), state.profiles.end(), [](const auto &value) { return !value; });
    return complete;
}

void pump_retired_profiles(WinContext &state) noexcept {
    MSG message{};
    const auto until = Clock::now() + std::chrono::milliseconds(2);
    for (unsigned iteration = 0; iteration < 64 && Clock::now() < until; ++iteration) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    collect_profiles(state, Clock::now() + std::chrono::milliseconds(2), 256);
}

void drain_profiles(WinContext &state) noexcept {
    // One deadline for the entire context, not 35 seconds for every window.
    const auto until = Clock::now() + std::chrono::seconds(35);
    bool quit_seen = false;
    WPARAM quit_code = 0;
    while (!collect_profiles(state, std::min(until, Clock::now() + std::chrono::milliseconds(5)), 2048)) {
        if (Clock::now() >= until) break;
        MSG message{};
        const auto batch_until = std::min(until, Clock::now() + std::chrono::milliseconds(2));
        for (unsigned iteration = 0; iteration < 64 && Clock::now() < batch_until; ++iteration) {
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
            if (message.message == WM_QUIT) {
                quit_seen = true; quit_code = message.wParam; continue;
            }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        // WebView2 asynchronous completions require the creating STA's pump.
        // Preserve application WM_QUIT while continuing this bounded drain.
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (quit_seen) PostQuitMessage(static_cast<int>(quit_code));
    bool retained = false;
    for (auto &profile : state.profiles) if (profile) {
        retained = true;
        // Break environment/callback ownership on timeout. Late callbacks hold
        // independent cancelled profile state and weak context/window handles.
        profile->release_monitor();
    }
    if (retained) {
        constexpr auto detail = "nilwebview: private profile cleanup did not complete within the context teardown deadline; retained profile evidence\n";
        OutputDebugStringA(detail);
        std::fputs(detail, stderr);
        std::fflush(stderr);
    }
}
void fail_initialization(WinContext &state, const std::shared_ptr<WinWindow> &value, const char *detail) {
    state.initialization_failed(value, detail); dispose(value);
}
LRESULT CALLBACK window_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto binding = reinterpret_cast<Binding *>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (binding) {
        if (message == WM_CLOSE) {
            callback(binding->context, binding->window, [](auto &state, const auto &value) {
                if (value->bootstrap) fail_initialization(state, value, "WebView2 host window closed during initialization");
                else state.close_requested(value);
            });
            return 0;
        }
        if (message == WM_SIZE) {
            callback(binding->context, binding->window, [&](auto &state, const auto &value) {
                if (value->controller) { RECT bounds{}; GetClientRect(handle, &bounds);
                    if (FAILED(value->controller->put_Bounds(bounds))) state.provider_failed(value, "WebView2 resize failed"); }
            });
        }
        if (message == WM_MOVE) {
            callback(binding->context, binding->window, [](auto &, const auto &value) {
                if (value->controller) value->controller->NotifyParentWindowPositionChanged();
            });
        }
        if (message == WM_SETFOCUS) {
            callback(binding->context, binding->window, [](auto &, const auto &value) {
                if (value->controller) value->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            });
        }
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
    // Never PostQuitMessage: this provider does not own the application's loop.
    return DefWindowProcW(handle, message, wparam, lparam);
}
HWND create_window(WinWindow &value, const std::wstring &title, int width, int height) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&window_proc), &module)) return nullptr;
    constexpr auto name = L"Execute.NilWebView.Provider.V1";
    WNDCLASSEXW klass{}; klass.cbSize = sizeof(klass); klass.hInstance = module;
    klass.lpfnWndProc = window_proc; klass.lpszClassName = name; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&klass)) {
        WNDCLASSEXW existing{}; existing.cbSize = sizeof(existing);
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS || !GetClassInfoExW(module, name, &existing) || existing.lpfnWndProc != window_proc) return nullptr;
    }
    RECT bounds{0, 0, width, height};
    if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0)) return nullptr;
    return CreateWindowExW(0, name, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, module, &value.binding);
}
bool canonical_uri(IUri *uri, std::wstring &output) {
    struct AbsoluteUri {
        BSTR value = nullptr;
        ~AbsoluteUri() { SysFreeString(value); }
    } absolute;
    if (!uri || FAILED(uri->GetAbsoluteUri(&absolute.value)) || !absolute.value ||
        SysStringLen(absolute.value) > NILWV_MAX_TEXT_BYTES) return false;
    output.assign(absolute.value, SysStringLen(absolute.value));
    return true;
}
bool parse_native_uri(LPCWSTR text, ComPtr<IUri> &uri) {
    if (!text) return false;
    std::size_t length = 0;
    while (length <= NILWV_MAX_TEXT_BYTES && text[length]) ++length;
    return length <= NILWV_MAX_TEXT_BYTES && SUCCEEDED(CreateUri(text, Uri_CREATE_CANONICALIZE, 0, &uri));
}
bool same_uri(IUri *expected, LPCWSTR text) {
    ComPtr<IUri> actual;
    std::wstring requested, observed;
    // IUri::IsEqual is case-insensitive. Compare canonical strings instead so
    // HTTP paths and fragments retain their case-sensitive identity.
    return parse_native_uri(text, actual) &&
        canonical_uri(expected, requested) && canonical_uri(actual.Get(), observed) && requested == observed;
}
bool fragment_only(IUri *requested, LPCWSTR source, bool &repeat) {
    repeat = false;
    ComPtr<IUri> current;
    std::wstring old_uri, new_uri;
    if (!parse_native_uri(source, current) ||
        !canonical_uri(current.Get(), old_uri) || !canonical_uri(requested, new_uri)) return false;
    if (new_uri.find(L'#') == std::wstring::npos ||
        old_uri.substr(0, old_uri.find(L'#')) != new_uri.substr(0, new_uri.find(L'#'))) return false;
    repeat = old_uri == new_uri;
    return true;
}
bool pending_fragment(const std::shared_ptr<WinWindow> &value) {
    return !value->bootstrap && value->ready && !value->closed && !value->failed && value->fragment_request &&
        value->admit_request && !value->navigation_id && value->navigation_operation > 0 &&
        value->operation_kind == Operation::navigation && value->operation == value->navigation_operation;
}
void finish_fragment(WinContext &state, const std::shared_ptr<WinWindow> &value, LPCWSTR source) {
    if (!state.live(value) || !pending_fragment(value) || !same_uri(value->requested_uri.Get(), source)) return;
    std::string url;
    if (!narrow(source, url)) {
        state.provider_failed(value, "WebView2 fragment URI exceeds text contract"); return;
    }
    const auto operation = value->navigation_operation;
    value->admit_request = false; value->fragment_request = false; value->fragment_repeat = false;
    value->navigation_operation = 0;
    state.navigation_finished(value, operation, true, url, "");
}
bool attach_handlers(const std::shared_ptr<WinContext> &context, const std::shared_ptr<WinWindow> &window) {
    const std::weak_ptr<WinContext> weak_context = context;
    const std::weak_ptr<WinWindow> weak_window = window;
    EventRegistrationToken token{};
    HRESULT result = window->view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
            if (!args) return E_POINTER;
            args->put_Cancel(TRUE);
            callback(weak_context, weak_window, [&](auto &state, const auto &value) {
                if (value->failed) return;
                WideText uri; BOOL redirected = TRUE; UINT64 id = 0;
                if (FAILED(args->get_Uri(&uri.value)) || FAILED(args->get_IsRedirected(&redirected)) || FAILED(args->get_NavigationId(&id))) {
                    state.provider_failed(value, "WebView2 navigation metadata unavailable"); return;
                }
                // WebView2 may announce its own initial about:blank before
                // the asynchronous document-created helper is installed. It
                // remains cancelled; only our subsequent NavigateToString is
                // authorized. Do not expose an unrecoverable navigation event
                // before the consumer has received Ready.
                if (value->bootstrap && !redirected && uri.value && !wcscmp(uri.value, L"about:blank")) return;
                if (value->bootstrap && !value->admit_request) {
                    fail_initialization(state, value, "WebView2 requested an unexpected navigation before initialization");
                    return;
                }
                const bool requested = value->admit_request && !redirected &&
                    (value->requested_html_uri.empty() ? same_uri(value->requested_uri.Get(), uri.value) :
                     uri.value && !wcsncmp(uri.value, value->requested_html_uri.c_str(), value->requested_html_uri.size() + 1));
                if (value->admit_request && !redirected && requested) {
                    value->admit_request = false; value->navigation_id = id;
                    value->requested_html_uri.clear();
                    if (FAILED(args->put_Cancel(FALSE))) state.provider_failed(value, "WebView2 cannot authorize navigation");
                    return;
                }
                if (value->bootstrap) {
                    fail_initialization(state, value, "WebView2 initial document did not match its authorized HTML content");
                    return;
                }
                if (id == value->navigation_id) value->policy_cancelled = true;
                std::string url;
                if (!narrow(uri.value, url)) { state.provider_failed(value, "WebView2 requested URI exceeds text contract"); return; }
                state.navigation_requested(value, url);
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *sender, ICoreWebView2SourceChangedEventArgs *args) -> HRESULT {
            callback(weak_context, weak_window, [&](auto &state, const auto &value) {
                if (!pending_fragment(value)) return;
                BOOL new_document = TRUE;
                WideText raw;
                if (!args || FAILED(args->get_IsNewDocument(&new_document)) ||
                    !sender || FAILED(sender->get_Source(&raw.value))) {
                    state.provider_failed(value, "WebView2 fragment navigation metadata unavailable"); return;
                }
                if (!new_document) finish_fragment(state, value, raw.value);
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
            if (!args) return E_POINTER;
            args->put_Cancel(TRUE);
            callback(weak_context, weak_window, [&](auto &state, const auto &value) {
                if (value->bootstrap) {
                    fail_initialization(state, value, "WebView2 requested a child-frame navigation during initialization");
                    return;
                }
                WideText raw; std::string url;
                if (FAILED(args->get_Uri(&raw.value)) || !narrow(raw.value, url)) state.provider_failed(value, "WebView2 frame URI unavailable");
                else state.navigation_requested(value, url);
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
            callback(weak_context, weak_window, [&](auto &state, const auto &value) {
                UINT64 id = 0; BOOL okay = FALSE;
                if (!args || FAILED(args->get_NavigationId(&id)) || FAILED(args->get_IsSuccess(&okay))) {
                    state.provider_failed(value, "WebView2 navigation completion unavailable"); return;
                }
                if (value->failed || id != value->navigation_id) return;
                value->admit_request = false; value->navigation_id = 0;
                value->fragment_request = false;
                value->fragment_repeat = false;
                if (value->bootstrap) {
                    value->bootstrap = false;
                    if (okay) state.initialized(value, value->runtime_detail);
                    else fail_initialization(state, value, "WebView2 initial document failed");
                    return;
                }
                WideText raw; std::string url;
                if (okay && (!sender || FAILED(sender->get_Source(&raw.value)) || !narrow(raw.value, url))) {
                    state.provider_failed(value, "WebView2 completed URI exceeds text contract"); return;
                }
                state.navigation_finished(value, value->navigation_operation, okay && !value->policy_cancelled, url,
                    value->policy_cancelled ? "WebView2 navigation cancelled by application policy" : okay ? "" : "WebView2 navigation failed",
                    value->policy_cancelled ? NILWV_FAILURE_CANCELLED : NILWV_FAILURE_NATIVE);
                value->navigation_operation = 0;
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
            callback(weak_context, weak_window, [&](auto &state, const auto &value) {
                if (value->bootstrap) {
                    fail_initialization(state, value, "WebView2 received a page message during initialization");
                    return;
                }
                WideText raw, source; std::string payload; Provenance provenance;
                if (!args || FAILED(args->TryGetWebMessageAsString(&raw.value)) || !narrow(raw.value, payload)) {
                    state.provider_failed(value, "WebView2 message must be bounded serialized JSON text"); return;
                }
                if (FAILED(args->get_Source(&source.value)) || !narrow(source.value, provenance.url)) {
                    state.provider_failed(value, "WebView2 native sender URI unavailable"); return;
                }
                // This event is the top-level ICoreWebView2 event, not the
                // separate ICoreWebView2Frame message event. Source is native.
                provenance.frame_kind = NILWV_FRAME_MAIN;
                if (!provenance.url.empty()) provenance.source_kind = NILWV_SOURCE_NATIVE_URL;
                state.message(value, payload, std::move(provenance));
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
            if (!args) return E_POINTER;
            args->put_Handled(TRUE);
            callback(weak_context, weak_window, [&](auto &state, const auto &value) {
                if (value->bootstrap) {
                    fail_initialization(state, value, "WebView2 requested a new window during initialization");
                    return;
                }
                WideText raw; std::string url;
                if (FAILED(args->get_Uri(&raw.value)) || !narrow(raw.value, url)) state.provider_failed(value, "WebView2 new-window URI unavailable");
                else state.navigation_requested(value, url);
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->modern->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
            if (!args) return E_POINTER;
            const auto result = args->put_Cancel(TRUE);
            args->put_Handled(TRUE);
            return result;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
            return args ? args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY) : E_POINTER;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_WindowCloseRequested(Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *, IUnknown *) -> HRESULT {
            callback(weak_context, weak_window, [](auto &state, const auto &value) {
                if (value->bootstrap) fail_initialization(state, value, "WebView2 requested closure during initialization");
                else state.close_requested(value);
            });
            return S_OK;
        }).Get(), &token);
    if (FAILED(result)) return false;
    result = window->view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [weak_context, weak_window](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *) -> HRESULT {
            callback(weak_context, weak_window, [](auto &state, const auto &value) { state.provider_failed(value, "WebView2 browser process failed"); });
            return S_OK;
        }).Get(), &token);
    return SUCCEEDED(result);
}

void initialize_controller(const std::shared_ptr<WinContext> &context, const std::shared_ptr<WinWindow> &value) {
    UINT32 pid = 0;
    if (FAILED(value->controller->get_CoreWebView2(&value->view)) ||
        FAILED(value->view->get_BrowserProcessId(&pid)) || !pid ||
        (value->profile->browser && GetProcessId(value->profile->browser) != pid) ||
        (!value->profile->browser && !(value->profile->browser = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))) {
        fail_initialization(*context, value, "WebView2 browser lifetime handle unavailable"); return;
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(value->view.As(&value->modern)) ||
        FAILED(value->view->get_Settings(&settings)) || FAILED(settings->put_IsWebMessageEnabled(TRUE)) ||
        FAILED(settings->put_AreDefaultScriptDialogsEnabled(FALSE)) || FAILED(settings->put_AreHostObjectsAllowed(FALSE)) ||
        FAILED(settings->put_AreDevToolsEnabled(FALSE)) || FAILED(settings->put_AreDefaultContextMenusEnabled(FALSE)) ||
        FAILED(settings->put_IsStatusBarEnabled(FALSE)) || FAILED(settings->put_IsBuiltInErrorPageEnabled(FALSE))) {
        fail_initialization(*context, value, "WebView2 runtime lacks the required modern script/settings contract"); return;
    }
    RECT bounds{};
    if (!GetClientRect(value->handle, &bounds) || FAILED(value->controller->put_Bounds(bounds)) ||
        FAILED(value->controller->put_IsVisible(TRUE)) || !attach_handlers(context, value)) {
        fail_initialization(*context, value, "WebView2 controller configuration failed"); return;
    }
    const std::weak_ptr<WinContext> weak_context = context;
    const std::weak_ptr<WinWindow> weak_window = value;
    const auto script = wide(message_script());
    const auto completion = std::make_shared<Completion>(value->profile);
    const auto result = value->view->AddScriptToExecuteOnDocumentCreated(script.c_str(),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [weak_context, weak_window, completion](HRESULT error, LPCWSTR id) -> HRESULT {
                const FinishCompletion finished{completion};
                callback(weak_context, weak_window, [&](auto &state, const auto &window) {
                    if (window->failed) return;
                    if (FAILED(error) || !id) { fail_initialization(state, window, "WebView2 message helper registration failed"); return; }
                    window->script_id = id;
                    window->requested_html_uri = wide(html_navigation_uri("<!doctype html><html></html>"));
                    window->admit_request = true;
                    if (FAILED(CreateUri(L"about:blank", Uri_CREATE_CANONICALIZE, 0, &window->requested_uri)) ||
                        FAILED(window->view->NavigateToString(L"<!doctype html><html></html>")))
                        fail_initialization(state, window, "WebView2 initial navigation failed");
                });
                return S_OK;
            }).Get());
    if (FAILED(result)) {
        completion->finish();
        fail_initialization(*context, value, "WebView2 message helper registration unavailable");
    }
}
} // namespace nilwebview::win32

struct nilwv_context_v1 { std::shared_ptr<nilwebview::win32::WinContext> state; };
namespace nilwebview::win32 {
namespace {
template<class F> void call(nilwv_context_v1 *context, nilwv_result_v1 *out, F &&function, bool allow_fatal = false) noexcept {
    invoke(context ? context->state : nullptr, out, std::forward<F>(function), allow_fatal);
}
void probe(nilwv_result_v1 *out) noexcept {
    WideText version;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version.value)) || !version.value)
        static_error(out, NILWV_E_UNAVAILABLE, "Microsoft WebView2 Runtime is not available");
    else success(out);
}
void create(nilwv_context_v1 **output, nilwv_result_v1 *out) noexcept {
    if (!out || out->size != sizeof(*out)) return;
    if (output) *output = nullptr;
    if (!output) { static_error(out, NILWV_E_ARGUMENT, "WebView context output is required"); return; }
    // Do not change process-wide COM settings; balance only our STA reference.
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) { static_error(out, NILWV_E_THREAD, "WebView2 requires a UI thread in a single-threaded COM apartment"); return; }
    try {
        auto value = std::make_unique<nilwv_context_v1>();
        value->state = std::make_shared<WinContext>(); value->state->com_initialized = true;
        *output = value.release(); success(out);
    } catch (...) { CoUninitialize(); static_error(out, NILWV_E_NATIVE, "WebView2 context allocation failed"); }
}
void destroy(nilwv_context_v1 *context) noexcept {
    if (!context || !context->state->owner_thread()) return;
    const auto windows = context->state->clear();
    for (const auto &value : windows) if (value) dispose(std::static_pointer_cast<WinWindow>(value));
    drain_profiles(*context->state);
    delete context;
}
void window(nilwv_context_v1 *context, nilwv_text_v1 title, std::int64_t width, std::int64_t height, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) {
        std::string text; if (!state.read_text(title, text, out)) return;
        if (width < 1 || height < 1 || width > 16384 || height > 16384) {
            state.fail(out, NILWV_E_ARGUMENT, "WebView dimensions must be between 1 and 16384"); return;
        }
        if (GetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", nullptr, 0) ||
            GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", nullptr, 0) ||
            GetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", nullptr, 0)) {
            state.fail(out, NILWV_E_ARGUMENT, "WebView2 environment overrides conflict with the isolated provider profile"); return;
        }
        pump_retired_profiles(state);
        const auto slot = std::find(state.profiles.begin(), state.profiles.end(), nullptr);
        if (slot == state.profiles.end()) {
            state.fail(out, NILWV_E_LIMIT, "WebView private profile limit reached; closed browser profiles are still draining"); return;
        }
        auto value = std::make_shared<WinWindow>();
        if (!state.add_window(value, out)) return;
        state.native_windows.erase(std::remove_if(state.native_windows.begin(), state.native_windows.end(), [](const auto &item) { return item.expired(); }), state.native_windows.end());
        state.native_windows.push_back(value);
        value->binding = {context->state, value};
        try { value->profile = create_profile(); *slot = value->profile; }
        catch (...) { fail_initialization(state, value, "WebView2 private profile could not be created"); return; }
        value->handle = create_window(*value, wide(text), static_cast<int>(width), static_cast<int>(height));
        if (!value->handle) { fail_initialization(state, value, "WebView2 host window could not be created"); return; }
        auto options = Make<CoreWebView2EnvironmentOptions>();
        if (!options || FAILED(options->put_AdditionalBrowserArguments(L"--disable-gpu --disable-background-networking")) ||
            FAILED(options->put_AllowSingleSignOnUsingOSPrimaryAccount(FALSE)) || FAILED(options->put_ExclusiveUserDataFolderAccess(TRUE))) {
            fail_initialization(state, value, "WebView2 isolated environment options unavailable"); return;
        }
        const std::weak_ptr<WinContext> weak_context = context->state;
        const std::weak_ptr<WinWindow> weak_window = value;
        const auto profile = value->profile;
        const auto completion = std::make_shared<Completion>(profile);
        const auto result = CreateCoreWebView2EnvironmentWithOptions(nullptr, profile->directory.c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [weak_context, weak_window, profile, completion](HRESULT error, ICoreWebView2Environment *environment) -> HRESULT {
                    const FinishCompletion finished{completion};
                    bool observed = false;
                    if (SUCCEEDED(error) && environment) {
                        observed = observe_browser_exit(profile, environment);
                        if (!observed) profile->await_browser_exit = true;
                    }
                    callback(weak_context, weak_window, [&](auto &owner, const auto &live_window) {
                        if (live_window->failed) return;
                        if (FAILED(error) || !environment) { fail_initialization(owner, live_window, "WebView2 environment initialization failed"); return; }
                        ComPtr<ICoreWebView2Environment7> identified;
                        WideText actual;
                        if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&identified))) ||
                            FAILED(identified->get_UserDataFolder(&actual.value)) || !actual.value ||
                            _wcsicmp(actual.value, profile->directory.c_str()) != 0) {
                            fail_initialization(owner, live_window, "WebView2 did not honor its isolated profile directory"); return;
                        }
                        if (!observed) {
                            fail_initialization(owner, live_window, "WebView2 browser resource lifetime observation unavailable"); return;
                        }
                        WideText runtime_version;
                        std::string version;
                        if (FAILED(environment->get_BrowserVersionString(&runtime_version.value)) ||
                            !runtime_version.value || !narrow(runtime_version.value, version, 128) || version.empty()) {
                            fail_initialization(owner, live_window, "WebView2 environment runtime version unavailable"); return;
                        }
                        // Actual environment identity, independent of the SDK
                        // loader version and probe's available-runtime check.
                        live_window->runtime_detail = "WebView2 " + version;
                        live_window->environment = environment;
                        const auto controller_completion = std::make_shared<Completion>(profile);
                        const bool browser_already_started = profile->await_browser_exit;
                        profile->await_browser_exit = true;
                        const auto creation = environment->CreateCoreWebView2Controller(live_window->handle,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [weak_context, weak_window, profile, controller_completion](HRESULT failure, ICoreWebView2Controller *controller) -> HRESULT {
                                    const FinishCompletion finished{controller_completion};
                                    bool retained = false;
                                    callback(weak_context, weak_window, [&](auto &, const auto &ready_window) {
                                        if (ready_window->failed) return;
                                        if (FAILED(failure) || !controller) {
                                            auto owner = weak_context.lock();
                                            if (owner) fail_initialization(*owner, ready_window, "WebView2 controller initialization failed");
                                            return;
                                        }
                                        ready_window->controller = controller; retained = true;
                                        if (auto owner = weak_context.lock()) initialize_controller(owner, ready_window);
                                    });
                                    // A closed/destroyed context cannot retain a late controller.
                                    if (!retained && controller) {
                                        ComPtr<ICoreWebView2> abandoned; UINT32 pid = 0;
                                        if (!profile->browser && SUCCEEDED(controller->get_CoreWebView2(&abandoned)) &&
                                            SUCCEEDED(abandoned->get_BrowserProcessId(&pid)) && pid)
                                            profile->browser = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                                        controller->Close();
                                    }
                                    return S_OK;
                                }).Get());
                        if (FAILED(creation)) {
                            controller_completion->finish();
                            profile->await_browser_exit = browser_already_started;
                            fail_initialization(owner, live_window, "WebView2 controller creation unavailable");
                        }
                    });
                    return S_OK;
                }).Get());
        if (FAILED(result)) {
            completion->finish();
            fail_initialization(state, value, "WebView2 environment creation unavailable");
        }
    });
}
void show(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) { auto base = state.window(id, out); if (!base) return;
        auto value = std::static_pointer_cast<WinWindow>(base); ShowWindow(value->handle, SW_SHOW); success(out); });
}
void navigate_common(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 input, nilwv_result_v1 *out, bool html) noexcept {
    call(context, out, [&](WinContext &state) {
        std::string data; if (!state.read_text(input, data, out, html)) return;
        auto base = state.window(id, out); if (!base) return;
        auto value = std::static_pointer_cast<WinWindow>(base);
        const auto text = wide(data); ComPtr<IUri> uri;
        if ((!html && !navigation_uri(data)) || FAILED(CreateUri(html ? L"about:blank" : text.c_str(), Uri_CREATE_CANONICALIZE, 0, &uri))) {
            state.fail(out, NILWV_E_ARGUMENT, "WebView navigation requires an HTTP(S) URI or about:blank"); return;
        }
        auto html_uri = html ? wide(html_navigation_uri(data)) : std::wstring{};
        bool fragment = false, repeat = false;
        if (!html) {
            WideText current;
            if (SUCCEEDED(value->view->get_Source(&current.value))) fragment = fragment_only(uri.Get(), current.value, repeat);
        }
        const auto operation = state.start(value, Operation::navigation, out); if (!operation) return;
        value->admit_request = true; value->policy_cancelled = false; value->navigation_id = 0;
        value->requested_uri = uri; value->navigation_operation = operation;
        value->requested_html_uri = std::move(html_uri);
        value->fragment_request = fragment; value->fragment_repeat = repeat;
        const HRESULT result = html ? value->view->NavigateToString(text.c_str()) : value->view->Navigate(text.c_str());
        if (FAILED(result)) {
            value->admit_request = false; value->fragment_request = false; value->fragment_repeat = false;
            value->navigation_operation = 0;
            value->requested_html_uri.clear();
            state.reject(value, operation, out, "WebView2 rejected the navigation");
        }
    });
}
void navigate(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 text, nilwv_result_v1 *out) noexcept { navigate_common(context, id, text, out, false); }
void load_html(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 text, nilwv_result_v1 *out) noexcept { navigate_common(context, id, text, out, true); }
void evaluate(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 input, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) {
        std::string data; if (!state.read_text(input, data, out)) return;
        auto base = state.window(id, out); if (!base) return; auto value = std::static_pointer_cast<WinWindow>(base);
        const auto script = wide(evaluation_script(data));
        const auto operation = state.start(value, Operation::script, out); if (!operation) return;
        const std::weak_ptr<WinContext> weak_context = context->state;
        const std::weak_ptr<WinWindow> weak_window = value;
        const auto completion = std::make_shared<Completion>(value->profile);
        const auto result = value->modern->ExecuteScriptWithResult(script.c_str(),
            Callback<ICoreWebView2ExecuteScriptWithResultCompletedHandler>(
                [weak_context, weak_window, operation, completion](HRESULT error, ICoreWebView2ExecuteScriptResult *result) -> HRESULT {
                    const FinishCompletion finished{completion};
                    callback(weak_context, weak_window, [&](auto &owner, const auto &live_window) {
                        if (FAILED(error) || !result) { owner.script_failed(live_window, operation, "WebView2 script execution failed", NILWV_FAILURE_NATIVE); return; }
                        BOOL okay = FALSE;
                        if (FAILED(result->get_Succeeded(&okay))) { owner.script_failed(live_window, operation, "WebView2 script outcome unavailable", NILWV_FAILURE_NATIVE); return; }
                        if (!okay) { owner.script_failed(live_window, operation, "WebView2 script raised an exception"); return; }
                        BOOL is_string = FALSE; WideText raw; std::string encoded;
                        if (FAILED(result->TryGetResultAsString(&raw.value, &is_string)) || !is_string) {
                            owner.script_failed(live_window, operation, "WebView2 script envelope was not text", NILWV_FAILURE_SERIALIZATION); return;
                        }
                        if (!narrow(raw.value, encoded, NILWV_MAX_TEXT_BYTES + 2)) {
                            owner.script_failed(live_window, operation, "WebView2 script result exceeds text contract", NILWV_FAILURE_PAYLOAD_LIMIT); return;
                        }
                        owner.script_finished(live_window, operation, encoded);
                    });
                    return S_OK;
                }).Get());
        if (FAILED(result)) {
            completion->finish();
            state.reject(value, operation, out, "WebView2 rejected script execution");
        }
    });
}
void poll(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) {
        if (!state.window(id, out, false)) return;
        const auto until = Clock::now() + std::chrono::milliseconds(2);
        MSG message{};
        for (unsigned iteration = 0; iteration < 64 && Clock::now() < until; ++iteration) {
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        // Repeating an already-current fragment has no SourceChanged event.
        // After pumping the STA, verify the explicitly requested native Source;
        // this shortcut cannot complete an ordinary same-URL document reload.
        for (const auto &weak : state.native_windows) if (auto value = weak.lock(); value &&
            value->fragment_repeat && pending_fragment(value) && value->view) {
            WideText current;
            if (FAILED(value->view->get_Source(&current.value)))
                state.provider_failed(value, "WebView2 repeated fragment source unavailable");
            else finish_fragment(state, value, current.value);
        }
        state.expire_operations();
        for (const auto &weak : state.native_windows) if (auto value = weak.lock(); value && value->failed) {
            value->admit_request = false;
            if (value->view) value->view->Stop();
        }
        collect_profiles(state, Clock::now() + std::chrono::milliseconds(2), 256);
        state.poll(id, out);
    });
}
void event_number(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 field, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) { std::string name; if (state.read_text(field, name, out)) state.event_number(id, name, out); });
}
void event_text(nilwv_context_v1 *context, std::int64_t id, nilwv_text_v1 field, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) { std::string name; if (state.read_text(field, name, out)) state.event_text(id, name, out); });
}
void release_event(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) { state.release_event(id, out); }, true);
}
void close(nilwv_context_v1 *context, std::int64_t id, nilwv_result_v1 *out) noexcept {
    call(context, out, [&](WinContext &state) { auto value = state.close(id, out); if (value) dispose(std::static_pointer_cast<WinWindow>(value)); }, true);
}
const nilwv_api_v1 api = {NILWV_ABI_V1, sizeof(nilwv_api_v1), NILWV_CAP_MESSAGE_URL | NILWV_CAP_MESSAGE_FRAME,
    "WebView2", "nilwebview-v1-sdk-1.0.4314.35", probe, create, destroy, window, show, navigate,
    load_html, evaluate, poll, event_number, event_text, release_event, close};
} // namespace
} // namespace nilwebview::win32
extern "C" NILWV_EXPORT const nilwv_api_v1 *NILWV_CALL
nilwebview_provider_v1_get_api(std::uint32_t version, std::uint32_t size) {
    if (version != NILWV_ABI_V1 || size > sizeof(nilwv_api_v1)) return nullptr;
    return &nilwebview::win32::api;
}
