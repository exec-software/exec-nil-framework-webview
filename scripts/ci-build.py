#!/usr/bin/env python3
"""Build and qualify source WebView consumers; never activate or package an SDK."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

from process_support import require, run

REPO = Path(__file__).resolve().parents[1]
FRAMEWORK_TESTS = {
    'nil.webview-runtime': '57 WebView runtime checks passed',
    'nil.webview-intrinsic-contract': 'WebView intrinsic ABI: 28 checks passed',
    'nil.ui-intrinsic-contract': 'UI intrinsic ABI: 40 checks passed',
    'nil.formatted-text-contract': '216 formatted-text rejection checks passed',
    'nil.markup-contract': '392 markup contract checks passed',
}
PROVIDER_TESTS = {
    'webview.shared': '197 WebView shared checks passed',
    'webview.script-transport': '36 WebView script checks passed',
}


def git(root: Path, *arguments: str) -> str:
    return subprocess.check_output(['git', '-C', str(root), *arguments], text=True, timeout=20).strip()


def save(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def machine() -> str:
    value = platform.machine().lower()
    return {'amd64': 'x86_64', 'aarch64': 'arm64'}.get(value, value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', required=True, choices=('configure', 'framework-build', 'provider-build', 'contracts', 'desktop'))
    parser.add_argument('--framework', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--llvm-dir', type=Path)
    parser.add_argument('--webview2-sdk', type=Path)
    parser.add_argument('--dia-library', type=Path)
    parser.add_argument('--expected-system', required=True, choices=('Linux', 'Windows', 'Darwin'))
    parser.add_argument('--expected-machine', required=True, choices=('x86_64', 'arm64'))
    args = parser.parse_args()
    framework = args.framework.resolve(strict=True)
    output = args.output.resolve()
    require(all(output != source and source not in output.parents for source in (REPO, framework)),
            'CI output must be outside source checkouts')
    output.mkdir(parents=True, exist_ok=True)
    evidence = output / 'evidence'; evidence.mkdir(exist_ok=True)
    framework_build, provider_build = output / 'framework-build', output / 'provider-build'
    environment = os.environ.copy()
    environment.update(PYTHONDONTWRITEBYTECODE='1', PYTHONUTF8='1')
    stage = {'phase': args.phase, 'status': 'FAIL', 'commands': []}

    def command(label: str, arguments, seconds: int):
        row = run(list(map(str, arguments)), cwd=REPO, env=environment, evidence=evidence,
                  label=label, timeout=seconds)
        stage['commands'].append(row)
        return row

    def inventory(build: Path, expected: dict[str, str], label: str) -> dict:
        selector = '^(' + '|'.join(re.escape(name) for name in expected) + ')$'
        command(label + '-inventory', ['ctest', '--test-dir', build, '-C', 'Release', '-R', selector, '--show-only=json-v1'], 30)
        data = json.loads((evidence / (label + '-inventory.stdout')).read_text(encoding='utf-8'))
        tests = data.get('tests', [])
        require(len(tests) == len(expected) and {test['name'] for test in tests} == set(expected),
                label + ' exact CTest inventory differs')
        return {test['name']: test for test in tests}

    def contracts(build: Path, expected: dict[str, str], label: str):
        inventory(build, expected, label)
        selector = '^(' + '|'.join(re.escape(name) for name in expected) + ')$'
        junit = evidence / (label + '.xml')
        command(label, ['ctest', '--test-dir', build, '-C', 'Release', '-R', selector,
                       '--parallel', '1', '--verbose', '--no-tests=error', '--output-junit', junit], 430)
        cases = ET.parse(junit).getroot().findall('.//testcase')
        require(len(cases) == len(expected) and {case.attrib['name'] for case in cases} == set(expected),
                label + ' executed test inventory differs')
        for case in cases:
            require(case.find('failure') is None and case.find('error') is None and case.find('skipped') is None,
                    label + ' contains a failed or skipped test')
            require((case.findtext('system-out') or '').strip() == expected[case.attrib['name']],
                    label + ' exact assertion count/output differs for ' + case.attrib['name'])
        return {name: expected[name] for name in expected}

    try:
        require(platform.system() == args.expected_system and machine() == args.expected_machine,
                'runner OS or architecture differs from the job')
        pin = (REPO / 'framework-revision.txt').read_text(encoding='ascii').strip()
        require(re.fullmatch('[0-9a-f]{40}', pin) is not None, 'framework revision must be a full immutable commit')
        require(git(framework, 'rev-parse', 'HEAD') == pin, 'framework source revision differs from pin')
        require(not git(framework, 'status', '--porcelain', '--untracked-files=all'), 'framework source must remain clean')
        require(not git(REPO, 'status', '--porcelain', '--untracked-files=all'), 'WebView source must remain clean')
        header_hashes = {}
        for public, bridge in (('provider_v1.h', 'webview_provider_v1.h'), ('json_v1.hpp', 'webview_json_v1.hpp')):
            value = (REPO / 'native/include/nilwebview' / public).read_bytes()
            require(value == (framework / 'common' / bridge).read_bytes(), 'framework/provider contract bytes differ: ' + public)
            header_hashes[public] = hashlib.sha256(value).hexdigest()
        identity = {'framework_revision': pin, 'webview_revision': git(REPO, 'rev-parse', 'HEAD'),
                    'workflow_revision': os.environ.get('GITHUB_WORKFLOW_SHA'),
                    'system': platform.system(), 'machine': machine(), 'platform': platform.platform(),
                    'python': sys.version, 'llvm': True, 'database_providers': False,
                    'header_sha256': header_hashes, 'scope': 'source builds and real browser consumers; no installed SDK qualification'}
        identity_path = evidence / 'identity.json'
        if identity_path.exists():
            require(json.loads(identity_path.read_text(encoding='utf-8')) == identity, 'CI identity changed between phases')
        else:
            save(identity_path, identity)
        if args.phase == 'configure':
            require(args.llvm_dir is not None and (args.llvm_dir / 'LLVMConfig.cmake').is_file(), 'LLVM development CMake package is required')
            require(not framework_build.exists() and not provider_build.exists(), 'configure requires fresh build directories')
            command('cmake-version', ['cmake', '--version'], 20)
            command('node-version', ['node', '--version'], 20)
            if platform.system() == 'Linux':
                command('browser-dependencies', ['pkg-config', '--modversion', 'gtk4', 'webkitgtk-6.0'], 20)
            elif platform.system() == 'Darwin':
                command('macos-version', ['sw_vers'], 20)
            flags = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=ON']
            if platform.system() == 'Windows':
                # CMake's compiler probes otherwise use /Zi and can retain an
                # mspdbsrv.exe helper after the probe exits. Embed debug info
                # in each object instead; try_compile inherits this policy.
                # Keep the owned-process cleanup gate unchanged.
                flags += ['-DCMAKE_POLICY_DEFAULT_CMP0141=NEW',
                          '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded']
            native = ['-DNIL_ENABLE_LLVM=ON', '-DNIL_BUILD_EXPERIMENTAL_TOOLS=OFF',
                      '-DNILCOMPILER_SQLITE_INCLUDE_DIR=OFF', '-DNILCOMPILER_POSTGRESQL_INCLUDE_DIR=OFF',
                      '-DPython3_EXECUTABLE=' + sys.executable, '-DLLVM_DIR=' + str(args.llvm_dir.resolve()),
                      '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
            if args.dia_library:
                require(args.dia_library.is_file(), 'installed VS DIA library is missing')
                native.append('-DNIL_LLVM_DIA_LIBRARY:FILEPATH=' + str(args.dia_library.resolve()))
            command('framework-configure', ['cmake', '-S', framework, '-B', framework_build, *flags, *native], 420)
            compile_commands = json.loads((framework_build / 'compile_commands.json').read_text(encoding='utf-8'))
            require(any(Path(entry['file']).name == 'llvm_backend.cpp' for entry in compile_commands), 'LLVM backend silently disabled')
            provider = []
            if platform.system() == 'Windows':
                require(args.webview2_sdk is not None and (args.webview2_sdk / 'nilwebview-sdk.json').is_file(), 'verified WebView2 SDK is required')
                provider.append('-DNILWEBVIEW_WEBVIEW2_SDK=' + str(args.webview2_sdk.resolve()))
            command('provider-configure', ['cmake', '-S', REPO, '-B', provider_build, *flags, *provider], 240)
        elif args.phase == 'framework-build':
            command('framework-build', ['cmake', '--build', framework_build, '--config', 'Release', '--parallel', '2',
                    '--target', 'nilcompiler', 'nilinterpreter', 'nil_webview_runtime_tests'], 2300)
        elif args.phase == 'provider-build':
            command('provider-build', ['cmake', '--build', provider_build, '--config', 'Release', '--parallel', '2',
                    '--target', 'nilwebview', 'nilwebview_shared_tests'], 420)
        elif args.phase == 'contracts':
            stage['framework'] = contracts(framework_build, FRAMEWORK_TESTS, 'framework-contracts')
            stage['provider'] = contracts(provider_build, PROVIDER_TESTS, 'provider-contracts')
        elif args.phase == 'desktop':
            tests = inventory(framework_build, FRAMEWORK_TESTS, 'desktop-framework')
            fixture = Path(tests['nil.webview-runtime']['command'][1])
            suffix = '.exe' if os.name == 'nt' else ''
            provider_name = 'nilwebview.dll' if os.name == 'nt' else 'libnilwebview.dylib' if platform.system() == 'Darwin' else 'libnilwebview.so'
            provider = provider_build / 'native' / provider_name
            paths = [framework_build / 'bin' / ('nilcompiler' + suffix), framework_build / 'bin' / ('nilinterpreter' + suffix), provider, fixture]
            require(all(path.is_file() for path in paths), 'actual compiler/interpreter/provider outputs are missing')
            arguments = [sys.executable, '-B', REPO / 'scripts/test-platform.py',
                '--compiler', paths[0], '--interpreter', paths[1], '--provider', provider,
                '--fixture-provider', fixture, '--nilstd', framework / 'nilstd/src', '--llvm', 'ON',
                '--expected-system', args.expected_system, '--expected-machine', args.expected_machine,
                '--output', output / 'platform']
            if platform.system() == 'Linux':
                arguments = ['dbus-run-session', '--', 'xvfb-run', '-a', '-s', '-screen 0 1280x900x24', *arguments]
            command('desktop-platform', arguments, 680)
            report = json.loads((output / 'platform/evidence/report.json').read_text(encoding='utf-8'))
            require(report['status'] == 'PASS', 'platform report did not complete')
            stage['platform_report'] = str(output / 'platform/evidence/report.json')
        require(not git(framework, 'status', '--porcelain', '--untracked-files=all'), 'framework source changed during phase')
        require(not git(REPO, 'status', '--porcelain', '--untracked-files=all'), 'WebView source changed during phase')
        stage['status'] = 'PASS'
    except Exception as error:
        stage['error'] = str(error)
    finally:
        save(evidence / (args.phase + '.json'), stage)
    print(json.dumps({'phase': args.phase, 'status': stage['status'], 'evidence': str(evidence), 'error': stage.get('error')}))
    return 0 if stage['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
