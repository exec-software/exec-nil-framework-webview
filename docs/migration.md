# Migrating from Ny WebView

nil-webview is a Matrix rewrite with a checked framework/provider boundary.
It is not a source-compatible replacement for the archived Ny wrapper or its
native exports. The archive remains historical reference; applications should
import `nilwebview`, not bind directly to its old native symbols.

## Every legacy public WebView function

The old Ny module exposed these 21 functions:

| Legacy function | Initial Matrix profile |
| --- | --- |
| `create(title, width, height)` | `create-window`; returns `Result of Window, text`, then asynchronous `Ready`/`InitializationFailed`. |
| `navigate(handle, url)` | `navigate`; returns an operation identity. HTTP, HTTPS and `about:blank` only, with explicit redirect approval. |
| `run(handle)` | Replace the blocking loop with repeated `poll-event` calls on the UI thread. |
| `is_closed(handle)` | Track application state; handle `CloseRequested`, call `close-window`, then stop using the handle. No direct query. |
| `poll_message(handle)` | `poll-event` and `EventKind.PageMessage`; empty queue is `Option.None`, distinct from an empty JSON string. |
| `set_html(handle, html)` | `load-html`; asynchronous completion, no base URI. |
| `load_html_base(handle, html, base_uri)` | Outside the initial profile. Use self-contained HTML or an explicitly authorized HTTP(S) application URL. |
| `eval_js(handle, js)` | `evaluate-function`; function-body syntax with explicit `return`, serialized completion and explicit errors. |
| `close(handle)` | `close-window`; checked context/lifetime, duplicate close is an error. |
| `minimize(handle)` | Outside the initial profile; no programmatic window-state API. |
| `maximize(handle)` | Outside the initial profile. |
| `is_maximized(handle)` | Outside the initial profile. |
| `drag(handle)` | Outside the initial profile; use the native window frame. |
| `set_position(handle, x, y, w, h)` | Outside the initial profile; only initial width/height are configurable. |
| `get_x(handle)` | Outside the initial profile. |
| `get_y(handle)` | Outside the initial profile. |
| `get_w(handle)` | Outside the initial profile. |
| `get_h(handle)` | Outside the initial profile. |
| `pick_directory(handle)` | Outside the initial profile; no native dialog wrapper. |
| `start_bridge(handle, watch_path, base_uri, interval_ms)` | Outside the initial profile; no file-watching, command bridge or implicit reload. |
| `set_icon(handle, path)` | Outside the initial profile. |

Window controls supplied by the operating system may still be visible. That
does not expose the corresponding programmatic legacy API.

The old companion process module is also outside this package. Its `spawn`,
`write_stdin`, `read_stdout`, `read_stderr`, `is_alive`, `exit_code`, `kill`,
`close_process`, `create_directory`, `open_folder_dialog` and `open_file_dialog`
functions are not silently provided by page messaging.

## Changes applications must make

1. Select a trusted native provider by absolute path through `configure`.
   The runtime validates its versioned C ABI and keeps the library loaded.
2. Replace raw integer handles and sentinel failures with typed `Window`,
   `Operation`, `Event`, `Result` and `Option` values.
3. Wait for readiness and match operation completion identities. Native
   initialization and JavaScript evaluation are asynchronous even though the
   JavaScript function body itself is synchronous.
4. Send page values with `window.nilwebview.post(value)`. Receive serialized
   JSON; do not interpret it as a shell command or use claimed JSON origins as
   sender authentication.
5. Handle explicit navigation requests, failures, timeouts and close requests.
   A timed-out window requires close/recreate; it cannot quietly resume.

Linux now targets GTK4 with WebKitGTK 6 rather than GTK3 with older WebKitGTK
APIs. macOS uses WKWebView directly. Native providers have different provenance
capabilities; an application requiring authenticated sender metadata must check
each event and cannot assume the Windows path applies on GTK.

The [message-console example](../examples/message-console/src/main.matrix)
demonstrates the basic lifecycle. The [platform evidence](platform-support.md)
separates implementation from actual browser qualification.
