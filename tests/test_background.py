#!/usr/bin/env python3
"""Actual detached controller and ARM-compatible private socket operations."""
from pathlib import Path
import ftplib
import io
import os
import pwd
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve()
SERVER = BINARY

@unittest.skipIf(os.geteuid()==0,'services must run as the launching non-root user')
class BackgroundTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='easy-connect-background-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root/'bin').mkdir()
        (self.root/'state').mkdir()
        (self.root/'books').mkdir()
        shutil.copy2(SERVER, self.root/'bin/easy-connect')
        for name in ('dropbear','dropbearkey','vsftpd'):
            shutil.copy2(ROOT/'work/stage/bin'/name,self.root/'bin'/name)
        import socket
        ports=[]
        while len(ports)<3:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1',0))
                port=sock.getsockname()[1]
            if port not in ports: ports.append(port)
        self.ssh,self.ftp,self.passive=ports
        (self.root/'easy-connect.conf').write_text(f'bind_address=127.0.0.1\n'
            f'auth=password\nssh_port={self.ssh}\nftp_port={self.ftp}\n'
            f'pasv_min={self.passive}\npasv_max={self.passive}\nftp_root={self.root}/books\n')
        self.password='background service password'
        for service in ('ssh','ftp'):
            subprocess.run([sys.executable,str(ROOT/'tools/set-password.py'),str(self.root/'state'),
                            '--service',service,'--stdin'],input=self.password+'\n',text=True,
                            capture_output=True,check=True)
        self.addCleanup(self.stop_controller)

    def run_command(self,*arguments,check=True):
        result=subprocess.run([str(BINARY),'--root',str(self.root),*arguments],capture_output=True,
                              text=True,timeout=12)
        if check: self.assertEqual(result.returncode,0,result.stderr)
        return result

    def stop_controller(self):
        self.run_command('background-stop','both')
        # Wait for the final response to be followed by controller cleanup.
        for _ in range(100):
            if 'owner=0' in self.run_command('background-status').stdout: return
            time.sleep(.02)
        self.fail('controller remained after explicit Stop')

    def test_detached_controller_and_independent_explicit_stop(self):
        self.run_command('background-start','ssh')
        self.run_command('background-start','ftp')
        status=self.run_command('background-status').stdout
        self.assertNotIn('ssh=0',status)
        self.assertNotIn('ftp=0',status)
        self.assertNotIn('owner=0',status)
        with ftplib.FTP() as ftp:
            ftp.connect('127.0.0.1',self.ftp,timeout=5)
            ftp.login(pwd.getpwuid(os.geteuid()).pw_name,self.password)
            ftp.storbinary('STOR background.txt',io.BytesIO(b'background transfer'))
            self.run_command('background-stop','ssh')
            self.assertIn('background.txt',ftp.nlst())
        runtime=Path(status.split('runtime=')[1].strip())
        self.run_command('background-stop','ftp')
        self.stop_controller()
        for secret in ('host_ed25519','authorized_keys','password.hash','ftp-password.hash'):
            self.assertFalse((runtime/secret).exists())

    def test_simultaneous_starts_share_one_controller(self):
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(self.run_command,'background-start',service) for service in ('ssh','ftp')]
            for future in futures: future.result()
        status=self.run_command('background-status').stdout
        self.assertNotIn('ssh=0',status)
        self.assertNotIn('ftp=0',status)
        self.assertNotIn('owner=0',status)

    def test_another_installation_cannot_stop_running_services(self):
        self.run_command('background-start','ftp')
        other=self.root/'other-installation'
        other.mkdir()
        rejected=subprocess.run([str(BINARY),'--root',str(other),'background-stop','ftp'],
                                capture_output=True,text=True,timeout=10)
        self.assertNotEqual(rejected.returncode,0)
        self.assertIn('another Easy Connect installation',rejected.stderr)
        self.assertNotIn('ftp=0',self.run_command('background-status').stdout)

    def test_malformed_request_does_not_affect_services(self):
        import socket
        self.run_command('background-start','ftp')
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
            client.connect(f'/tmp/easy-connect-{os.geteuid()}/control.sock')
            client.sendall(b'bad request')
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(client.recv(1),b'')
        self.assertNotIn('ftp=0',self.run_command('background-status').stdout)

    def test_public_socket_is_rejected_and_can_be_restored(self):
        self.run_command('background-start','ftp')
        control=Path(f'/tmp/easy-connect-{os.geteuid()}/control.sock')
        try:
            control.chmod(0o666)
            rejected=self.run_command('background-status',check=False)
            self.assertNotEqual(rejected.returncode,0)
            self.assertIn('owned and private',rejected.stderr)
        finally:
            control.chmod(0o600)
        self.assertNotIn('ftp=0',self.run_command('background-status').stdout)

if __name__=='__main__': unittest.main()
