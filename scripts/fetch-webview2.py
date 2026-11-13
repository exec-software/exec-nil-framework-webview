#!/usr/bin/env python3
"""Fetch and verify the pinned Microsoft WebView2 SDK into a fresh owned folder.

This does not install or update the WebView2 browser runtime or any global SDK.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile

VERSION = "1.0.4314.35"
PACKAGE = "microsoft.web.webview2"
URL = f"https://api.nuget.org/v3-flatcontainer/{PACKAGE}/{VERSION}/{PACKAGE}.{VERSION}.nupkg"
CATALOG = "https://api.nuget.org/v3/catalog0/data/2026.10.23.06.50.10/microsoft.web.webview2.1.0.4314.35.json"
PACKAGE_BYTES = 9818548
SHA512 = "cOaiVqP3BmQHMnxGpZzANM22tUXgycrisyJHjIGNzIaMsm8iXj7Kx+ceYg+RbYwJtzT009qfcFTocvVLY+ekMw=="
SHA256 = "90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4"
REQUIRED = {"build/native/include/WebView2.h", "build/native/include/WebView2EnvironmentOptions.h",
            "build/native/x64/WebView2LoaderStatic.lib", "build/native/arm64/WebView2LoaderStatic.lib",
            "LICENSE.txt", "NOTICE.txt"}


class OfficialRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        parsed = urllib.parse.urlparse(url)
        if (parsed.scheme != "https" or parsed.hostname not in {"api.nuget.org", "globalcdn.nuget.org"}
                or parsed.username is not None or parsed.password is not None):
            raise ValueError("SDK download redirected outside official HTTPS NuGet hosts")
        return super().redirect_request(request, fp, code, message, headers, url)


def download() -> bytes:
    deadline = time.monotonic() + 90
    with urllib.request.build_opener(OfficialRedirects()).open(URL, timeout=30) as response:
        blocks = []
        size = 0
        while True:
            if time.monotonic() > deadline:
                raise TimeoutError("SDK download exceeded its bounded deadline")
            block = response.read(min(1024 * 1024, PACKAGE_BYTES + 1 - size))
            if not block:
                break
            blocks.append(block)
            size += len(block)
            if size > PACKAGE_BYTES:
                raise ValueError("SDK package exceeds pinned size")
    return b"".join(blocks)


def verified_entries(data: bytes) -> tuple[zipfile.ZipFile, list[zipfile.ZipInfo]]:
    if (len(data) != PACKAGE_BYTES or hashlib.sha512(data).digest() != base64.b64decode(SHA512)
            or hashlib.sha256(data).hexdigest() != SHA256):
        raise ValueError("SDK package differs from the pinned official NuGet SHA512/size")
    archive = zipfile.ZipFile(io.BytesIO(data))
    entries = archive.infolist()
    if len(entries) > 2048 or sum(item.file_size for item in entries) > 128 * 1024 * 1024:
        raise ValueError("SDK archive exceeds extraction limits")
    names = set()
    files = set()
    for item in entries:
        path = PurePosixPath(item.filename)
        if (not item.filename or path.is_absolute() or ".." in path.parts
                or "\\" in item.filename or ":" in item.filename
                or path.as_posix() != item.filename.rstrip("/")):
            raise ValueError("SDK archive contains an invalid path")
        key = path.as_posix().casefold()
        if key in names:
            raise ValueError("SDK archive contains duplicate or case-aliased paths")
        names.add(key)
        mode = stat.S_IFMT(item.external_attr >> 16)
        if mode not in (0, stat.S_IFREG, stat.S_IFDIR) or item.flag_bits & 1:
            raise ValueError("SDK archive contains a link, special entry, or encryption")
        if not item.is_dir():
            files.add(path.as_posix())
    if not REQUIRED <= files:
        raise ValueError("SDK package is missing a required header, loader, or notice")
    return archive, entries


def publish(data: bytes, output: Path) -> dict:
    archive, entries = verified_entries(data)
    # Resolve only the parent. An existing destination (including a symlink) is
    # never followed, overwritten, merged, or deleted by this command.
    parent = output.parent.resolve(strict=True)
    output = parent / output.name
    if output.exists() or output.is_symlink():
        raise FileExistsError("SDK output must be a new directory")
    metadata = {"package": "Microsoft.Web.WebView2", "version": VERSION, "source": URL,
                "catalog": CATALOG, "package_bytes": PACKAGE_BYTES, "sha512_base64": SHA512,
                "sha256": SHA256, "files": {}}
    with archive, tempfile.TemporaryDirectory(prefix=".nilwebview-sdk-", dir=parent) as staging:
        private = Path(staging)
        for item in entries:
            path = private / item.filename
            if item.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                content = archive.read(item)
                path.write_bytes(content)
                metadata["files"][item.filename] = {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}
        # Fresh destination and copyfile preserve destination ACL inheritance;
        # never rename a protected TemporaryDirectory into a published SDK.
        output.mkdir()
        try:
            for name in metadata["files"]:
                target = output / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(private / name, target)
                if hashlib.sha256(target.read_bytes()).hexdigest() != metadata["files"][name]["sha256"]:
                    raise ValueError("SDK file changed during publication")
            (output / "nilwebview-sdk.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        except BaseException:
            shutil.rmtree(output)
            raise
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="new SDK directory; its parent must exist")
    args = parser.parse_args()
    metadata = publish(download(), args.output)
    print(json.dumps({"status": "PASS", "sdk": str(args.output.resolve()), "version": VERSION,
                      "files": len(metadata["files"]), "sha256": SHA256}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
