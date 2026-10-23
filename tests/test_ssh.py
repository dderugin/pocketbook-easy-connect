#!/usr/bin/env python3
"""Real local OpenSSH-to-Dropbear tests. Requires built host dependencies."""
from contextlib import contextmanager
from pathlib import Path
import os
import pwd
import shutil
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve()

class SSHTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ('dropbear', 'dropbearkey'):
            if not (ROOT / 'work/stage/bin' / tool).is_file():
                raise RuntimeError('Build host Dropbear before running integration tests')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-ssh-')
        self.root = Path(self.temp.name) / 'app with spaces'
        (self.root / 'bin').mkdir(parents=True)
        (self.root / 'state').mkdir()
        for tool in ('dropbear', 'dropbearkey'):
            shutil.copy2(ROOT / 'work/stage/bin' / tool, self.root / 'bin' / tool)
        self.key = Path(self.temp.name) / 'client'
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(self.key)], check=True)
        shutil.copy2(self.key.with_suffix('.pub'), self.root / 'state/authorized_keys')
        self.password = 'integration test password'
        subprocess.run([sys.executable, str(ROOT / 'tools/set-password.py'), str(self.root / 'state'), '--stdin'],
                       input=self.password+'\n', text=True, check=True, capture_output=True)
        while True:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                self.port = sock.getsockname()[1]
            if self.port != 2121 and not 50000 <= self.port <= 50009:
                break
        self.askpass = Path(self.temp.name) / 'askpass'
        self.askpass.write_text('#!/bin/sh\nprintf "%s\\n" "$EC_TEST_PASSWORD"\n')
        self.askpass.chmod(0o700)

    def tearDown(self):
        self.temp.cleanup()

    def ssh_command(self, method='key', password=None, command='printf easy-connect-ok'):
        env = dict(os.environ, SSH_ASKPASS=str(self.askpass), SSH_ASKPASS_REQUIRE='force',
                   DISPLAY='easy-connect-test', EC_TEST_PASSWORD=password or self.password)
        args = ['ssh', '-F', '/dev/null', '-p', str(self.port), '-o', 'ConnectTimeout=3',
                '-o', 'StrictHostKeyChecking=no', '-o', f'UserKnownHostsFile={self.root}/known_hosts',
                '-o', 'IdentityAgent=none', '-o', 'IdentitiesOnly=yes', '-o', 'NumberOfPasswordPrompts=1']
        if method == 'key':
            args += ['-o', 'BatchMode=yes', '-o', 'PreferredAuthentications=publickey', '-i', str(self.key)]
        else:
            args += ['-o', 'PubkeyAuthentication=no', '-o', 'PreferredAuthentications=password']
        args += [f'{pwd.getpwuid(os.geteuid()).pw_name}@127.0.0.1', command]
        return args, env

    def ssh(self, method='key', password=None, command='printf easy-connect-ok'):
        args, env = self.ssh_command(method, password, command)
        return subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, text=True, env=env, timeout=15)

    @contextmanager
    def server(self, auth):
        (self.root / 'easy-connect.conf').write_text(f'auth={auth}\nbind_address=127.0.0.1\nssh_port={self.port}\n')
        log = tempfile.TemporaryFile(mode='w+t')
        process = subprocess.Popen([str(BINARY), '--root', str(self.root), 'serve', 'ssh'], stdout=log, stderr=log)
        try:
            deadline = time.monotonic()+8
            while time.monotonic()<deadline:
                log.seek(0)
                if 'ssh listening on' in log.read():
                    break
                if process.poll() is not None:
                    log.seek(0)
                    self.fail('Server exited: '+log.read())
                time.sleep(0.05)
            else:
                self.fail('Server startup timed out')
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=10)
            log.seek(0)
            output = log.read()
            log.close()
            runtime = output.split('log ',1)[-1].split('/ssh.log',1)[0]
            if runtime.startswith('/tmp/easy-connect-'):
                self.assertFalse((Path(runtime) / 'password.hash').exists())
                self.assertFalse((Path(runtime) / 'host_ed25519').exists())

    def assert_ssh_ok(self, *args, **kwargs):
        result = self.ssh(*args, **kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'easy-connect-ok')

    def test_key_only_and_password_rejection(self):
        with self.server('key'):
            self.assert_ssh_ok()
            self.assertNotEqual(self.ssh('password').returncode, 0)

    def test_password_only_and_key_rejection(self):
        with self.server('password'):
            self.assert_ssh_ok('password')
            self.assertNotEqual(self.ssh().returncode, 0)
            self.assertNotEqual(self.ssh('password', password='a different password').returncode, 0)

    def test_either_authentication(self):
        with self.server('both'):
            self.assert_ssh_ok()
            self.assert_ssh_ok('password')

    def test_key_revocation_after_restart(self):
        with self.server('key'):
            self.assert_ssh_ok()
            replacement = Path(str(self.key) + '-replacement')
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(replacement)], check=True)
            shutil.copy2(replacement.with_suffix('.pub'), self.root / 'state/authorized_keys')
            # Running sessions use the staged key list until an explicit restart.
            self.assert_ssh_ok()
        with self.server('key'):
            self.assertNotEqual(self.ssh().returncode, 0)
            self.key = replacement
            self.assert_ssh_ok()

    def test_duplicate_start_and_port_released(self):
        with self.server('key'):
            other = subprocess.run([str(BINARY), '--root', str(self.root), 'serve', 'ssh'],
                                   capture_output=True, text=True, timeout=5)
            self.assertNotEqual(other.returncode, 0)
            self.assertIn('another Easy Connect session', other.stderr)
        with socket.socket() as sock:
            self.assertNotEqual(sock.connect_ex(('127.0.0.1', self.port)), 0)

    def test_start_with_connection_in_time_wait(self):
        # Close the server side first so Linux retains TIME_WAIT on our port.
        with socket.socket() as listener, socket.socket() as client:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(('127.0.0.1', self.port))
            listener.listen(1)
            client.settimeout(5)
            client.connect(('127.0.0.1', self.port))
            accepted, _ = listener.accept()
            accepted.close()
            self.assertEqual(client.recv(1), b'')
        with self.server('key'):
            self.assert_ssh_ok()

    def test_occupied_port_rejected(self):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(('127.0.0.1', self.port))
            listener.listen(1)
            (self.root / 'easy-connect.conf').write_text(
                f'auth=key\nbind_address=127.0.0.1\nssh_port={self.port}\n')
            result = subprocess.run([str(BINARY), '--root', str(self.root), 'serve', 'ssh'],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(f'cannot bind 127.0.0.1:{self.port}', result.stderr)

    def test_corrupt_password_never_falls_back(self):
        (self.root / 'state/password.hash').write_text('invalid verifier\n')
        with self.server('password'):
            self.assertNotEqual(self.ssh('password').returncode, 0)

    def test_private_key_material_is_not_overwritten(self):
        with self.server('key'):
            self.assert_ssh_ok()
        before = (self.root / 'state/host_ed25519').read_bytes()
        with self.server('key'):
            self.assert_ssh_ok()
        self.assertEqual(before, (self.root / 'state/host_ed25519').read_bytes())

    def test_password_reset_after_restart(self):
        with self.server('password'):
            self.assert_ssh_ok('password')
        replacement = 'a replaced long password'
        subprocess.run([sys.executable, str(ROOT / 'tools/set-password.py'), str(self.root / 'state'), '--stdin'],
                       input=replacement+'\n', text=True, check=True, capture_output=True)
        with self.server('password'):
            self.assertNotEqual(self.ssh('password').returncode, 0)
            self.assert_ssh_ok('password', password=replacement)

    def test_key_source_symlink_rejected(self):
        (self.root / 'state/authorized_keys').unlink()
        (self.root / 'state/authorized_keys').symlink_to(self.key.with_suffix('.pub'))
        result = subprocess.run([str(BINARY), '--root', str(self.root), 'serve', 'ssh'],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)

    def test_missing_and_empty_credentials_fail_before_listening(self):
        for auth, name in (('key', 'authorized_keys'), ('password', 'password.hash'),
                           ('both', 'authorized_keys'), ('both', 'password.hash')):
            path = self.root / 'state' / name
            original = path.read_bytes()
            for missing in (True, False):
                with self.subTest(auth=auth, name=name, missing=missing):
                    if missing:
                        path.unlink()
                    else:
                        path.write_bytes(b'')
                    (self.root / 'easy-connect.conf').write_text(
                        f'auth={auth}\nbind_address=127.0.0.1\nssh_port={self.port}\n')
                    result = subprocess.run([str(BINARY), '--root', str(self.root), 'serve', 'ssh'],
                                            capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn('ssh listening on', result.stdout)
                    with socket.socket() as probe:
                        self.assertNotEqual(probe.connect_ex(('127.0.0.1', self.port)), 0)
                    path.write_bytes(original)

    def test_symlinked_host_key_is_not_followed_or_overwritten(self):
        victim = Path(self.temp.name) / 'victim'
        victim.write_bytes(b'must not overwrite host key')
        (self.root / 'state/host_ed25519').symlink_to(victim)
        (self.root / 'easy-connect.conf').write_text(
            f'bind_address=127.0.0.1\nssh_port={self.port}\n')
        result = subprocess.run([str(BINARY), '--root', str(self.root), 'serve', 'ssh'],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(victim.read_bytes(), b'must not overwrite host key')

    def test_fingerprint_matches_host_key_presented_to_openssh(self):
        with self.server('key'):
            scan = subprocess.run(['ssh-keyscan', '-T', '3', '-p', str(self.port), '-t', 'ed25519', '127.0.0.1'],
                                  capture_output=True, text=True, timeout=10)
            self.assertEqual(scan.returncode, 0, scan.stderr)
            fingerprint = subprocess.run(['ssh-keygen', '-lf', '-'], input=scan.stdout,
                                        capture_output=True, text=True, timeout=5)
            self.assertEqual(fingerprint.returncode, 0, fingerprint.stderr)
            expected = fingerprint.stdout.split()[1]
            result = subprocess.run([str(BINARY), '--root', str(self.root), 'fingerprint'],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(expected, result.stdout)

    def test_unknown_account_cannot_use_valid_key(self):
        with self.server('key'):
            args, env = self.ssh_command()
            args[-2] = 'easy-connect-nonexistent-user@127.0.0.1'
            result = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, env=env, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('easy-connect-ok', result.stdout)

    def test_unicode_password_authentication(self):
        password = 'пароль для моей книги'
        subprocess.run([sys.executable, str(ROOT / 'tools/set-password.py'), str(self.root / 'state'), '--stdin'],
                       input=password + '\n', text=True, capture_output=True, check=True)
        with self.server('password'):
            self.assert_ssh_ok('password', password=password)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'subreaper cleanup requires Linux')
    def test_active_session_terminated_on_controller_stop(self):
        with self.server('key') as controller:
            args, env = self.ssh_command(command='printf "ready\\n"; exec sleep 120')
            client = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True, env=env)
            try:
                ready, _, _ = select.select([client.stdout], [], [], 10)
                self.assertTrue(ready, 'SSH command did not become ready')
                self.assertEqual(client.stdout.readline(), 'ready\n')
                controller.terminate()
                controller.wait(timeout=10)
                self.assertNotEqual(client.wait(timeout=10), 0)
            finally:
                if client.poll() is None:
                    client.terminate()
                    client.wait(timeout=5)
                client.stdout.close()
                client.stderr.close()

if __name__ == '__main__':
    unittest.main()
