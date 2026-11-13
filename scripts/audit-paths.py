#!/usr/bin/env python3
"""Refuse absolute paths, routable addresses and contact details in the source.

A framework's source has no business naming a developer's home directory, a
machine on anyone's network, or a person. None of that is a secret in the sense
gitleaks looks for, which is exactly why it needs its own check. A personal
home-directory path leaks a name and a working habit; a scanner built for API
keys will not necessarily detect it.

What is allowed, and why, because a check nobody can satisfy gets deleted:

  * Loopback — 127.0.0.0/8 and ::1. A server binding loopback is the
    documented default and names no machine but the one running it.
  * The unspecified address — 0.0.0.0 and ::. Means "every interface", which
    is a configuration choice, not a location.
  * The documentation ranges of RFC 5737 and RFC 3849 — 192.0.2.0/24,
    198.51.100.0/24, 203.0.113.0/24, 2001:db8::/32. Reserved precisely so that
    examples can use them.
  * Numeric strings that do not parse as IP addresses. Four-part version
    numbers that also form valid addresses are deliberately reported: context
    alone cannot establish that an address is harmless.

Every tracked file must be a readable, regular UTF-8 text file without NUL
bytes. Symlinks and binary assets require an explicit policy before adding
them; silently skipping a file would leave part of the public source unaudited.

Everything else is reported with its file and line, and the line itself is
never printed: if a finding is real, printing it puts the thing being
complained about into a build log that may be more public than the file was.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path

# A home directory with a name after it. `/home/` alone is not interesting --
# it appears in prose about Linux -- so a following name is required.
HOME_PATH = re.compile(r"/(?:home|Users)/[\w.-]+|/(?:root)(?:/|\b)")
WINDOWS_PATH = re.compile(r"[A-Za-z]:[\\/]+(?:Users|Documents)(?:[\\/]|\b)", re.IGNORECASE)

# Deliberately loose: the point is to catch a real address, and the parse below
# decides whether a match is one.
IPV4 = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
# Include compressed and IPv4-mapped forms; ipaddress validates candidates.
IPV6 = re.compile(r"(?<![0-9A-Za-z_:])[0-9A-Fa-f:.]*:[0-9A-Fa-f:.]+(?![0-9A-Za-z_:])")

# An address this project may name in its own source.
DOCUMENTATION_NETWORKS = [
    ipaddress.ip_network("127.0.0.0/8"),      # loopback
    ipaddress.ip_network("0.0.0.0/32"),       # every interface
    ipaddress.ip_network("255.255.255.255/32"),
    ipaddress.ip_network("192.0.2.0/24"),     # RFC 5737
    ipaddress.ip_network("198.51.100.0/24"),  # RFC 5737
    ipaddress.ip_network("203.0.113.0/24"),   # RFC 5737
    ipaddress.ip_network("::1/128"),          # loopback
    ipaddress.ip_network("::/128"),           # every interface
    ipaddress.ip_network("2001:db8::/32"),    # RFC 3849
]

EMAIL = re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")

# example.com and friends are reserved for documentation by RFC 2606, and a
# .matrix or .nilhtml suffix in a template is not a top-level domain.
ALLOWED_EMAIL_DOMAINS = ("example.com", "example.org", "example.net")


def tracked_files(root: Path) -> list[Path]:
    listed = subprocess.run(["git", "ls-files", "--cached", "-z"], cwd=root,
                            capture_output=True, check=True)
    return [root / os.fsdecode(name) for name in listed.stdout.split(b"\0")
            if name]


def is_reportable_address(candidate: str) -> bool:
    try:
        address = ipaddress.ip_address(candidate.rstrip("."))
    except ValueError:
        # Not an address at all -- a version number, a byte sequence, a time.
        return False
    # Mapped loopback/documentation IPv4 addresses retain their classification.
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        address = address.ipv4_mapped
    return not any(address in network for network in DOCUMENTATION_NETWORKS)


def is_reportable_email(candidate: str) -> bool:
    domain = candidate.rsplit("@", 1)[-1].lower()
    if domain in ALLOWED_EMAIL_DOMAINS:
        return False
    # A bare suffix that is not a real TLD: "name@type.matrix" in prose.
    return not domain.endswith((".matrix", ".nilhtml", ".ny", ".nil"))


def audit(root: Path, skip: set[str]) -> list[str]:
    findings: list[str] = []
    for path in tracked_files(root):
        relative = path.relative_to(root).as_posix()
        if relative in skip:
            continue

        def report(number: int, rule: str) -> None:
            # Filenames can themselves contain newlines or terminal controls.
            # JSON escaping keeps each diagnostic on one line, without content.
            findings.append(f"{json.dumps(relative, ensure_ascii=True)}:{number}: {rule}")

        try:
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                report(0, "symlink-file")
                continue
            if not stat.S_ISREG(mode):
                report(0, "non-regular-file")
                continue
            raw = path.read_bytes()
        except OSError:
            report(0, "unreadable-file")
            continue
        if b"\0" in raw:
            report(0, "binary-file")
            continue
        try:
            body = raw.decode("utf-8")
        except UnicodeDecodeError:
            report(0, "non-utf8-file")
            continue
        for number, line in enumerate(body.splitlines(), start=1):
            if HOME_PATH.search(line):
                report(number, "home-path")
            if WINDOWS_PATH.search(line):
                report(number, "windows-user-path")
            if any(is_reportable_address(found)
                   for found in IPV4.findall(line) + IPV6.findall(line)):
                report(number, "non-documentation-address")
            if any(is_reportable_email(found) for found in EMAIL.findall(line)):
                report(number, "contact-address")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--skip", action="append", default=[],
                        help="a tracked path to leave out, repeatable")
    args = parser.parse_args()

    try:
        root = args.root.resolve(strict=True)
        findings = audit(root, set(args.skip))
    except (OSError, RuntimeError, subprocess.CalledProcessError):
        print("Source audit failed: repository files could not be enumerated.",
              file=sys.stderr)
        return 1
    if findings:
        print("Source audit failures (the matching text is deliberately not shown):")
        for finding in sorted(set(findings)):
            print(f"  - {finding}")
        return 1
    print("Source audit: all selected tracked files are readable text; no "
          "personal paths, non-documentation addresses or contact details found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
