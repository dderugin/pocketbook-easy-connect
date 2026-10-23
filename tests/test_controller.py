#!/usr/bin/env python3
"""Portable config and fail-closed checks; no listening sockets required."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()

class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-test-')
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, *args):
        return subprocess.run([str(BINARY), '--root', str(self.root), *args],
                              capture_output=True, text=True, timeout=10)

    def test_diagnostics_with_defaults(self):
        result = self.run_cli('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('auth=key', result.stdout)
        self.assertIn('UID:', result.stdout)
        self.assertIn('bin/dropbear: missing', result.stdout)

    def test_invalid_config_rejected(self):
        for setting in ('ssh_port=22', 'ssh_port=65536', 'ssh_port=2222;id',
                        'auth=none', 'auth=', 'ftp_auth=none', 'bind_address=localhost',
                        'state_dir=relative', 'pasv_min=60000\npasv_max=50000',
                        'ftp_port=2222', 'ssh_port=50001', 'ftp_user=root\nunknown=1',
                        'ssh_port=2222\nssh_port=2223'):
            with self.subTest(setting=setting):
                (self.root / 'easy-connect.conf').write_text(setting+'\n')
                self.assertNotEqual(self.run_cli('doctor').returncode, 0)

    def test_paths_with_spaces_and_shell_metacharacters(self):
        (self.root / 'easy-connect.conf').write_text('state_dir=/tmp/a b;echo x\n')
        result = self.run_cli('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('State: /tmp/a b;echo x', result.stdout)

    def test_ftp_reports_current_user_mode(self):
        result = self.run_cli('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('FTP user:',result.stdout)
        if os.geteuid()!=0:
            self.assertIn('current user; starting directory only',result.stdout)

    def test_unknown_service_rejected(self):
        self.assertEqual(self.run_cli('serve', 'telnet').returncode, 2)

    def test_password_tool_never_saves_plaintext(self):
        script = Path(__file__).resolve().parents[1] / 'tools/set-password.py'
        result = subprocess.run([sys.executable, str(script), str(self.root / 'state'), '--stdin'],
                                input='a long test password\n', capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        path = self.root / 'state/password.hash'
        self.assertTrue(path.read_text().startswith('ec1$210000$'))
        self.assertNotIn('a long test password', path.read_text())
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_short_password_rejected(self):
        script = Path(__file__).resolve().parents[1] / 'tools/set-password.py'
        result = subprocess.run([sys.executable, str(script), str(self.root / 'state'), '--stdin'],
                                input='short\n', capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'state/password.hash').exists())

if __name__ == '__main__':
    unittest.main()
