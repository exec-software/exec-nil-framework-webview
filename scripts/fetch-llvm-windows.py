#!/usr/bin/env python3
"""Fetch the pinned complete LLVM 18 Windows development archive into CI storage.

The compiler-only installer lacks headers, libraries and LLVMConfig.cmake.
This command neither installs a toolchain globally nor changes PATH.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request

VERSION = '18.1.8'
PREFIX = 'clang+llvm-18.1.8-x86_64-pc-windows-msvc'
URL = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang%2Bllvm-18.1.8-x86_64-pc-windows-msvc.tar.xz'
PACKAGE_BYTES = 981666720
SHA256 = '22c5907db053026cc2a8ff96d21c0f642a90d24d66c23c6d28ee7b1d572b82e8'
UNPACKED_BYTES = 4772738139
FILE_COUNT = 4509
DIRECTORY_COUNT = 258


class OfficialRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        parsed = urllib.parse.urlparse(url)
        if (parsed.scheme != 'https' or parsed.hostname not in {
                'github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com'}
                or parsed.username is not None or parsed.password is not None):
            raise ValueError('LLVM archive redirected outside official HTTPS GitHub hosts')
        return super().redirect_request(request, fp, code, message, headers, url)


def download(path: Path) -> None:
    deadline = time.monotonic() + 900
    size = 0
    digest = hashlib.sha256()
    with urllib.request.build_opener(OfficialRedirects()).open(URL, timeout=30) as response, path.open('xb') as output:
        while True:
            if time.monotonic() > deadline:
                raise TimeoutError('LLVM download exceeded its deadline')
            block = response.read(min(1024 * 1024, PACKAGE_BYTES + 1 - size))
            if not block:
                break
            size += len(block)
            if size > PACKAGE_BYTES:
                raise ValueError('LLVM archive exceeds pinned size')
            digest.update(block)
            output.write(block)
    if size != PACKAGE_BYTES or digest.hexdigest() != SHA256:
        raise ValueError('LLVM archive differs from the pinned official size/SHA256')


def unpack(archive_path: Path, output: Path) -> dict:
    # Caller has already verified the entire compressed archive. Extract into a
    # fresh normally inherited directory, never publish a private staging DACL.
    output.mkdir(exist_ok=False)
    files = directories = size = 0
    names = set()
    deadline = time.monotonic() + 600
    try:
        with tarfile.open(archive_path, 'r|xz') as archive:
            for entry in archive:
                if time.monotonic() > deadline:
                    raise TimeoutError('LLVM extraction exceeded its deadline')
                path = PurePosixPath(entry.name)
                if (not path.parts or path.parts[0] != PREFIX or path.is_absolute() or '..' in path.parts
                        or '\\' in entry.name or ':' in entry.name or path.as_posix() != entry.name.rstrip('/')):
                    raise ValueError('LLVM archive contains an unexpected path')
                key = path.as_posix().casefold()
                if key in names:
                    raise ValueError('LLVM archive contains duplicate or case-aliased paths')
                names.add(key)
                target = output.joinpath(*path.parts[1:])
                if entry.isdir():
                    directories += 1
                    target.mkdir(parents=True, exist_ok=True)
                elif entry.isfile():
                    files += 1
                    size += entry.size
                    if size > UNPACKED_BYTES or files > FILE_COUNT or entry.size > 2 * 1024**3:
                        raise ValueError('LLVM extraction exceeds pinned inventory bounds')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = archive.extractfile(entry)
                    if source is None:
                        raise ValueError('LLVM regular file is unavailable')
                    with source, target.open('xb') as destination:
                        shutil.copyfileobj(source, destination, 1024 * 1024)
                    if target.stat().st_size != entry.size:
                        raise ValueError('LLVM extracted file size differs')
                else:
                    raise ValueError('LLVM archive contains a link or special file')
                if directories > DIRECTORY_COUNT:
                    raise ValueError('LLVM directory inventory exceeds pin')
        if (files, directories, size) != (FILE_COUNT, DIRECTORY_COUNT, UNPACKED_BYTES):
            raise ValueError('LLVM complete archive inventory differs')
        for relative in ('lib/cmake/llvm/LLVMConfig.cmake', 'include/llvm/IR/LLVMContext.h',
                         'lib/LLVMCore.lib', 'bin/llvm-config.exe', 'bin/clang.exe'):
            if not (output / relative).is_file():
                raise ValueError('LLVM development archive is missing required content: ' + relative)
        metadata = {'version': VERSION, 'source': URL, 'package_bytes': PACKAGE_BYTES,
                    'sha256': SHA256, 'files': files, 'directories': directories, 'unpacked_bytes': size}
        (output / 'nilwebview-llvm.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        return metadata
    except BaseException:
        shutil.rmtree(output)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='new external toolchain directory')
    args = parser.parse_args()
    output = args.output.parent.resolve(strict=True) / args.output.name
    repo = Path(__file__).resolve().parents[1]
    if output == repo or repo in output.parents or output.exists() or output.is_symlink():
        parser.error('LLVM output must be a fresh directory outside the source checkout')
    with tempfile.TemporaryDirectory(prefix='.nilwebview-llvm-', dir=output.parent) as temporary:
        archive = Path(temporary) / 'llvm.tar.xz'
        download(archive)
        metadata = unpack(archive, output)
    print(json.dumps({'status': 'PASS', 'llvm': str(output), **metadata}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
