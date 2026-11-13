# Platform support and evidence

The initial provider implementations target the four hosts below. Actual
browser qualification is pending; this table must not be read as a release or
SDK compatibility guarantee.

| Host | Native provider | Native page-message metadata | Current browser evidence |
| --- | --- | --- | --- |
| Linux x86-64 | GTK4 + WebKitGTK 6 | None; capabilities are zero, sender fields/frame unknown | Pending |
| Windows x86-64 | Win32 + WebView2 | Native sender URL and main-frame identity from the top-level message event; origin unavailable | Pending |
| macOS Intel | AppKit + WKWebView | Native frame URL, security origin when available, and frame identity | Pending |
| macOS Apple Silicon | AppKit + WKWebView | Same API profile as Intel; requires its own architecture run | Pending |

Shared controls have local native coverage, including owner-thread and handle
checks, timeouts, queue limits, immutable snapshots, JSON validation and actual
generated JavaScript transport tests. Matrix wrapper checks with a synthetic
provider pass through textual NIL, binary NIL, native compilation and JIT.
These establish the checked interface, not a real engine's behavior.

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
