# Contributing to nil-webview

Start with the [API contract](docs/api.md), [platform evidence](docs/platform-support.md)
and [migration table](docs/migration.md). Keep public behavior in Matrix when
possible; native code should implement the platform browser boundary. Changes
to `webview.v1` also require the corresponding NIL Framework changes.

## Development checks

The shared controls need CMake, a C++17 compiler and Node.js. They can run
without a display or browser development package:

```sh
cmake -S . -B build/shared -DNILWEBVIEW_BUILD_PROVIDER=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/shared --config Release --parallel 2
ctest --test-dir build/shared -C Release --output-on-failure
```

For a provider change, also build that provider and run the affected desktop
checks on its actual OS and architecture. Report the framework revision,
provider revision, browser runtime version, execution route, assertions and
cleanup result. A shared test, synthetic provider, compiler-only pass or
interpreter pass must not be described as full browser or native-route parity.

Use synthetic pages and bounded test runs. Keep browser profiles, displays,
temporary directories and child processes owned by the test. Do not use a
personal browser profile, disable its sandbox, or install a global event loop
to make a test pass. Preserve failures when retrying a qualification run.

Change the C ABI deliberately: keep its framework copy synchronized, preserve
version and layout checks, and add negative tests for malformed provider data.
The provider and framework copies of the WebView JSON grammar must remain
identical. Provider libraries remain loaded for process lifetime; hot unloading
is outside this API.

## Public source review

Submit focused pull requests with regression coverage and documentation for
behavior changes. Let the repository's configured review and status checks
complete before merging. Do not claim a gate or platform pass that has not run.

Do not commit credentials, private configuration, internal notes, personal
paths, browser profiles, generated binaries or unreviewed logs. Review history
as well as the current tree. After staging the intended text files, run:

```sh
python3 -B tests/test_audit_paths.py
python3 -B scripts/audit-paths.py --root .
```

The path audit examines tracked files. It supplements source review and the
repository's secret scanning; passing it does not establish absence of secrets.
Keep test evidence outside tracked source and sanitize reports before sharing.

Report vulnerabilities through [SECURITY.md](SECURITY.md). Contributions to
original project materials use the [MIT License](LICENSE); preserve third-party
notices and distinguish SDK acquisition from permission to redistribute it.
