# Platform support and evidence

The current provider profile targets the four hosts below. The table records
actual source-consumer tests, not a release or installed-SDK compatibility
guarantee. Windows browser qualification remains in progress.

| Host | Native provider | Native page-message metadata | Current browser evidence |
| --- | --- | --- | --- |
| Linux x86-64 | GTK4 + WebKitGTK 6 | None; capabilities are zero, sender fields/frame unknown | Four real browser routes passed ([run](https://github.com/exec-software/exec-nil-framework-webview/actions/runs/43468602639)) |
| Windows x86-64 | Win32 + WebView2 | Native sender URL and main-frame identity from the top-level message event; origin unavailable | Build, synthetic routes and real initialization/messages pass; fragment qualification in progress |
| macOS Intel | AppKit + WKWebView | Native frame URL, security origin when available, and frame identity | Four real browser routes passed ([run](https://github.com/exec-software/exec-nil-framework-webview/actions/runs/43468602639)) |
| macOS Apple Silicon | AppKit + WKWebView | Same API profile as Intel, tested independently | Four real browser routes passed ([run](https://github.com/exec-software/exec-nil-framework-webview/actions/runs/43466470278)) |

Shared controls have local native coverage, including owner-thread and handle
checks, timeouts, queue limits, immutable snapshots, JSON validation and actual
generated JavaScript transport tests. Matrix wrapper checks with a synthetic
provider pass through textual NIL, binary NIL, native compilation and JIT.
These establish the checked interface, not a real engine's behavior.

## Recorded source qualification

The November 13, 2026 runs above use framework revision
`e7bc2a00e6854084b7b5084e29fa50456cbe8977`. Linux and Intel Mac tested the
`e34c495` WebView branch through GitHub's `9d9c156` merge revision; Apple
Silicon tested `59330e5` through merge revision `8b99c54`.

Each passing host verified seven exact framework/provider contracts: 57
runtime checks, 28 WebView ABI checks, 40 UI ABI checks, 216 formatted-text
checks, 392 markup checks, 196 shared-provider checks and 36 JavaScript
transport checks. It then executed four synthetic and four actual browser
consumers through textual NIL, binary NIL, AOT and JIT. Each browser route
passed all 16 behavior groups, including navigation policy, messages, dialogs,
script failures, copied events and window closure. No contract was skipped.

All 14 owned commands and the loopback HTTP fixture cleaned up; source,
toolchain, standard-library and copied-provider hashes remained unchanged.
Linux used GTK 4.14.5 and WebKitGTK 2.52.6, and restored the exact pre-test
AppArmor profile inventory. The Mac runs used system WKWebView on macOS
15.7.9 (Intel) and 15.8.2 (Apple Silicon). Windows' first browser run failed
before `Ready`; its failure evidence and successful process/profile cleanup
are retained. The follow-up records the actual WebView2 environment version
for each initialized window as well as the separately pinned SDK version.
The [subsequent navigation run](https://github.com/exec-software/exec-nil-framework-webview/actions/runs/43474498040)
reached real Windows readiness and message delivery on WebView2 131.0.2903.86.
Its additional fragment regression exposed document reload differences in the
native engines. The follow-up defines changed-fragment navigation to preserve
the document and exact repeated fragments to complete idempotently. It requires
17 browser behavior groups, separate changed/repeated fragment observations,
preserved JavaScript state and no additional HTTP document request.

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
