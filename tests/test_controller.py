#!/usr/bin/env python3
"""Portable config and fail-closed checks; no listening sockets required."""
from pathlib import Path
import os
import hashlib
import pwd
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

    def test_invalid_command_shapes_have_usage_exit_code(self):
        for arguments in ((), ('unknown',), ('serve',), ('serve', 'ssh', 'extra'),
                          ('doctor', 'extra'), ('fingerprint', 'extra')):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.run_cli(*arguments).returncode, 2)

    def test_missing_application_root_is_reported(self):
        result = subprocess.run([str(BINARY), '--root', str(self.root / 'missing'), 'doctor'],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('root ', result.stderr)

    def test_comments_whitespace_crlf_and_final_line_without_newline(self):
        path = self.root / 'easy-connect.conf'
        path.write_bytes(b'  # comment\r\n\r\n auth = both \r\nftp_auth = anonymous\r\nssh_port=2233')
        result = self.run_cli('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(':2233 auth=both', result.stdout)
        self.assertIn('FTP access: anonymous', result.stdout)

    def test_port_boundaries_and_single_passive_port_are_valid(self):
        (self.root / 'easy-connect.conf').write_text(
            'ssh_port=1024\nftp_port=65535\npasv_min=60000\npasv_max=60000\n')
        result = self.run_cli('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(':1024 auth=key', result.stdout)
        self.assertIn(':65535 passive=60000-60000', result.stdout)

    def test_each_setting_rejects_duplicates(self):
        settings = dict(ssh_port='2233', ftp_port='2133', pasv_min='51000', pasv_max='51001',
                        auth='both', ftp_auth='anonymous', bind_address='127.0.0.1',
                        state_dir='/tmp/state', ftp_root='/tmp/books', ftp_user='current')
        for key, value in settings.items():
            with self.subTest(key=key):
                (self.root / 'easy-connect.conf').write_text(f'{key}={value}\n{key}={value}\n')
                result = self.run_cli('doctor')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('duplicate config setting on line 2', result.stderr)

    def test_malformed_config_never_starts_services(self):
        invalid = ('not-an-assignment', '=value', 'unknown=value', 'ssh_port=+2223',
                   'ssh_port=-1', 'ssh_port=0', 'ssh_port=1e4', 'ssh_port=1234.0',
                   'ssh_port=999999999999999999999999', 'ftp_port=', 'pasv_min=1023',
                   'bind_address=::1', 'bind_address=256.0.0.1', 'bind_address=127.1',
                   'ftp_root=relative', 'ftp_root=', 'ftp_user=', 'ftp_user=user name',
                   'ftp_user=user;id', 'state_dir=/tmp/a\rb', 'auth=KEY',
                   'ftp_root=/' + 'x' * 5000)
        for setting in invalid:
            with self.subTest(setting=setting[:80]):
                (self.root / 'easy-connect.conf').write_text(setting + '\n')
                result = self.run_cli('serve', 'both')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('config setting', result.stderr)
                self.assertFalse((self.root / 'state').exists())

    def test_all_port_overlaps_are_rejected(self):
        for setting in ('ssh_port=2121', 'ftp_port=2222', 'ssh_port=50000',
                        'ssh_port=50009', 'ftp_port=50000', 'ftp_port=50009',
                        'pasv_min=2222\npasv_max=2222', 'pasv_min=51000\npasv_max=50000'):
            with self.subTest(setting=setting):
                (self.root / 'easy-connect.conf').write_text(setting + '\n')
                result = self.run_cli('doctor')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('must not overlap', result.stderr)

    def test_current_user_and_explicit_account_diagnostics(self):
        user = pwd.getpwuid(os.geteuid()).pw_name
        for setting in ('current', user):
            (self.root / 'easy-connect.conf').write_text(f'ftp_user={setting}\n')
            result = self.run_cli('doctor')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('FTP user: ' + user, result.stdout)

    def test_diagnostics_never_dump_verifiers_or_private_keys(self):
        state = self.root / 'state'
        state.mkdir()
        for name in ('password.hash', 'ftp-password.hash', 'host_ed25519', 'authorized_keys'):
            (state / name).write_text('secret marker must stay private')
        result = self.run_cli('doctor')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('secret marker', result.stdout + result.stderr)

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

    def test_password_tool_boundaries_utf8_and_service_separation(self):
        script = Path(__file__).resolve().parents[1] / 'tools/set-password.py'
        state = self.root / 'state'
        for service, password in (('ssh', 'x' * 12), ('ftp', 'я' * 512)):
            result = subprocess.run([sys.executable, str(script), str(state), '--service', service, '--stdin'],
                                    input=password + '\n', text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            path = state / ('password.hash' if service == 'ssh' else 'ftp-password.hash')
            _, rounds, salt, digest = path.read_text().strip().split('$')
            self.assertEqual(hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), int(rounds)).hex(), digest)
            self.assertNotIn(password, result.stdout + result.stderr)
        self.assertNotEqual((state / 'password.hash').read_bytes(), (state / 'ftp-password.hash').read_bytes())

    def test_password_tool_rejection_preserves_existing_verifier(self):
        script = Path(__file__).resolve().parents[1] / 'tools/set-password.py'
        state = self.root / 'state'
        state.mkdir()
        path = state / 'password.hash'
        path.write_text('previous verifier')
        for password in ('', 'x' * 11, 'x' * 1025, 'я' * 513, 'nul\0inside a password', 'carriage\rreturn password'):
            with self.subTest(length=len(password)):
                result = subprocess.run([sys.executable, str(script), str(state), '--stdin'],
                                        input=password + '\n', text=True, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(path.read_text(), 'previous verifier')
                self.assertEqual(list(state.glob('.password-*')), [])

if __name__ == '__main__':
    unittest.main()
