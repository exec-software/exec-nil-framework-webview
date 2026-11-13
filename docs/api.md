# Matrix WebView API

Import `nilwebview`. The module uses the checked `webview.v1` framework bridge
on both interpreter and native routes. All calls belong to the creating UI
thread: the process main thread on macOS, the toolkit owner thread on Linux,
and the creating STA thread on Windows. No native callback enters Matrix;
the application polls events.

## Functions

| Function | Result | Meaning |
| --- | --- | --- |
| `configure(provider-path: text)` | `Result of Capabilities, text` | Select an explicit absolute trusted provider path. Repeating the same selection is allowed; switching an active context is rejected. |
| `create-window(title: text, width: number, height: number)` | `Result of Window, text` | Create a hidden pending window; dimensions are 1–16384. Poll for readiness. |
| `show-window(surface: Window)` | `Result of boolean, text` | Show a ready window. Success contains `yes`. |
| `navigate(surface: Window, url: text)` | `Result of Operation, text` | Start one HTTP, HTTPS or `about:blank` navigation. |
| `load-html(surface: Window, html: text)` | `Result of Operation, text` | Load inline HTML without a base URI. |
| `evaluate-function(surface: Window, body: text)` | `Result of Operation, text` | Execute a synchronous JavaScript function body; use an explicit `return` for a value. |
| `poll-event(surface: Window)` | `Result of PendingEvent, text` | Pump a bounded batch and return `Option.Some holding Event`, or `Option.None` when empty. |
| `close-window(surface: Window)` | `Result of boolean, text` | Invalidate the window and its outstanding operations/events. Closing twice is an error. |

`Window.handle` and `Operation.id` are opaque numbers, checked against the
execution context and lifetime. Do not fabricate them or persist them across
executions. `Operation.window` identifies its associated window. `PendingEvent`
is an alias for `Option of Event`.

## Window and operation lifecycle

Creation success means that a pending handle exists. It is followed by exactly
one `Ready` or `InitializationFailed`, unless the application closes it first.
Wait for `Ready` before showing or starting an operation.

Only one navigation or script operation may be active per window. An overlapping
request fails immediately. An accepted operation returns its identity and later
produces one matching completion/failure while the window remains open. Compare
both the window and operation identity; do not assume the next event is its
completion. Page messages can arrive before or after a script completion.

`CloseRequested` asks the application to close; it does not silently destroy the
window. `close-window` cancels its pending work and invalidates retained runtime
snapshots. Already copied Matrix `Event` values remain ordinary values. A late
native callback cannot publish into a closed window.

Initialization and operations have 30-second deadlines, checked during polling.
A timeout produces the corresponding failure with `FailureKind.Timeout` and
makes the window terminal: it must be closed and recreated. Queue overflow is
an explicit context failure; execution-context teardown releases its resources.
Do not assume that cancelling an evaluation stops JavaScript execution.

## Events

An `Event` contains these fields:

| Field | Meaning |
| --- | --- |
| `window` | The owning `Window`. |
| `kind` | One of the event kinds below. |
| `sequence` | Positive provider sequence identity; gaps are allowed. |
| `operation` | `Option of Operation`; `None` means no initiating host operation. |
| `value-kind` | `None`, `Json` or `Undefined`. |
| `payload-json` | Serialized JSON text for `Json`; otherwise empty. |
| `source-kind` | `Unknown`, `NativeURL` or `NativeOrigin`. |
| `source-url`, `source-origin` | Native sender metadata, empty when unavailable. |
| `frame-kind` | `Unknown`, `Main` or `Child`. |
| `failure-kind` | Failure classification, or `None`. |
| `detail` | Diagnostic text, not a stable machine-readable error code. |

| Event kind | Payload or purpose |
| --- | --- |
| `Ready` / `InitializationFailed` | Result of creating the pending window. |
| `NavigationCompleted` / `NavigationFailed` | Completion of a host navigation; URI payloads are JSON strings, not sender provenance. |
| `ScriptCompleted` / `ScriptFailed` | Evaluation result or failure for the matching operation. |
| `PageMessage` | Serialized page JSON plus available native sender metadata. |
| `NavigationRequested` | Cancelled navigation destination as a JSON string; the application decides whether to follow. |
| `CloseRequested` | Native/page close request; application calls `close-window`. |
| `ProviderFailed` | Browser failure; close the affected window. |

Failure kinds are `None`, `Native`, `ScriptException`, `Serialization`,
`AsyncResult`, `PayloadLimit`, `Timeout` and `Cancelled`. Native diagnostic
wording may differ by platform.

On Windows, `Ready.detail` includes `WebView2 ` followed by the browser version
reported by that window's actual environment. It identifies the runtime used
by the window, independently of the SDK/loader version used at build time.
Treat it as a diagnostic; it does not replace capability checks.

`poll-event` copies every event field and releases the runtime snapshot on both
success and error paths. Applications do not call native snapshot-release APIs.
The pump processes at most 64 native dispatch iterations with a 2 ms soft
budget. One native dispatch may deliver multiple callbacks and take longer;
this is not a hard execution-time guarantee.

## JavaScript and JSON

```javascript
// Page to Matrix:
window.nilwebview.post({ kind: "message", value: 42 });

// Body supplied by Matrix to evaluate-function:
return { title: document.title, value: 42 };
```

The page shim is installed in the main frame. Its existence is not sender
authentication: platform transports can have different frame exposure, and the
event's native provenance fields determine what is known.

Evaluation uses `JSON.stringify` semantics. `null` is JSON text `null`;
`undefined` is the distinct `ValueKind.Undefined`. Nonfinite numbers serialize
as JSON null. Promises and thenables are rejected with `AsyncResult`; v1 does not
await them. Cycles, BigInt values and other unsuccessful serialization produce
`Serialization`; an exception from the function body produces `ScriptException`.

JSON is validated as text without numeric conversion or Unicode normalization.
Escaped lone UTF-16 surrogates produced by `JSON.stringify` remain escaped text.
Nested JSON is bounded at depth 64. Raw transport text must be valid UTF-8
without NUL; JSON escapes such as `\u0000` can represent those values. Consumers
must choose an appropriate JSON parser before treating payloads as structured
data. Never concatenate untrusted messages into evaluated JavaScript.

## Navigation policy

Only an explicit `navigate` authorizes an initial URL request. Redirects,
page-initiated navigation and new-window requests are cancelled and reported
as `NavigationRequested`. If an active navigation was cancelled, it completes
with `NavigationFailed` and `FailureKind.Cancelled`. Wait for that completion,
decode the requested JSON string, apply the application's destination policy,
then start a new `navigate` if approved. Do not automatically follow every
request. This policy is common to all initial providers.

`load-html` supplies no base URI. Relative assets, filesystem navigation,
downloads and browser-managed popups are outside the initial profile. JavaScript
alert/confirm/prompt dialogs are suppressed with immediate default responses;
they do not open modal native dialogs or block the host loop.

## Provenance and limits

`Capabilities.sender-url`, `sender-origin` and `sender-frame` describe the
provider's native metadata support. All three may be `no`; this is a valid
configuration. They do not grant page permissions or guarantee metadata for
every URL scheme.

`Unknown` has empty sender fields. `NativeURL` provides a native sender URL but
no authenticated origin. `NativeOrigin` provides an engine-reported origin;
opaque origins remain unavailable. Current-document URLs and page-supplied
claims are never substituted for native sender metadata. If an operation needs
origin or main-frame proof, require that proof on the individual event.

Per-field text is limited to 1 MiB. There are at most 16 windows per context.
Provider queues allow 256 events and 8 MiB including retained snapshots and
metadata; framework snapshot copies have their own bounded storage. Retaining
arbitrary copied Matrix events is application memory, outside the provider queue.
Browser subprocess memory and page execution are not bounded by these limits.

The native library remains loaded for the process lifetime, including after
context destruction. Context/profile resources are isolated from personal
browser profiles; browser subprocesses can finish asynchronously after window
closure. On Windows, active and retiring profiles together are limited to 16.
`poll` and window creation reclaim completed profiles; context destruction
performs a bounded STA drain of pending completions and browser-resource release
before deleting private profile directories. The deadline is 35 seconds for the
whole context. If cleanup cannot complete, a diagnostic is written and residual
directories remain as failure evidence. Qualification checks those directories
after the consumer exits. The explicit provider path selects trusted native
code, not sandboxed plugin code.
