#!/usr/bin/env python3
"""Test real C filesystem/credential code through a host-only driver."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class CoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-core-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / 'state'
        self.state.mkdir()
        self.password = b'a separate test password'

    def run_core(self, command, *args, password=b''):
        return subprocess.run([str(BINARY), command, *map(str, args)], input=password,
                              capture_output=True, timeout=10)

    def write_password(self, service='ssh', password=None):
        result = self.run_core('set-password', self.state, service,
                               password=self.password if password is None else password)
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.state / ('password.hash' if service == 'ssh' else 'ftp-password.hash')

    def assert_rejected(self, command, *args, password=b''):
        result = self.run_core(command, *args, password=password)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_error_message_matches_stderr(self):
        result = self.run_core('error', 'saving settings')
        self.assertEqual(result.stdout, b'operation saving settings failed: 42\n')
        self.assertEqual(result.stderr, b'easy-connect: ' + result.stdout)

    def test_private_directory_creation_and_reuse(self):
        path = self.root / 'runtime'
        for _ in range(2):
            self.assertEqual(self.run_core('private-dir', path).returncode, 0)
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)

    def test_private_directory_rejects_public_permissions(self):
        self.state.chmod(0o755)
        self.assert_rejected('private-dir', self.state)
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o755)

    def test_private_directory_rejects_symlink_and_regular_file(self):
        link = self.root / 'link'
        link.symlink_to(self.state, target_is_directory=True)
        file = self.root / 'file'
        file.touch()
        for path in (link, file):
            with self.subTest(path=path):
                self.assert_rejected('private-dir', path)

    def test_private_copy_handles_binary_and_limit_sized_files(self):
        source, target = self.root / 'source', self.root / 'target'
        for size in (0, 8193, 1048576):
            with self.subTest(size=size):
                payload = os.urandom(size)
                source.write_bytes(payload)
                self.assertEqual(self.run_core('copy', source, target, 0).returncode, 0)
                self.assertEqual(target.read_bytes(), payload)
                self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_private_copy_rejects_empty_required_file(self):
        source, target = self.root / 'empty', self.root / 'target'
        source.touch()
        self.assert_rejected('copy', source, target, 1)
        self.assertFalse(target.exists())

    def test_private_copy_rejects_missing_oversized_and_special_sources(self):
        oversized, fifo = self.root / 'oversized', self.root / 'fifo'
        oversized.write_bytes(b'x' * 1048577)
        os.mkfifo(fifo)
        for source in (self.root / 'missing', oversized, fifo, self.state):
            with self.subTest(source=source):
                target = self.root / 'target'
                self.assert_rejected('copy', source, target, 1)
                self.assertFalse(target.exists())

    def test_private_copy_rejects_symlinks_without_changing_victim(self):
        source, victim = self.root / 'source', self.root / 'victim'
        source.write_bytes(b'secret')
        victim.write_bytes(b'unchanged')
        link = self.root / 'link'
        link.symlink_to(victim)
        self.assert_rejected('copy', link, self.root / 'target', 1)
        self.assert_rejected('copy', source, link, 1)
        self.assertEqual(victim.read_bytes(), b'unchanged')

    def test_password_roundtrip_matches_independent_pbkdf2(self):
        path = self.write_password()
        version, rounds, salt, digest = path.read_text().strip().split('$')
        self.assertEqual(version, 'ec1')
        self.assertEqual(int(rounds), 210000)
        self.assertEqual(len(bytes.fromhex(salt)), 16)
        expected = hashlib.pbkdf2_hmac('sha256', self.password, bytes.fromhex(salt), int(rounds))
        self.assertEqual(bytes.fromhex(digest), expected)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.run_core('verify', path, password=self.password).returncode, 0)
        self.assert_rejected('verify', path, password=b'incorrect password')

    def test_password_length_boundaries_and_utf8(self):
        for password in (b'a' * 12, b'b' * 1024, 'пароль для устройства'.encode()):
            with self.subTest(length=len(password)):
                path = self.write_password(password=password)
                self.assertEqual(self.run_core('verify', path, password=password).returncode, 0)

    def test_invalid_password_keeps_existing_verifier(self):
        path = self.write_password()
        before = path.read_bytes()
        for password in (b'', b'x' * 11, b'x' * 1025, b'line break\npassword', b'carriage\rreturn password'):
            with self.subTest(password=password):
                self.assert_rejected('set-password', self.state, 'ssh', password=password)
                self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(self.state.glob('.password.*')), [])

    def test_password_salts_and_service_separation(self):
        ssh = self.write_password()
        before = ssh.read_bytes()
        ftp = self.write_password('ftp')
        self.assertNotEqual(before, ftp.read_bytes())
        self.assertEqual(ssh.read_bytes(), before)
        self.write_password()
        self.assertNotEqual(ssh.read_bytes(), before)

    def test_unknown_password_service_cannot_create_file(self):
        self.assert_rejected('set-password', self.state, 'telnet', password=self.password)
        self.assert_rejected('has-password', self.state, 'telnet')
        self.assertEqual(list(self.state.iterdir()), [])

    def test_password_state_symlink_is_rejected(self):
        link = self.root / 'link'
        link.symlink_to(self.state, target_is_directory=True)
        self.assert_rejected('set-password', link, 'ssh', password=self.password)
        self.assertEqual(list(self.state.iterdir()), [])

    def test_password_atomic_replace_does_not_follow_target_symlink(self):
        victim = self.root / 'victim'
        victim.write_bytes(b'unchanged')
        path = self.state / 'password.hash'
        path.symlink_to(victim)
        self.write_password()
        self.assertFalse(path.is_symlink())
        self.assertEqual(victim.read_bytes(), b'unchanged')

    def test_password_rename_failure_cleans_temporary_file(self):
        target = self.state / 'password.hash'
        target.mkdir()
        (target / 'keep').write_text('unchanged')
        self.assert_rejected('set-password', self.state, 'ssh', password=self.password)
        self.assertEqual((target / 'keep').read_text(), 'unchanged')
        self.assertEqual(list(self.state.glob('.password.*')), [])

    def test_password_presence_excludes_missing_empty_and_symlink(self):
        path = self.state / 'password.hash'
        self.assert_rejected('has-password', self.state, 'ssh')
        path.touch()
        self.assert_rejected('has-password', self.state, 'ssh')
        self.write_password()
        self.assertEqual(self.run_core('has-password', self.state, 'ssh').returncode, 0)
        self.assert_rejected('has-password', self.state, 'ftp')
        path.unlink()
        path.symlink_to(self.write_password('ftp'))
        self.assert_rejected('has-password', self.state, 'ssh')

    def test_verifier_rejects_insecure_permissions_symlink_and_special_files(self):
        path = self.write_password()
        path.chmod(0o644)
        self.assert_rejected('verify', path, password=self.password)
        path.chmod(0o600)
        link = self.root / 'link'
        link.symlink_to(path)
        fifo = self.root / 'fifo'
        os.mkfifo(fifo)
        for source in (link, fifo, self.state, self.root / 'missing', Path('relative.hash')):
            with self.subTest(source=source):
                self.assert_rejected('verify', source, password=self.password)

    def test_verifier_rejects_malformed_records(self):
        path = self.write_password()
        valid = path.read_bytes()
        _, rounds, salt, digest = valid.strip().split(b'$')
        records = [b'', b'invalid', valid + b'extra', valid + b'\0hidden', b'x' * 160,
                   b'ec2$' + b'$'.join((rounds, salt, digest)),
                   b'ec1$209999$' + salt + b'$' + digest,
                   b'ec1$1000001$' + salt + b'$' + digest,
                   b'ec1$210000$' + salt[:-1] + b'$' + digest,
                   b'ec1$210000$' + b'z' * 32 + b'$' + digest,
                   b'ec1$210000$' + salt + b'$' + digest[:-1],
                   b'ec1$210000$' + salt + b'$' + b'g' * 64]
        for index, record in enumerate(records):
            with self.subTest(record=index):
                path.write_bytes(record)
                self.assert_rejected('verify', path, password=self.password)

    def test_verifier_rejects_empty_and_oversized_passwords(self):
        path = self.write_password()
        for password in (b'', b'x' * 1025):
            self.assert_rejected('verify', path, password=password)

    def test_key_count_ignores_comments_blanks_and_long_line_fragments(self):
        path = self.state / 'authorized_keys'
        path.write_text('# comment\n \t\n ssh-ed25519 first\n\t# comment\n' + 'x' * 20000 + '\nssh-rsa last')
        self.assertEqual(self.run_core('key-count', self.state).stdout, b'3\n')

    def test_key_count_rejects_symlinks_special_and_oversized_files(self):
        path = self.state / 'authorized_keys'
        self.assertEqual(self.run_core('key-count', self.state).stdout, b'0\n')
        victim = self.root / 'victim'
        victim.write_text('ssh-ed25519 key\n')
        path.symlink_to(victim)
        self.assertEqual(self.run_core('key-count', self.state).stdout, b'0\n')
        path.unlink()
        os.mkfifo(path)
        self.assertEqual(self.run_core('key-count', self.state).stdout, b'0\n')
        path.unlink()
        path.write_bytes(b'x' * 1048577)
        self.assertEqual(self.run_core('key-count', self.state).stdout, b'0\n')

    def test_config_roundtrip_preserves_all_values_and_private_mode(self):
        settings = dict(auth='both', ftp_auth='anonymous', bind_address='127.0.0.1',
                        ssh_port='2022', ftp_port='2021', pasv_min='51000', pasv_max='51001',
                        ftp_user='current', ftp_root='/tmp/shared books', state_dir=str(self.state))
        path = self.root / 'easy-connect.conf'
        path.write_text(''.join(f'{k}={v}\n' for k, v in settings.items()))
        result = self.run_core('config-save', self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        import pwd
        settings['ftp_user'] = pwd.getpwuid(os.geteuid()).pw_name
        saved = dict(line.split('=', 1) for line in path.read_text().splitlines() if not line.startswith('#'))
        self.assertEqual(saved, settings)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(self.root.glob('.easy-connect.conf.*')), [])

    def test_key_info_drains_large_child_output_and_propagates_failure(self):
        (self.root / 'bin').mkdir()
        tool = self.root / 'bin/dropbearkey'
        tool.write_text('#!/bin/sh\nprintf "SHA256:test\\n"\nhead -c 131072 /dev/zero\n')
        tool.chmod(0o755)
        result = self.run_core('key-info', self.root, 8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'SHA256:')
        tool.write_text('#!/bin/sh\nexit 7\n')
        self.assert_rejected('key-info', self.root, 8)


if __name__ == '__main__':
    unittest.main()
