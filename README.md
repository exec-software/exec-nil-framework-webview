# nil-webview

Native browser windows for [Matrix and the NIL Framework](https://github.com/exec-software/exec-nil-framework).
The public API is written in Matrix; small providers use GTK4/WebKitGTK on Linux,
WebView2 on Windows, and WKWebView on macOS.

[API](docs/api.md) · [Platform support](docs/platform-support.md) ·
[Migration](docs/migration.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)

[![Support us on Ko-fi](https://img.shields.io/badge/Support_us-Ko--fi-FF5E5B?logo=kofi&logoColor=white)](https://ko-fi.com/execsoftware)

## Status

The current source profile is implemented on Linux, Windows, Intel macOS and
Apple Silicon macOS. Each platform has a required real-browser check through
textual NIL, binary NIL, native compilation and JIT. See
[platform support and evidence](docs/platform-support.md) for the exact scope
and [current checks](https://github.com/exec-software/exec-nil-framework-webview/actions/workflows/test.yml)
for qualification results. These source checks do not qualify a published SDK.

The current profile provides hidden-window creation, explicit showing,
navigation, loading HTML, synchronous JavaScript function evaluation, copied
events, page messages and closing. It does not provide every feature of the
archived Ny WebView library; the [migration table](docs/migration.md) records
each legacy function.

## How it works

Import `nilwebview` and configure an explicit absolute path to a trusted native
provider library. Create a window, poll for `Ready`, and then show it or start
an operation. Polling drives the browser on the process UI thread. Navigation
and script calls return operation identities; their completion events carry
the same identities. The application decides when to close a window.

Pages send JSON-serializable values through:

```javascript
window.nilwebview.post({ kind: "message", text: "Hello from the page" });
```

Matrix receives a `PageMessage` event containing serialized JSON text. This
bridge does not interpret messages as commands, launch processes, or grant
pages filesystem access. Applications decide what each message means.

Sender metadata has explicit capabilities and quality. GTK currently provides
no authenticated sender URL, origin or frame metadata. A native sender URL
alone is not proof of origin. Applications that require origin or frame checks
must reject messages when the required native metadata is unavailable.

Navigation accepts HTTP, HTTPS and `about:blank`. Each explicit call authorizes
one initial request. Redirects and uninitiated document loads are cancelled and
reported as `NavigationRequested`; the application may validate the requested
destination and explicitly navigate after the previous operation completes.
Same-document fragment and history changes may update the current URL without
loading another document; they do not grant permission to load a new page.

## Build and try the example

Use a compatible NIL Framework build containing the checked `webview.v1`
intrinsics. A previous SDK without that bridge cannot run this revision.
See [platform support](docs/platform-support.md) for provider dependencies.
The exact source revision used by CI is pinned in
[framework-revision.txt](framework-revision.txt). The four platform jobs build
that compiler and standard library from source, verify the shared ABI headers,
and run both synthetic and actual-browser Matrix consumers through interpreted
and native execution. The same jobs also check the pinned framework's formatted
text and markup contracts with LLVM enabled on all four hosts. Provider binaries and SDKs are not published by these
source checks.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

On Windows, first fetch the pinned WebView2 SDK with the repository's fetch
script and pass its extracted directory as `NILWEBVIEW_WEBVIEW2_SDK` to CMake.
WebView2 also requires an installed compatible browser runtime.

The [message-console example](examples/message-console/src/main.matrix) loads
its bundled HTML, prints page messages and a JavaScript result in the terminal,
and closes when its native window is closed. It needs no HTTP server. Its
program arguments are `--provider ABSOLUTE_LIBRARY --html ABSOLUTE_HTML_FILE`.
The runner compiles the example and supplies its bundled HTML path:

```sh
python3 scripts/run-example.py --compiler "$NIL_COMPILER" \
  --interpreter "$NIL_INTERPRETER" --nilstd "$NIL_STD" \
  --provider "$NILWEBVIEW_PROVIDER"
```

Set those variables to the compatible tools, standard-library source directory
and absolute provider library path. To compile directly, put the repository's
`src` directory on the Matrix import path and pass the program arguments after
the interpreter's `--` separator. Use the example project's
`nil.project.nildata` when working through `nilbuild` or the editor.

The provider output is normally `build/native/libnilwebview.so` on Linux,
`build/native/libnilwebview.dylib` on macOS, or
`build/native/Release/nilwebview.dll` with a Visual Studio generator on Windows.
Resolve the actual output to an absolute path; loading code from an arbitrary
download or untrusted writable directory is not a supported trust boundary.

## Limits and lifecycle

Text is valid UTF-8 without embedded NUL, with a 1 MiB limit per field. JSON can
represent NUL as an escape. Contexts allow at most 16 windows; provider queues
hold at most 256 events and 8 MiB including retained provider snapshots and
metadata. Framework snapshot copies are also bounded. Initialization and
operations have a 30-second deadline checked while polling.

A timeout makes the affected window unusable for new operations; close it and
create another. Queue overflow is an explicit failure. The event pump has a
bounded dispatch batch with a soft time budget; it is not a hard real-time
deadline. See [the API contract](docs/api.md) for ownership and failure details.

## Layout

```text
src/nilwebview/           Matrix API, types and copied event handling
native/include/          versioned provider C ABI and JSON grammar
native/src/              shared controls and platform providers
tests/                   shared, transport, framework and desktop controls
examples/message-console/ local HTML and Matrix application
docs/                    API, migration and platform evidence
scripts/                 build/test support and source audit
```

## Support and license

You can support Execute Software through [Ko-fi](https://ko-fi.com/execsoftware).
Donations are optional and do not change the project's features or license.
Original project materials use the [MIT License](LICENSE). Platform browser
engines, loaders and SDKs retain their own licenses and distribution terms.
