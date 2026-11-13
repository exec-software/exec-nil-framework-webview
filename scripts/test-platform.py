#!/usr/bin/env python3
"""Bounded Matrix WebView routes against explicit framework and provider binaries.

Only evidence/ is uploadable. work/ retains generated executables, bytecode and
owned provider copies. The HTTP fixture is an in-process thread, never a daemon
or reusable development server. Browser descendants belong to each route's
Windows Job or POSIX process group through process_support.run.
"""
from __future__ import annotations

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import stat
import sys
import threading
import time
from urllib.parse import urlsplit
import uuid

from process_support import require, run

REPO = Path(__file__).resolve().parents[1]
DESKTOP_CASES = (
    "configuration", "ready-show", "http-document", "message-immediate",
    "message-deferred", "message-provenance", "html-json-unicode-depth",
    "undefined-null", "script-exception", "script-serialization", "script-async-result",
    "operation-busy", "redirect-explicit-follow", "http-error-document",
    "cross-window-routing", "close-stale-copied-events",
)
WHOLE_SECONDS = 600
MAX_HTTP_REQUESTS = 256


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_pin(root: Path) -> dict[str, str]:
    return {path.relative_to(root).as_posix(): digest(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def source_pin() -> dict[str, str]:
    paths = list((REPO / "src").rglob("*.matrix"))
    paths += list((REPO / "tests/matrix").rglob("*.matrix"))
    paths += [path for path in (REPO / "native").rglob("*")
              if path.is_file() and path.suffix in (".cpp", ".mm", ".h", ".hpp")]
    paths += [Path(__file__).resolve(), REPO / "scripts/process_support.py"]
    return {path.relative_to(REPO).as_posix(): digest(path) for path in sorted(set(paths))}


def normalized_machine() -> str:
    return {"amd64": "x86_64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())


class FixtureHTTPServer(ThreadingHTTPServer):
    daemon_threads = False
    block_on_close = True
    request_queue_size = 16

    def __init__(self, owner):
        self.owner = owner
        self.slots = threading.BoundedSemaphore(16)
        super().__init__(("127.0.0.1", 0), FixtureHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.owner.record_error("HTTP fixture connection limit exceeded")
            self.shutdown_request(request)
            return
        with self.owner.lock:
            self.owner.connections.add(request)
        try:
            super().process_request(request, client_address)
        except BaseException:
            with self.owner.lock:
                self.owner.connections.discard(request)
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        with self.owner.lock:
            self.owner.active_handlers += 1
        try:
            super().process_request_thread(request, client_address)
        finally:
            with self.owner.lock:
                self.owner.active_handlers -= 1
                self.owner.connections.discard(request)
            self.slots.release()

    def handle_error(self, request, client_address):
        if not self.owner.closing:
            self.owner.record_error("HTTP fixture handler failed")


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def setup(self):
        self.request.settimeout(2)
        super().setup()

    def log_message(self, *_):
        pass

    def do_GET(self):
        owner = self.server.owner
        path = urlsplit(self.path).path
        with owner.lock:
            prefix = next((value for value in owner.prefixes if path.startswith(value + "/")), None)
            if len(owner.requests) >= MAX_HTTP_REQUESTS:
                owner.errors.append("HTTP fixture request limit exceeded")
                self.close_connection = True
                return
        suffix = path[len(prefix):] if prefix else ""
        status, title, location = 200, "owned-http-page", None
        if suffix == "/page":
            content = "<div id=marker>http-ready-λ</div>"
        elif suffix == "/redirect":
            status, title, content = 302, "owned-redirect", "redirect requires explicit follow"
            location = owner.origin + prefix + "/follow"
        elif suffix == "/follow":
            title, content = "explicit-follow", "followed by application"
        else:
            status, title, content = 404, "owned-not-found", "owned missing document"
        body = ("<!doctype html><meta charset=utf-8><title>" + title + "</title>" + content).encode("utf-8")
        with owner.lock:
            owner.requests.append({"path": path, "status": status})
        try:
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            if location:
                self.send_header("Location", location)
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            # Cancelling an authorized redirect may close its response early.
            pass
        finally:
            self.close_connection = True


class HTTPFixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.prefixes: set[str] = set()
        self.requests: list[dict] = []
        self.errors: list[str] = []
        self.active_handlers = 0
        self.connections: set[socket.socket] = set()
        self.closing = False
        self.server = FixtureHTTPServer(self)
        self.origin = "http://127.0.0.1:" + str(self.server.server_address[1])
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.05),
                                       name="nilwebview-owned-http", daemon=False)
        try:
            self.thread.start()
        except BaseException:
            self.server.server_close()
            raise
        self.closed = False

    def record_error(self, message: str):
        with self.lock:
            if len(self.errors) < 16:
                self.errors.append(message)

    def route(self, label: str) -> tuple[str, str]:
        prefix = "/" + uuid.uuid4().hex + "/" + label
        with self.lock:
            self.prefixes.add(prefix)
        return self.origin + prefix, prefix

    def verify_route(self, prefix: str) -> dict:
        with self.lock:
            require(not self.errors, "owned HTTP fixture failed")
            counts = {suffix: sum(row["path"] == prefix + suffix for row in self.requests)
                      for suffix in ("/page", "/redirect", "/follow", "/missing")}
            statuses = {suffix: [row["status"] for row in self.requests if row["path"] == prefix + suffix]
                        for suffix in counts}
        require(counts == {"/page": 1, "/redirect": 1, "/follow": 1, "/missing": 1},
                "HTTP request inventory differs, or a redirect followed without authorization")
        require(statuses == {"/page": [200], "/redirect": [302], "/follow": [200], "/missing": [404]},
                "HTTP response status inventory differs")
        return {"requests": counts, "statuses": statuses}

    def close(self) -> dict:
        if not self.closed:
            with self.lock:
                self.closing = True
            self.server.shutdown()
            self.thread.join(timeout=5)
            # Close only sockets accepted by this fixture, so a client slowly
            # sending headers cannot extend teardown by resetting read timeouts.
            with self.lock:
                connections = list(self.connections)
            for connection in connections:
                try: connection.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                connection.close()
            self.server.server_close()
            self.closed = True
        with self.lock:
            clean = not self.thread.is_alive() and self.active_handlers == 0 and not self.connections
            report = {"cleanup_verified": clean, "requests": list(self.requests), "errors": list(self.errors)}
        return report


def verify_output(text: str, profile: str, system: str | None = None) -> dict:
    lines = text.splitlines()
    if profile == "synthetic":
        require(lines == ["webview-matrix-contract-ok"], "synthetic route sentinel differs or includes a skip")
        return {"sentinels": 1}
    cases = [line.removeprefix("webview-desktop-case:") for line in lines if line.startswith("webview-desktop-case:")]
    require(tuple(cases) == DESKTOP_CASES, "desktop exact case inventory differs or includes a skip")
    orders = [line for line in lines if line.startswith("webview-desktop-order:")]
    require(len(orders) == 2 and
            re.fullmatch(r"webview-desktop-order:immediate:(?:message-first|completion-first)", orders[0]) and
            re.fullmatch(r"webview-desktop-order:deferred:(?:message-first|completion-first)", orders[1]),
            "desktop paired-event order inventory differs")
    runtimes = [line.removeprefix("webview-desktop-runtime:") for line in lines
                if line.startswith("webview-desktop-runtime:")]
    if (system or platform.system()) == "Windows":
        require(len(runtimes) == 3 and all(len(value) <= 136 and
                re.fullmatch(r"WebView2 [0-9]+(?:\.[0-9]+){3}(?: (?:beta|dev|canary))?", value)
                for value in runtimes), "actual WebView2 environment versions are missing or malformed")
    else:
        require(not runtimes, "unexpected WebView2 environment version on this host")
    require(lines[-1:] == ["webview-desktop-contract-ok"] and len(lines) == len(DESKTOP_CASES) + 3 + len(runtimes),
            "desktop route emitted unexpected output or omitted completion")
    return {"checks": len(cases), "cases": cases, "observed_event_order": orders,
            "browser_runtime_versions": runtimes}


def route_environment(env: dict[str, str], work: Path, label: str,
                      system: str | None = None) -> tuple[dict[str, str], Path | None]:
    """Give every Windows consumer its own directory for GetTempPathW.

    Compiler and runtime share this route's owned directory. This keeps profile
    evidence attributable even when a route fails; no existing profile or user
    temporary directory is scanned, cleaned, or reused.
    """
    if (system or platform.system()) != "Windows":
        return dict(env), None
    temporary = work / (label + " temp λ")
    temporary.mkdir(exist_ok=False)
    result = {key: value for key, value in env.items()
              if key.upper() not in ("TEMP", "TMP", "TMPDIR")}
    result.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    return result, temporary


def profile_cleanup(temporary: Path) -> dict:
    """Inspect direct names only after owned processes have finished cleanup.

    Never descend into a profile, junction or link, and never remove anything:
    a leaked profile is failure evidence, not something the harness can repair.
    """
    report = {"path": str(temporary), "cleanup_verified": False,
              "residual_profiles": [], "entry_count": 0}
    try:
        mode = temporary.lstat()
        require(stat.S_ISDIR(mode.st_mode) and not stat.S_ISLNK(mode.st_mode)
                and not getattr(mode, "st_file_attributes", 0) & 0x400,
                "owned temporary root is missing, replaced or a reparse point")
        with os.scandir(temporary) as entries:
            for entry in entries:
                report["entry_count"] += 1
                require(report["entry_count"] <= 4096, "owned temporary directory exceeds entry bound")
                if not entry.name.casefold().startswith("nilwebview-"):
                    continue
                info = entry.stat(follow_symlinks=False)
                kind = ("link-or-reparse" if stat.S_ISLNK(info.st_mode) or
                        getattr(info, "st_file_attributes", 0) & 0x400 else
                        "directory" if stat.S_ISDIR(info.st_mode) else
                        "file" if stat.S_ISREG(info.st_mode) else "special")
                report["residual_profiles"].append({"name": entry.name, "kind": kind})
        report["residual_profiles"].sort(key=lambda value: value["name"])
        report["cleanup_verified"] = not report["residual_profiles"]
    except (OSError, AssertionError) as error:
        report["error"] = str(error)
    return report


def exercise_routes(*, compiler: Path, interpreter: Path, std: Path, profile: str,
                    provider: Path, llvm: bool, work: Path, evidence: Path,
                    env: dict[str, str], deadline: float, server: HTTPFixture | None,
                    rows: list[dict] | None = None) -> list[dict]:
    source = REPO / "tests/matrix" / ("provider_contract.matrix" if profile == "synthetic" else "desktop_contract.matrix")
    common = [str(compiler), "--quiet", "--sdk-path", str(std), "-I", str(REPO / "src")]
    if rows is None:
        rows = []

    def command(label, arguments, limit, route_env):
        remaining = int(deadline - time.monotonic())
        require(remaining > 0, "whole WebView acceptance deadline exhausted")
        return run(arguments, cwd=work, env=route_env, evidence=evidence, label=label, timeout=min(limit, remaining))

    for route in ("text", "binary", "aot", "jit") if llvm else ("text", "binary"):
        label = work.name + "-" + profile + "-" + route
        row = {"route": route, "profile": profile, "status": "FAIL", "commands": []}
        rows.append(row)
        route_env, temporary = route_environment(env, work, label)
        try:
            arguments = [str(provider)]
            prefix = None
            if profile == "desktop":
                require(server is not None, "desktop route needs its owned HTTP fixture")
                base, prefix = server.route(label)
                arguments += [base, server.origin, platform.system()]
            if route in ("text", "binary"):
                target = work / (profile + "-" + route + ".nil")
                flags = ["--emit-binary-nil"] if route == "binary" else []
                row["commands"].append(command(label + "-compile", common + ["--no-bundle", *flags, "-o", str(target), str(source)], 120, route_env))
                row["commands"].append(command(label, [str(interpreter), "--no-ai", str(target), "--", *arguments], 180, route_env))
                row["consumer_sha256"] = digest(target)
            elif route == "aot":
                target = work / (profile + "-native" + (".exe" if os.name == "nt" else ""))
                row["commands"].append(command(label + "-compile", common + ["--native", "-o", str(target), str(source)], 180, route_env))
                row["commands"].append(command(label, [str(target), *arguments], 180, route_env))
                row["consumer_sha256"] = digest(target)
            else:
                target = work / (profile + "-jit" + (".exe" if os.name == "nt" else ""))
                row["commands"].append(command(label, common + ["--native", "--jit", "-o", str(target), str(source), "--", *arguments], 240, route_env))
        finally:
            if temporary is not None:
                cleanup = profile_cleanup(temporary)
                row["private_profiles"] = cleanup
                (evidence / (label + ".profile-cleanup.json")).write_text(
                    json.dumps(cleanup, indent=2) + "\n", encoding="utf-8")
        if temporary is not None:
            require(row["private_profiles"]["cleanup_verified"],
                    label + " left a private WebView profile; see preserved cleanup evidence")
        row.update(verify_output((evidence / (label + ".stdout")).read_text(encoding="utf-8"), profile))
        if profile == "desktop":
            row["http"] = server.verify_route(prefix)
        row["cleanup_verified"] = all(item["cleanup_verified"] for item in row["commands"])
        row["status"] = "PASS"
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("compiler", "interpreter", "provider", "fixture-provider"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--nilstd", type=Path, action="append", required=True)
    parser.add_argument("--llvm", choices=("ON", "OFF"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-system", choices=("Linux", "Windows", "Darwin"))
    parser.add_argument("--expected-machine", choices=("x86_64", "arm64"))
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.output == REPO or REPO in args.output.parents:
        parser.error("--output must be outside the source checkout")
    args.output.mkdir(parents=True, exist_ok=False)
    work, evidence = args.output / "work", args.output / "evidence"
    work.mkdir(); evidence.mkdir()
    report = {"schema": "nilwebview.platform.v1", "status": "FAIL", "system": platform.system(),
              "machine": normalized_machine(), "llvm": args.llvm, "whole_seconds": WHOLE_SECONDS,
              "scope": "source Matrix consumers of a copied native provider; no installed SDK qualification",
              "expected_routes_per_profile": ["text", "binary", "aot", "jit"] if args.llvm == "ON" else ["text", "binary"],
              "expected_desktop_cases": list(DESKTOP_CASES), "roots": []}
    sources, tools, standard = {}, {}, {}
    server = None
    copied = {}
    try:
        require(not args.expected_system or args.expected_system == platform.system(), "unexpected runner system")
        require(not args.expected_machine or args.expected_machine == normalized_machine(), "unexpected runner architecture")
        for name in ("compiler", "interpreter", "provider", "fixture_provider"):
            setattr(args, name, getattr(args, name).resolve(strict=True))
        roots = [value.resolve(strict=True) for value in args.nilstd]
        require(len(set(roots)) == len(roots), "duplicate standard-library roots")
        sources = source_pin()
        tools = {str(value): digest(value) for value in (args.compiler, args.interpreter, args.provider, args.fixture_provider)}
        standard = {str(root): tree_pin(root) for root in roots}
        for key, original, folder in (("provider", args.provider, "provider space λ"),
                                      ("fixture", args.fixture_provider, "fixture provider space λ")):
            destination = work / folder / original.name
            destination.parent.mkdir()
            shutil.copy2(original, destination)
            require(digest(destination) == digest(original), "provider copy identity differs")
            copied[key] = destination
        report.update(source_sha256=sources, tools_sha256=tools, nilstd_before=standard,
                      copied_providers={key: {"path": str(value), "sha256": digest(value)} for key, value in copied.items()})
        env = {key: value for key, value in os.environ.items() if not key.startswith(("NIL_", "NY_", "NILWEB_", "NILWEBVIEW_"))}
        env.update(PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1", TMPDIR=str(work), TMP=str(work), TEMP=str(work))
        server = HTTPFixture()
        deadline = time.monotonic() + WHOLE_SECONDS
        for number, root in enumerate(roots):
            route_work = work / ("root-" + str(number)); route_work.mkdir()
            row = {"nilstd": str(root)}; report["roots"].append(row)
            for profile, provider in (("synthetic", copied["fixture"]), ("desktop", copied["provider"])):
                row[profile] = []
                exercise_routes(compiler=args.compiler, interpreter=args.interpreter, std=root,
                    profile=profile, provider=provider, llvm=args.llvm == "ON", work=route_work, evidence=evidence,
                    env=env, deadline=deadline, server=server if profile == "desktop" else None, rows=row[profile])
        report["status"] = "PASS"
    except Exception as error:
        report["error"] = str(error)
    finally:
        if server:
            try:
                report["http_fixture"] = server.close()
                if not report["http_fixture"]["cleanup_verified"] or report["http_fixture"]["errors"]:
                    report["status"] = "FAIL"
            except Exception as error:
                report["http_fixture"] = {"cleanup_verified": False, "error": type(error).__name__}
                report["status"] = "FAIL"
        seals = {
            "source_unchanged": lambda: bool(sources) and source_pin() == sources,
            "tools_unchanged": lambda: bool(tools) and all(digest(Path(path)) == value for path, value in tools.items()),
            "nilstd_unchanged": lambda: bool(standard) and all(tree_pin(Path(path)) == value for path, value in standard.items()),
            "provider_copies_unchanged": lambda: bool(copied) and all(digest(path) == report["copied_providers"][key]["sha256"] for key, path in copied.items()),
        }
        for key, check in seals.items():
            try: report[key] = check()
            except Exception: report[key] = False
        if not all(report[key] for key in seals): report["status"] = "FAIL"
        commands = [json.loads(path.read_text(encoding="utf-8")) for path in evidence.glob("*.command.json")]
        report["owned_command_count"] = len(commands)
        report["owned_commands_cleanup_verified"] = all(row.get("cleanup_verified") is True for row in commands)
        if not report["owned_commands_cleanup_verified"]: report["status"] = "FAIL"
        (evidence / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        files = {path.relative_to(evidence).as_posix(): {"bytes": path.stat().st_size, "sha256": digest(path)}
                 for path in sorted(evidence.rglob("*")) if path.is_file()}
        (evidence / "manifest.json").write_text(json.dumps({"schema": "nilwebview.platform-evidence.v1", "files": files}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(evidence / "report.json"), "error": report.get("error")}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
