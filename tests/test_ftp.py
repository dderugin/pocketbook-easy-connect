#!/usr/bin/env python3
"""Real authenticated passive FTP, running with the test user's permissions."""
from contextlib import contextmanager
from pathlib import Path
import ftplib
import io
import os
import pwd
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve()

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

@unittest.skipIf(os.geteuid()==0, 'rootless tests must run as a non-root user')
class FTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-ftp-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'app with spaces'
        self.share = Path(self.temp.name) / 'shared books'
        self.share.mkdir()
        (self.root / 'state').mkdir(parents=True)
        (self.root / 'bin').mkdir()
        source = ROOT / 'work/stage/bin/vsftpd'
        if not source.is_file():
            raise RuntimeError('Build host vsftpd before running FTP integration tests')
        shutil.copy2(source, self.root / 'bin/vsftpd')
        self.password = 'a separate FTP test password'
        self.user = pwd.getpwuid(os.geteuid()).pw_name
        subprocess.run([sys.executable, str(ROOT / 'tools/set-password.py'),
                        str(self.root / 'state'), '--service', 'ftp', '--stdin'],
                       input=self.password+'\n', text=True, capture_output=True, check=True)
        self.port, self.data_port = free_port(), free_port()
        while self.data_port in (self.port,2222) or self.port==2222:
            self.port, self.data_port = free_port(), free_port()
        self.config = (f'bind_address=127.0.0.1\nftp_port={self.port}\n'
                       f'pasv_min={self.data_port}\npasv_max={self.data_port}\n'
                       f'ftp_user=current\nftp_root={self.share}\n')
        (self.root / 'easy-connect.conf').write_text(self.config)

    @contextmanager
    def server(self):
        with tempfile.TemporaryFile(mode='w+t') as log:
            process = subprocess.Popen([str(BINARY), '--root', str(self.root), 'serve', 'ftp'],
                                       stdout=log, stderr=log)
            try:
                deadline = time.monotonic()+8
                while time.monotonic()<deadline:
                    log.seek(0)
                    text = log.read()
                    if 'ftp listening on' in text:
                        break
                    if process.poll() is not None:
                        self.fail('FTP startup failed: '+text)
                    time.sleep(0.05)
                else:
                    self.fail('FTP startup timed out')
                yield process
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)

    @contextmanager
    def client(self, user=None, password=None):
        client = ftplib.FTP(timeout=5)
        try:
            client.connect('127.0.0.1', self.port)
            client.login(user or self.user, password or self.password)
            yield client
        finally:
            client.close()

    def test_roundtrip_and_file_owner(self):
        payload = os.urandom(262144)
        with self.server(), self.client() as ftp:
            self.assertEqual(Path(ftp.pwd()).resolve(), self.share.resolve())
            name = 'book с пробелом.fb2'
            ftp.storbinary('STOR '+name, io.BytesIO(payload))
            self.assertEqual((self.share / name).stat().st_uid, os.geteuid())
            self.assertIn(name, ftp.nlst())
            output = io.BytesIO()
            ftp.retrbinary('RETR '+name, output.write)
            self.assertEqual(output.getvalue(), payload)
            ftp.rename(name, 'renamed.fb2')
            ftp.delete('renamed.fb2')
            ftp.mkd('new folder')
            ftp.rmd('new folder')

    def test_wrong_password_and_username_rejected(self):
        with self.server():
            for user, password in ((self.user,'wrong password'),
                                   ('not-the-launching-account',self.password),
                                   ('anonymous',self.password),('root',self.password)):
                with self.subTest(user=user):
                    with self.assertRaises(ftplib.error_perm) as rejected:
                        with self.client(user,password):
                            self.fail('Unauthorized login succeeded')
                    self.assertTrue(str(rejected.exception).startswith('530'), str(rejected.exception))

    def test_no_authentication_bypass(self):
        with self.server():
            ftp = ftplib.FTP(timeout=5)
            try:
                ftp.connect('127.0.0.1',self.port)
                with self.assertRaises(ftplib.error_perm):
                    ftp.pwd()
            finally:
                ftp.close()

    def test_current_user_permissions_are_enforced(self):
        path = self.share / 'unreadable'
        path.write_text('must not be read')
        path.chmod(0)
        try:
            with self.server(), self.client() as ftp:
                with self.assertRaises(ftplib.error_perm):
                    ftp.retrbinary('RETR unreadable', lambda chunk: None)
        finally:
            path.chmod(0o600)

    def test_starting_folder_is_not_a_jail(self):
        outside = Path(self.temp.name) / 'outside.txt'
        outside.write_bytes(b'current-user-access')
        with self.server(), self.client() as ftp:
            ftp.cwd('..')
            output = io.BytesIO()
            ftp.retrbinary('RETR outside.txt', output.write)
            self.assertEqual(output.getvalue(), outside.read_bytes())

    def test_corrupt_password_rejected(self):
        (self.root / 'state/ftp-password.hash').write_text('invalid verifier\n')
        with self.server():
            with self.assertRaises(ftplib.error_perm):
                with self.client():
                    self.fail('Corrupt verifier accepted')

    def test_account_switch_rejected(self):
        (self.root / 'easy-connect.conf').write_text(self.config.replace('ftp_user=current','ftp_user=root'))
        result = subprocess.run([str(BINARY),'--root',str(self.root),'serve','ftp'],
                                capture_output=True,text=True,timeout=10)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('non-root account',result.stderr)

    def test_active_mode_disabled(self):
        with self.server(), self.client() as ftp:
            with self.assertRaises(ftplib.error_perm):
                ftp.sendcmd('PORT 127,0,0,1,100,100')

    def test_anonymous_mode_needs_no_password_file(self):
        (self.root/'state/ftp-password.hash').unlink()
        (self.root/'easy-connect.conf').write_text(self.config+'ftp_auth=anonymous\n')
        with self.server(), self.client('anonymous','unused') as ftp:
            ftp.storbinary('STOR anonymous.txt',io.BytesIO(b'anonymous upload'))
            self.assertEqual((self.share/'anonymous.txt').read_bytes(),b'anonymous upload')
            self.assertEqual((self.share/'anonymous.txt').stat().st_uid,os.geteuid())
            with self.assertRaises(ftplib.error_perm):
                with self.client(): self.fail('Named login accepted in anonymous-only mode')

if __name__=='__main__':
    unittest.main()
