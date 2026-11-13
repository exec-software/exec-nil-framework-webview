#!/usr/bin/env python3
"""Owned Windows profile cleanup controls without a browser or desktop."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location('webview_platform', SCRIPTS / 'test-platform.py')
assert SPEC and SPEC.loader
DRIVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRIVER)


class ProfileControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='nilwebview-harness-control-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / 'work'; self.work.mkdir()
        self.evidence = self.root / 'evidence'; self.evidence.mkdir()

    def test_windows_environment_is_private_per_route_and_case_insensitive(self):
        initial = {'Temp': 'old', 'tMp': 'old', 'TMPDIR': 'old', 'KEEP': 'retained'}
        first, a = DRIVER.route_environment(initial, self.work, 'first', 'Windows')
        second, b = DRIVER.route_environment(initial, self.work, 'second', 'Windows')
        self.assertNotEqual(a, b)
        self.assertTrue(a.is_dir() and b.is_dir())
        self.assertEqual(first, {'TEMP': str(a), 'TMP': str(a), 'TMPDIR': str(a), 'KEEP': 'retained'})
        self.assertEqual(second['TEMP'], str(b))
        self.assertEqual(initial['Temp'], 'old')
        with self.assertRaises(FileExistsError):
            DRIVER.route_environment(initial, self.work, 'first', 'Windows')

    def test_other_platforms_do_not_change_temporary_environment(self):
        initial = {'TEMP': 'owned-parent', 'DISPLAY': 'display-fixture'}
        result, temporary = DRIVER.route_environment(initial, self.work, 'first', 'Linux')
        self.assertEqual(result, initial)
        self.assertIsNot(result, initial)
        self.assertIsNone(temporary)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_empty_directory_is_clean(self):
        self.assertTrue(DRIVER.profile_cleanup(self.work)['cleanup_verified'])

    def test_unrelated_compiler_temp_is_preserved(self):
        item = self.work / 'compiler-object'; item.write_bytes(b'fixture')
        report = DRIVER.profile_cleanup(self.work)
        self.assertTrue(report['cleanup_verified'])
        self.assertEqual(report['entry_count'], 1)
        self.assertEqual(item.read_bytes(), b'fixture')

    def test_profile_directory_and_file_are_retained_and_fail(self):
        directory = self.work / 'nilwebview-owned-fixture'; directory.mkdir()
        (directory / 'evidence').write_bytes(b'preserve')
        file = self.work / 'NILWEBVIEW-file'; file.write_bytes(b'preserve')
        report = DRIVER.profile_cleanup(self.work)
        self.assertFalse(report['cleanup_verified'])
        self.assertEqual({item['kind'] for item in report['residual_profiles']}, {'file', 'directory'})
        self.assertEqual((directory / 'evidence').read_bytes(), b'preserve')
        self.assertTrue(file.exists())

    def test_profile_link_is_not_followed(self):
        target = self.root / 'unrelated'; target.mkdir()
        sentinel = target / 'sentinel'; sentinel.write_bytes(b'untouched')
        link = self.work / 'nilwebview-link'
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError as error:
            self.skipTest('host does not permit synthetic directory links: ' + str(error))
        report = DRIVER.profile_cleanup(self.work)
        self.assertFalse(report['cleanup_verified'])
        self.assertEqual(report['residual_profiles'], [{'name': link.name, 'kind': 'link-or-reparse'}])
        self.assertEqual(sentinel.read_bytes(), b'untouched')

    def test_missing_root_is_not_cleanup_proof(self):
        report = DRIVER.profile_cleanup(self.work / 'missing')
        self.assertFalse(report['cleanup_verified'])
        self.assertIn('error', report)

    def test_route_completion_checks_every_private_temp(self):
        calls = []
        def command(arguments, *, cwd, env, evidence, label, timeout):
            calls.append((label, env['TEMP']))
            if label.endswith('-compile'):
                Path(arguments[arguments.index('-o') + 1]).write_bytes(b'owned artifact')
            else:
                (evidence / (label + '.stdout')).write_text('webview-matrix-contract-ok\n', encoding='utf-8')
            return {'cleanup_verified': True, 'exit_code': 0}
        with patch.object(DRIVER.platform, 'system', return_value='Windows'), patch.object(DRIVER, 'run', side_effect=command):
            rows = DRIVER.exercise_routes(compiler=self.root / 'compiler', interpreter=self.root / 'interpreter',
                std=self.root / 'std', profile='synthetic', provider=self.root / 'provider', llvm=False,
                work=self.work, evidence=self.evidence, env={'TEMP': 'old'},
                deadline=time.monotonic() + 20, server=None)
        self.assertEqual([row['status'] for row in rows], ['PASS', 'PASS'])
        self.assertEqual(len({temporary for _, temporary in calls}), 2)
        self.assertEqual(calls[0][1], calls[1][1])
        self.assertEqual(calls[2][1], calls[3][1])
        self.assertTrue(all(row['private_profiles']['cleanup_verified'] for row in rows))
        self.assertEqual(len(list(self.evidence.glob('*.profile-cleanup.json'))), 2)

    def failure_case(self, command_fails):
        rows = []
        def command(arguments, *, cwd, env, evidence, label, timeout):
            if label.endswith('-compile'):
                Path(arguments[arguments.index('-o') + 1]).write_bytes(b'owned artifact')
            else:
                leaked = Path(env['TEMP']) / 'nilwebview-preserved'; leaked.mkdir()
                (leaked / 'evidence').write_bytes(b'not deleted')
                if command_fails:
                    raise AssertionError('owned command failed after cleanup attempt')
                (evidence / (label + '.stdout')).write_text('webview-matrix-contract-ok\n', encoding='utf-8')
            return {'cleanup_verified': True, 'exit_code': 0}
        with patch.object(DRIVER.platform, 'system', return_value='Windows'), patch.object(DRIVER, 'run', side_effect=command):
            with self.assertRaises(AssertionError):
                DRIVER.exercise_routes(compiler=self.root / 'compiler', interpreter=self.root / 'interpreter',
                    std=self.root / 'std', profile='synthetic', provider=self.root / 'provider', llvm=False,
                    work=self.work, evidence=self.evidence, env={}, deadline=time.monotonic() + 20,
                    server=None, rows=rows)
        self.assertEqual(rows[0]['status'], 'FAIL')
        cleanup = rows[0]['private_profiles']
        self.assertFalse(cleanup['cleanup_verified'])
        self.assertEqual((Path(cleanup['path']) / 'nilwebview-preserved/evidence').read_bytes(), b'not deleted')
        report = next(self.evidence.glob('*.profile-cleanup.json'))
        self.assertEqual(json.loads(report.read_text(encoding='utf-8')), cleanup)

    def test_successful_process_with_leaked_profile_fails(self):
        self.failure_case(False)

    def test_failed_process_still_records_leaked_profile(self):
        self.failure_case(True)


if __name__ == '__main__':
    unittest.main()
