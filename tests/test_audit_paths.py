#!/usr/bin/env python3
"""Synthetic public-source audit controls; no real personal data or credentials."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "audit-paths.py"
SPEC = importlib.util.spec_from_file_location("audit_paths", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def ipv4(*parts: int) -> str:
    return ".".join(map(str, parts))


def ipv6(*parts: str) -> str:
    return ":".join(parts)


class AuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="nil-web-audit-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.file = self.root / "fixture.txt"

    def scan(self, body: str | bytes, name: str = "fixture.txt") -> list[str]:
        self.file = self.root / name
        self.file.parent.mkdir(parents=True, exist_ok=True)
        self.file.write_bytes(body.encode() if isinstance(body, str) else body)
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]):
            return AUDIT.audit(self.root, set())

    def rule(self, body: str | bytes, rule: str) -> None:
        findings = self.scan(body)
        self.assertTrue(any(item.endswith(": " + rule) for item in findings))
        for item in findings:
            self.assertEqual(len(item.splitlines()), 1)

    def test_posix_and_root_homes(self) -> None:
        for parent in ("home", "Users", "root"):
            with self.subTest(parent=parent):
                self.rule("/" + parent + "/audit-fixture/project", "home-path")

    def test_unicode_home(self) -> None:
        self.rule("/" + "home/\u00e9quipe/project", "home-path")

    def test_windows_user_paths(self) -> None:
        for directory in ("Users", "Documents"):
            for separator in ("/", "\\", "\\\\"):
                with self.subTest(directory=directory, separator=separator):
                    self.rule("C:" + separator + directory + separator + "fixture",
                              "windows-user-path")

    def test_private_and_global_ipv4(self) -> None:
        for parts in ((10, 73, 91, 17), (172, 19, 4, 8), (192, 168, 47, 8),
                      (8, 8, 4, 4)):
            self.rule(ipv4(*parts), "non-documentation-address")

    def test_compressed_ipv6_and_sentence_punctuation(self) -> None:
        for address in (ipv6("fd73", "91ab", "", "17"),
                        ipv6("2606", "4700", "", "1111")):
            for decorated in (address, "[" + address + "]:443", address + "."):
                self.rule(decorated, "non-documentation-address")

    def test_full_and_scoped_ipv6(self) -> None:
        self.rule(ipv6("fd73", "91ab", "0", "0", "0", "0", "0", "17"),
                  "non-documentation-address")
        self.rule(ipv6("fe80", "", "17") + "%fixture", "non-documentation-address")

    def test_mapped_ipv4_classification(self) -> None:
        self.rule("::ffff:" + ipv4(10, 73, 91, 17), "non-documentation-address")
        for address in ("127.0.0.1", "192.0.2.8"):
            self.assertEqual(self.scan("::ffff:" + address), [])

    def test_documentation_and_binding_addresses(self) -> None:
        for address in ("127.0.0.1", "127.23.0.9", "0.0.0.0", "255.255.255.255",
                        "192.0.2.8", "198.51.100.8", "203.0.113.8", "::", "::1",
                        "2001:db8::8", "[::1]:8080"):
            with self.subTest(address=address):
                self.assertEqual(self.scan(address), [])

    def test_public_documentation_urls_and_templates(self) -> None:
        body = "https://example.com/docs contact@example.org name@type.matrix name@file.nilhtml"
        self.assertEqual(self.scan(body), [])

    def test_nonreserved_contact(self) -> None:
        self.rule("fixture" + "@" + "audit.invalid", "contact-address")

    def test_invalid_ip_like_text(self) -> None:
        self.assertEqual(self.scan("999.888.777.666 release 1.2.3 12:34:56"), [])

    def test_ambiguous_four_part_version_is_conservatively_reported(self) -> None:
        self.rule("version " + ipv4(1, 2, 3, 4), "non-documentation-address")

    def test_diagnostics_do_not_include_matching_content(self) -> None:
        payload = "/" + "home/audit-fixture/private-name"
        findings = self.scan("ordinary line\n" + payload)
        self.assertEqual(findings, ['"fixture.txt":2: home-path'])
        self.assertNotIn(payload, "\n".join(findings))

    def test_non_utf8_fails_closed(self) -> None:
        self.rule(b"\xff synthetic bytes", "non-utf8-file")

    def test_nul_bytes_fail_closed(self) -> None:
        self.rule(b"text\x00synthetic bytes", "binary-file")

    def test_unreadable_file_fails_closed_without_exception_details(self) -> None:
        self.file.write_text("safe")
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]), \
                patch.object(Path, "read_bytes", side_effect=PermissionError("private-detail")):
            self.assertEqual(AUDIT.audit(self.root, set()),
                             ['"fixture.txt":0: unreadable-file'])

    def test_missing_tracked_file_fails_closed(self) -> None:
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]):
            self.assertEqual(AUDIT.audit(self.root, set()),
                             ['"fixture.txt":0: unreadable-file'])

    def test_directory_is_not_silently_skipped(self) -> None:
        self.file.mkdir()
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]):
            self.assertEqual(AUDIT.audit(self.root, set()),
                             ['"fixture.txt":0: non-regular-file'])

    @unittest.skipUnless(hasattr(os, "symlink"), "platform has no symlink support")
    def test_symlink_target_is_never_read(self) -> None:
        target = self.root / "target.txt"
        target.write_text("safe")
        try:
            self.file.symlink_to(target)
        except OSError:
            self.skipTest("symlink creation is unavailable for this account")
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]), \
                patch.object(Path, "read_bytes", side_effect=AssertionError("target read")):
            self.assertEqual(AUDIT.audit(self.root, set()),
                             ['"fixture.txt":0: symlink-file'])

    def test_real_git_unicode_and_control_filenames(self) -> None:
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True,
                       capture_output=True)
        names = ["unicode-\u00e9.txt", "space name.txt"]
        if os.name != "nt":
            names += ["line\nbreak.txt", "control\x1b.txt", "tab\tname.txt"]
        for name in names:
            (self.root / name).write_text("/" + "home/audit-fixture/project")
        subprocess.run(["git", "add", "--all"], cwd=self.root, check=True,
                       capture_output=True)
        self.assertEqual(set(AUDIT.tracked_files(self.root)),
                         {self.root / name for name in names})
        self.assertEqual(set(AUDIT.audit(self.root, set())),
                         {f"{json.dumps(name, ensure_ascii=True)}:1: home-path"
                          for name in names})

    def test_auditor_path_has_no_implicit_exemption(self) -> None:
        findings = self.scan("/" + "home/audit-fixture/project", "scripts/audit-paths.py")
        self.assertEqual(findings, ['"scripts/audit-paths.py":1: home-path'])
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]), \
                patch.object(sys, "argv", ["audit-paths", "--root", str(self.root)]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(AUDIT.main(), 1)

    def test_explicit_skip_is_exact(self) -> None:
        self.file.write_bytes(b"\xff")
        with patch.object(AUDIT, "tracked_files", return_value=[self.file]):
            self.assertEqual(AUDIT.audit(self.root, {"fixture.txt"}), [])
            self.assertNotEqual(AUDIT.audit(self.root, {"fixture"}), [])

    def test_git_failure_has_no_command_output(self) -> None:
        with patch.object(AUDIT, "tracked_files", side_effect=subprocess.CalledProcessError(
                1, ["git"], stderr=b"private-detail")), \
                patch.object(sys, "argv", ["audit-paths", "--root", str(self.root)]), \
                contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(AUDIT.main(), 1)
        self.assertNotIn("private-detail", output.getvalue())

    def test_audit_script_and_controls_pass_their_own_policy(self) -> None:
        paths = [SCRIPT, Path(__file__).resolve()]
        with patch.object(AUDIT, "tracked_files", return_value=paths):
            self.assertEqual(AUDIT.audit(SCRIPT.parents[1], set()), [])


if __name__ == "__main__":
    unittest.main()
