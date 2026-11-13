# Platform support and evidence

The current provider profile is implemented for the four hosts below. Each
host has a required real-browser check; implementation alone is not a passing
qualification result or an installed-SDK compatibility guarantee.

| Host | Native provider | Native page-message metadata | Required browser check |
| --- | --- | --- | --- |
| Linux x86-64 | GTK4 + WebKitGTK 6 | None; capabilities are zero, sender fields/frame unknown | `webview-linux-x86_64` |
| Windows x86-64 | Win32 + WebView2 | Native sender URL and main-frame identity from the top-level message event; origin unavailable | `webview-windows-x86_64` |
| macOS Intel | AppKit + WKWebView | Native frame URL, security origin when available, and frame identity | `webview-macos-x86_64` |
| macOS Apple Silicon | AppKit + WKWebView | Same API profile as Intel, tested independently | `webview-macos-arm64` |

See the [current platform runs](https://github.com/exec-software/exec-nil-framework-webview/actions/workflows/test.yml)
and the [initial qualification PR](https://github.com/exec-software/exec-nil-framework-webview/pull/1).
A run is qualified only when its required jobs pass. Cancelled builds, missing
desktops, partial route results and retained private profiles are failures or
incomplete evidence, not platform passes.

## Source qualification evidence

The workflow pins framework revision
`e7bc2a00e6854084b7b5084e29fa50456cbe8977`. Each job records its actual WebView
checkout (including GitHub's PR merge revision), framework revision, host,
toolchain, standard library and browser dependency identities. Download the
job's evidence artifact to inspect those identities, exact contract inventories,
route output, cleanup results and unchanged-source hashes.

Each host must verify seven exact framework/provider contracts: 57 runtime
checks, 28 WebView ABI checks, 40 UI ABI checks, 216 formatted-text checks,
392 markup checks, 209 shared-provider checks and 36 JavaScript transport
checks. It then executes four synthetic and four actual browser consumers
through textual NIL, binary NIL, AOT and JIT. Every real-browser route must pass
all 17 behavior groups, including navigation policy, messages, dialogs, script
failures, copied events and window closure. No contract may be skipped.

The browser fixture includes an initial full-document URL with a fragment,
changed and repeated same-document fragments, preserved DOM and JavaScript
state, a cancelled redirect followed by explicit host navigation, and a real
HTTP error document. Its owned HTTP server must observe exactly one request
each for the initial page, redirect, explicit follow and error document.
Windows enforces initial document requests before network dispatch as well as
cancelling unapproved navigation: [WebView2 documents that navigation cancellation alone can allow an early GET](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2navigationstartingeventargs#get_cancel).
Ordinary subresources retain normal browser behavior. This is not a general
network sandbox.

All 14 owned commands and the loopback HTTP fixture must clean up, and source,
toolchain, standard-library and copied-provider hashes must remain unchanged.
Windows also checks for private profile remnants after every browser route,
including a window closed during asynchronous initialization. Linux verifies a
real browser namespace launch and restores the exact pre-test AppArmor profile
inventory. Mac evidence records the actual OS version for its system WKWebView;
Windows records the actual WebView2 environment version separately from the
pinned SDK/loader version.

## Dependencies and builds

All builds require CMake 3.20 or later and a C++17 compiler. Tests also require
Node.js. Matrix consumers require the compatible NIL Framework `webview.v1`
bridge and its standard library; an older SDK is not sufficient.

Linux uses `gtk4 >= 4.0` and `webkitgtk-6.0 >= 2.44` through pkg-config. On
Ubuntu 24.04 the development packages are `libgtk-4-dev`,
`libwebkitgtk-6.0-dev` and `pkg-config`. Use the system packages' update channel
for browser security updates. The new provider does not link GTK3 or
`webkit2gtk-4.0`/`webkit2gtk-4.1` as a fallback. Desktop tests need an owned
display and session bus; absence of a working display is not a successful test.

Windows uses a 64-bit MSVC build and the checksum-verified Microsoft WebView2
SDK acquired by `scripts/fetch-webview2.py`. Pass the extracted SDK directory
through CMake's `NILWEBVIEW_WEBVIEW2_SDK` option. The SDK/loader version and the
installed WebView2 browser runtime version are separate dependencies. Record
both in qualification evidence. The initial qualification target is x86-64;
CMake architecture selection alone does not qualify Windows ARM64. All calls
must use the creating STA thread; the provider does not infer process-main
identity from thread creation timestamps. Closed profiles retain a slot until
their initialization callbacks and browser resources drain; at most 16 active
or retiring profiles exist per context. Context destruction pumps the STA and
waits for owned completions and browser-resource release for a shared maximum
of 35 seconds before returning. A cleanup failure emits a diagnostic and
retains the private directory as evidence. No detached cleanup worker or
process-name-based termination is used.

macOS uses the system AppKit and WebKit frameworks and Objective-C++ with ARC.
Install Xcode Command Line Tools or use a suitable Xcode runner. The system
WebKit version follows the OS; record the OS, architecture and toolchain when
testing. Build Intel and Apple Silicon independently. A compile-only hosted
job does not demonstrate an available window server or functioning browser.

## Qualification expectations

A browser qualification run must use the actual provider and compatible
framework, exercise the real Matrix application boundary, and retain bounded
evidence. Core behavior includes readiness, show/close, inline HTML, host
navigation, cancelled redirects followed by explicit application navigation,
script JSON/undefined/errors, page messages, truthful provenance, stale handles
and owned-resource cleanup. A native-route claim requires that route to execute;
compiling it alone is insufficient.

Keep the tested framework source, provider source, compiler/interpreter,
standard library and browser dependency identities in the report. Distinguish
source builds from installed SDKs. Record failures, missing desktop sessions
and skipped assertions explicitly. Do not activate a candidate SDK or replace a
user installation as part of qualification.

Hosted Linux qualification grants only the installed WebKit bubblewrap
executable a temporary AppArmor user-namespace allowance, verifies a real
namespace launch, and removes the owned profile afterward. The WebKit sandbox
stays enabled; no global user-namespace policy is relaxed. Hosted Windows
qualification disables optional MSVC telemetry and temporarily removes only the
selected compiler's verified optional uploader. Its bytes, owner/group/DACL and
saved telemetry preferences are restored in the job's final cleanup. These
steps target disposable GitHub runners, not developer machines.
CMake's Windows probes and builds use embedded MSVC debug information (`/Z7`)
to avoid leaving a persistent compiler PDB server after the owning command exits.

## Distribution

Original nil-webview source is MIT licensed. GTK/WebKitGTK and their dependencies
have separate notices and redistribution obligations. Microsoft licenses its
WebView2 SDK, loader and browser runtime separately. Apple supplies its system
frameworks under Apple's terms. Source integration does not grant permission
to bundle those engines in an SDK. Preserve notices and use the platform's
supported runtime distribution mechanism when packaging an application.

Provider libraries are selected explicitly and pinned for process lifetime.
Browser profile state is isolated from personal profiles; cleanup may outlive
the last window while native browser processes shut down. Windows context
teardown performs the bounded drain described above before the consumer exits;
qualification must fail if a private profile remains. No provider hot unload, legacy command bridge or generic arbitrary-native-call route is part of
this profile.
