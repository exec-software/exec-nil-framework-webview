# Reporting security issues

Use [private vulnerability reporting](https://github.com/exec-software/exec-nil-framework-webview/security/advisories/new)
when it is available. Otherwise, open a minimal issue requesting a private
reporting channel without including exploit details, credentials or private
data. Ordinary bugs can be reported publicly. There is no guaranteed
response-time SLA.

Include the WebView and NIL Framework revisions, platform and architecture,
browser runtime version, execution route, impact, and a small reproduction
using synthetic data. Remove credentials, profile contents, personal paths,
private hosts and user content from logs. Test only systems you own or are
authorized to test, with bounded activity that does not affect other users.

## Current boundaries

The [platform support document](docs/platform-support.md) records what has
actually been qualified. Initial browser qualification is pending; shared
controls and synthetic-provider passes do not establish production readiness.

Provider configuration loads trusted native code from an explicit absolute
path. ABI checks are compatibility checks, not a sandbox for a hostile library.
Provider libraries remain pinned for process lifetime. Do not load a provider
selected by a page or from a directory an untrusted party can replace.

Page messages are untrusted data. The library exposes native provenance only
when the provider has it. `SourceKind.Unknown`, an absent origin, or unknown
frame identity cannot satisfy an application policy that requires that proof.
`SourceKind.NativeURL` does not establish an authenticated origin. Do not
substitute the page's current URL or a JSON field claiming an origin.

Redirects and uninitiated navigation require application approval through a
new explicit navigation. Keep destination policy in the application and do not
turn page messages directly into shell commands, filesystem operations or
script source. Resource bounds and asynchronous cancellation do not guarantee
that a script has stopped executing or that a browser subprocess has already
exited. The system browser engine remains responsible for its own sandbox and
security updates.
