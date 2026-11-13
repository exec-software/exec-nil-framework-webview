#!/usr/bin/env bash
# Ephemeral hosted-runner setup only. Keep WebKit's bubblewrap sandbox enabled.
# Ubuntu permits user namespaces through per-program AppArmor policy:
# https://documentation.ubuntu.com/security/security-features/privilege-restriction/apparmor/
# https://manpages.ubuntu.com/manpages/noble/man8/apparmor_parser.8.html
set -euo pipefail
exec python3 -I - "$@" <<'PY'
import fnmatch
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import uuid


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def captured(command, prefix, seconds=20):
    # Every command is owned, bounded, and recorded without dumping environment.
    with prefix.with_suffix('.stdout').open('xb') as stdout, prefix.with_suffix('.stderr').open('xb') as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            code = process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            raise RuntimeError(prefix.name + ' timed out')
    require(code == 0, prefix.name + ' failed; see preserved diagnostics')
    return prefix.with_suffix('.stdout').read_text(encoding='utf-8')


def snapshot(evidence, label):
    # Kernel-provided names and attachments, not guessed /etc profile filenames.
    helper = r'''
import json
from pathlib import Path
root = Path('/sys/kernel/security/apparmor/policy/profiles')
if not root.is_dir(): raise RuntimeError('AppArmor profile inventory is unavailable')
rows = []
for item in sorted(root.iterdir()):
    if len(rows) >= 4096: raise RuntimeError('AppArmor inventory exceeds bound')
    row = {}
    for key in ('name', 'attach', 'mode'):
        value = (item/key).read_text()
        if len(value) > 65536: raise RuntimeError('AppArmor metadata exceeds bound')
        row[key] = value.strip()
    rows.append(row)
print(json.dumps(rows))
'''
    return json.loads(captured(['sudo', '-n', sys.executable, '-I', '-c', helper], evidence/label))


def profile_text(name, executable):
    return ('abi <abi/4.0>,\n'
            f'profile {name} "{executable}" flags=(unconfined) {{\n'
            '  userns,\n'
            '}\n')


def owned_profile_matches(row, metadata):
    # AppArmor can expose an attachment as <unknown> when only its compiled
    # matcher is exported (apparmorfs.c: seq_profile_attach_show). For our own
    # fresh --add profile, trust the verified definition and unique name; the
    # real namespace probe below verifies that bubblewrap can create its userns.
    # This exception never applies to the pre-existing profile conflict check.
    return (row['name'] == metadata['name'] and row['mode'] == 'unconfined' and
            row['attach'] in (metadata['executable'], '<unknown>'))


def expanded_patterns(pattern):
    # AppArmor supports brace alternatives. Conservative matching also treats
    # * as spanning /, so an ambiguous existing attachment is refused.
    pending = [pattern]
    result = []
    while pending:
        value = pending.pop()
        require(len(pending) + len(result) < 128, 'AppArmor attachment pattern exceeds bound')
        match = re.search(r'\{([^{}]+)\}', value)
        if match:
            pending.extend(value[:match.start()] + item + value[match.end():]
                           for item in match.group(1).split(','))
        else:
            result.append(value)
    return result


def conflicts(rows, executable):
    alternatives = {str(executable)}
    if str(executable).startswith('/usr/'):
        alternatives.add(str(executable)[4:])  # merged-/usr path spelling
    for row in rows:
        attachment = row['attach']
        if 'bwrap' in row['name'].lower() and not attachment.startswith('/'):
            return True  # do not guess an opaque existing attachment
        if any(fnmatch.fnmatchcase(path, pattern) for path in alternatives
               for pattern in expanded_patterns(attachment)):
            return True
    return False


def webkit_bubblewrap(evidence):
    # WebKit may use a private libexec copy. Inspect its installed runtime ELF
    # for the compiled BWRAP_EXECUTABLE, never substitute command-v bwrap.
    listing = captured(['dpkg-query', '-L', 'libwebkitgtk-6.0-4'], evidence/'webkit-package-files')
    libraries = {Path(line).resolve(strict=True) for line in listing.splitlines()
                 if re.fullmatch(r'/usr/lib/[A-Za-z0-9_./+-]*/libwebkitgtk-6\.0\.so\.[0-9.]+', line)}
    require(len(libraries) == 1, 'Expected one installed WebKitGTK6 runtime ELF')
    library = libraries.pop()
    require(library.stat().st_uid == 0 and 0 < library.stat().st_size < 512*1024*1024,
            'WebKit runtime ELF ownership or size differs')
    with library.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        compiled = {match.group(1).decode('ascii') for match in re.finditer(
            rb'(?<![A-Za-z0-9_./+-])(/usr/(?:bin|lib|libexec)/[A-Za-z0-9_./+-]*bwrap)\x00', data)}
    require(len(compiled) == 1, 'Cannot identify exactly one compiled WebKit bubblewrap path')
    compiled_path = compiled.pop()
    executable = Path(compiled_path).resolve(strict=True)
    require(re.fullmatch(r'/usr/(?:bin|lib|libexec)/[A-Za-z0-9_./+-]*bwrap', str(executable)) is not None and
            executable.is_file() and executable.stat().st_uid == 0 and not executable.stat().st_mode & 0o022,
            'Use the root-owned installed WebKit bubblewrap executable')
    owner = captured(['dpkg-query', '-S', compiled_path], evidence/'bubblewrap-package-owner').strip()
    require(owner, 'WebKit bubblewrap executable has no package owner')
    write_json(evidence/'bubblewrap-discovery.json', {'library': str(library), 'compiled_path': compiled_path,
                                                   'executable': str(executable), 'package_owner': owner})
    return executable


def owned_state(directory, temporary):
    require(directory.parent == temporary and directory.name.startswith('nilwebview-apparmor-'),
            'AppArmor state must be this run\'s private temporary directory')
    require(directory.is_dir() and not directory.is_symlink() and directory.stat().st_uid == os.getuid(),
            'AppArmor state ownership differs')
    metadata_path = directory/'state.json'
    profile = directory/'profile'
    require(not metadata_path.is_symlink() and not profile.is_symlink(), 'AppArmor state must not contain symlinks')
    metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
    require(metadata['run_id'] == os.environ['GITHUB_RUN_ID'] and metadata['run_attempt'] == os.environ['GITHUB_RUN_ATTEMPT'],
            'AppArmor state belongs to another run')
    require(re.fullmatch(r'nilwebview-ci-[0-9]+-[0-9]+-[0-9a-f]{32}', metadata['name']) is not None,
            'Invalid owned AppArmor profile name')
    require(re.fullmatch(r'/usr/(?:bin|lib|libexec)/[A-Za-z0-9_./+-]*bwrap', metadata['executable']) is not None,
            'Invalid owned bubblewrap executable')
    expected = profile_text(metadata['name'], metadata['executable'])
    require(profile.read_text(encoding='utf-8') == expected and
            hashlib.sha256(expected.encode()).hexdigest() == metadata['profile_sha256'],
            'Owned AppArmor profile bytes changed')
    return metadata, profile


def cleanup(directory, temporary, evidence, parser, label='cleanup'):
    metadata, profile = owned_state(directory, temporary)
    rows = snapshot(evidence, label + '-before')
    matches = [row for row in rows if row['name'] == metadata['name']]
    require(len(matches) <= 1, 'Owned AppArmor profile is ambiguous')
    if matches:
        require(owned_profile_matches(matches[0], metadata),
                'Loaded AppArmor profile no longer matches the owned definition')
        captured(['sudo', '-n', str(parser), '--remove', '--skip-cache', str(profile)], evidence/(label + '-remove'))
    require(not any(row['name'] == metadata['name'] for row in snapshot(evidence, label + '-after')),
            'Owned AppArmor profile remains loaded')
    write_json(evidence/(label + '.json'), {'status': 'PASS', 'name': metadata['name'], 'removed': bool(matches)})


def main():
    require(len(sys.argv) == 2 and sys.argv[1] in ('setup', 'cleanup'), 'usage: ci-linux-browser.sh setup|cleanup')
    require(os.environ.get('GITHUB_ACTIONS') == 'true' and os.environ.get('RUNNER_OS') == 'Linux' and
            os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted', 'Only Linux GitHub-hosted runners are supported')
    require(os.getuid() != 0 and os.geteuid() == os.getuid(), 'Run as the ordinary hosted runner, never as root')
    for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT'):
        require(re.fullmatch(r'[0-9]+', os.environ.get(key, '')) is not None, 'Missing GitHub run identity')
    temporary_input = Path(os.environ['RUNNER_TEMP'])
    temporary = temporary_input.resolve(strict=True)
    require(temporary_input.is_absolute() and temporary.is_dir() and temporary != Path('/') and
            '\n' not in str(temporary) and '\r' not in str(temporary), 'Invalid runner temporary directory')
    evidence = temporary/'webview-ci/evidence/linux-browser'
    parser = Path('/usr/sbin/apparmor_parser').resolve(strict=True)
    require(parser.is_file() and parser.stat().st_uid == 0 and not parser.stat().st_mode & 0o022,
            'A root-owned system AppArmor parser is required')
    if sys.argv[1] == 'cleanup':
        location = os.environ.get('NILWEBVIEW_APPARMOR_STATE')
        if not location:
            print('No owned AppArmor setup was recorded')
            return
        evidence.mkdir(parents=True, exist_ok=True)
        cleanup(Path(location), temporary, evidence, parser)
        print('Removed only the owned temporary WebView AppArmor profile')
        return
    require(not os.environ.get('NILWEBVIEW_APPARMOR_STATE'), 'AppArmor setup is already recorded')
    evidence.mkdir(parents=True, exist_ok=False)
    directory = None
    attempted = False
    try:
        executable = webkit_bubblewrap(evidence)
        rows = snapshot(evidence, 'setup-before')
        require(not conflicts(rows, executable), 'An existing bubblewrap AppArmor attachment is present; it will not be replaced')
        name = 'nilwebview-ci-' + os.environ['GITHUB_RUN_ID'] + '-' + os.environ['GITHUB_RUN_ATTEMPT'] + '-' + uuid.uuid4().hex
        directory = Path(tempfile.mkdtemp(prefix='nilwebview-apparmor-', dir=temporary))
        profile = directory/'profile'
        text = profile_text(name, executable)
        with profile.open('x', encoding='utf-8') as stream:
            stream.write(text)
        metadata = {'run_id': os.environ['GITHUB_RUN_ID'], 'run_attempt': os.environ['GITHUB_RUN_ATTEMPT'],
                    'name': name, 'executable': str(executable), 'profile_sha256': hashlib.sha256(text.encode()).hexdigest()}
        write_json(directory/'state.json', metadata)
        shutil.copyfile(profile, evidence/'profile')
        write_json(evidence/'identity.json', metadata)
        # Record cleanup ownership before loading; the workflow's always() step
        # can then remove this exact profile even after an interrupted setup.
        environment_file = Path(os.environ['GITHUB_ENV'])
        require(environment_file.is_file() and not environment_file.is_symlink(), 'GitHub environment command file is required')
        with environment_file.open('a', encoding='utf-8') as stream:
            stream.write('NILWEBVIEW_APPARMOR_STATE=' + str(directory) + '\n')
        attempted = True
        captured(['sudo', '-n', str(parser), '--add', '--skip-cache', str(profile)], evidence/'setup-add')
        loaded = [row for row in snapshot(evidence, 'setup-loaded') if row['name'] == name]
        require(len(loaded) == 1 and owned_profile_matches(loaded[0], metadata),
                'Owned AppArmor profile was not loaded exactly')
        probe = ('import json, os; from pathlib import Path; '
                 'print(json.dumps({"uid": os.getuid(), '
                 '"label": Path("/proc/self/attr/apparmor/current").read_text().strip()}))')
        output = captured([str(executable), '--die-with-parent', '--new-session', '--unshare-user',
                           '--uid', '0', '--gid', '0', '--unshare-pid', '--ro-bind', '/', '/',
                           '--proc', '/proc', '--dev', '/dev', '--', '/usr/bin/python3', '-I', '-c', probe],
                          evidence/'userns-probe')
        result = json.loads(output)
        require(result['uid'] == 0, 'Bubblewrap did not enter the requested private user namespace')
        # Record the child label; another existing attachment may legitimately
        # transition it when Python is executed inside the verified namespace.
        write_json(evidence/'setup.json', {'status': 'PASS', 'name': name, 'bubblewrap_userns_probe': 'PASS',
                                         'apparmor_label': result['label'], 'webkit_sandbox': 'enabled',
                                         'global_sysctls_changed': False})
        print('Verified bubblewrap user namespaces with the owned temporary AppArmor profile')
    except Exception as error:
        report = {'status': 'FAIL', 'error': str(error)}
        if attempted and directory:
            try:
                cleanup(directory, temporary, evidence, parser, 'setup-rollback')
                report['rollback'] = 'PASS'
            except Exception as cleanup_error:
                report['rollback'] = str(cleanup_error)
        write_json(evidence/'setup.json', report)
        raise


try:
    main()
except Exception as error:
    print(str(error), file=sys.stderr)
    raise SystemExit(1)
PY
