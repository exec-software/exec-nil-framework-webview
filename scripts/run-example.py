#!/usr/bin/env python3
"""Compile and launch the interactive Matrix message-console example.

Compilation is bounded and owns its process tree. The launched application has
no artificial runtime deadline: close its native window or interrupt it normally.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from process_support import run

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, help='compatible nilcompiler executable')
    parser.add_argument('--interpreter', type=Path, help='compatible nilinterpreter executable')
    parser.add_argument('--nilstd', type=Path, help='matching standard-library source directory')
    parser.add_argument('--provider', type=Path, required=True, help='absolute trusted provider library path')
    args = parser.parse_args()
    sdk_value = os.environ.get('NIL_SDK')
    sdk = Path(sdk_value).expanduser() if sdk_value else None
    extension = '.exe' if os.name == 'nt' else ''
    compiler = args.compiler or (sdk / ('nilcompiler' + extension) if sdk else None)
    interpreter = args.interpreter or (sdk / ('nilinterpreter' + extension) if sdk else None)
    standard = args.nilstd
    if standard is None and sdk is not None:
        standard = sdk / 'nilstd'
        if (standard / 'src').is_dir():
            standard /= 'src'
    if compiler is None or interpreter is None or standard is None:
        parser.error('provide --compiler, --interpreter and --nilstd, or set NIL_SDK')
    if not args.provider.is_absolute():
        parser.error('--provider must be an explicit absolute trusted library path')
    try:
        compiler = compiler.expanduser().resolve(strict=True)
        interpreter = interpreter.expanduser().resolve(strict=True)
        standard = standard.expanduser().resolve(strict=True)
        provider = args.provider.resolve(strict=True)
    except OSError as error:
        parser.error(str(error))
    if not compiler.is_file() or not interpreter.is_file() or not provider.is_file() or not standard.is_dir():
        parser.error('tools/provider must be regular files and --nilstd must be a directory')
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='message-console-', dir=build))
    source = ROOT / 'examples/message-console/src/main.matrix'
    html = ROOT / 'examples/message-console/assets/index.html'
    artifact = output / 'message-console.nil'
    environment = os.environ.copy()
    command = [str(compiler), '--quiet', '--sdk-path', str(standard), '-I', str(ROOT / 'src'),
               '--no-bundle', '-o', str(artifact), str(source)]
    print('Building the message console...', flush=True)
    try:
        run(command, cwd=ROOT, env=environment, evidence=output, label='compile', timeout=120)
    except Exception as error:
        print(f'Compilation failed: {error}. Evidence: {output}', file=sys.stderr)
        return 1
    print(f'Build evidence: {output}', flush=True)
    print('Close the native window to exit. Page messages appear below.', flush=True)
    try:
        return subprocess.call([str(interpreter), '--no-ai', str(artifact), '--',
            '--provider', str(provider), '--html', str(html)], cwd=ROOT, env=environment)
    except KeyboardInterrupt:
        return 130
    except OSError as error:
        print(f'Could not launch the example: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
