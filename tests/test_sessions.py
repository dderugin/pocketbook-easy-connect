#!/usr/bin/env python3
"""Use the actual tab buttons against real local SSH and FTP daemons."""
from contextlib import contextmanager
from pathlib import Path
import ftplib
import io
import os
import pwd
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=Path(sys.argv.pop(1)).resolve()

@unittest.skipIf(os.geteuid()==0,'run current-user session tests without root')
class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='easy-connect-tabs-')
        self.root=Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.share=self.root/'books'
        self.share.mkdir()
        (self.root/'bin').mkdir()
        (self.root/'state').mkdir()
        for name in ('dropbear','dropbearkey','vsftpd'):
            shutil.copy2(ROOT/'work/stage/bin'/name,self.root/'bin'/name)
        self.key=self.root/'client'
        subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(self.key)],check=True)
        shutil.copy2(self.key.with_suffix('.pub'),self.root/'state/authorized_keys')
        self.password='separate FTP session password'
        subprocess.run([sys.executable,str(ROOT/'tools/set-password.py'),str(self.root/'state'),'--service','ftp','--stdin'],
                       input=self.password+'\n',text=True,check=True,capture_output=True)
        ports=set()
        while len(ports)<4:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1',0)); ports.add(sock.getsockname()[1])
        self.ssh_port,self.ftp_port,self.passive,self.new_port=ports
        (self.root/'easy-connect.conf').write_text(f'bind_address=127.0.0.1\nauth=key\n'
            f'ssh_port={self.ssh_port}\nftp_port={self.ftp_port}\npasv_min={self.passive}\npasv_max={self.passive}\n'
            f'ftp_root={self.share}\n')
        self.err=tempfile.TemporaryFile(mode='w+t')
        self.addCleanup(self.err.close)
        self.process=subprocess.Popen([str(BINARY)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.err,
            text=True,bufsize=1,env=dict(os.environ,EC_UI_ROOT=str(self.root),EC_UI_EVENTS='-'))
        self.pending=b''
        self.addCleanup(self.close_ui)

    def command(self,value):
        self.process.stdin.write(value+'\n'); self.process.stdin.flush()
        while True:
            while b'\n' not in self.pending:
                ready,_,_=select.select([self.process.stdout],[],[],10)
                self.assertTrue(ready,'UI command timed out: '+value)
                chunk=os.read(self.process.stdout.fileno(),4096)
                self.assertTrue(chunk,'UI exited unexpectedly')
                self.pending+=chunk
            line,self.pending=self.pending.split(b'\n',1)
            if line.strip()==b'UI_EVENT_DONE': return

    def close_ui(self):
        if self.process.poll() is None:
            self.command('quit')
            self.process.wait(timeout=10)
        self.process.stdin.close(); self.process.stdout.close()

    def tab(self,tab): self.command(f'tap {550 if tab else 100} 130')
    def start(self,tab): self.tab(tab); self.command('tap 100 340')
    def stop(self,tab): self.tab(tab); self.command('tap 550 340')

    def ssh(self):
        user=pwd.getpwuid(os.geteuid()).pw_name
        result=subprocess.run(['ssh','-F','/dev/null','-p',str(self.ssh_port),'-i',str(self.key),
            '-o','IdentityAgent=none','-o','IdentitiesOnly=yes','-o','BatchMode=yes',
            '-o','StrictHostKeyChecking=no','-o',f'UserKnownHostsFile={self.root}/known_hosts',
            f'{user}@127.0.0.1','printf session-alive'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout,'session-alive')

    @contextmanager
    def ftp(self,user=None,password=None):
        client=ftplib.FTP(timeout=5)
        try:
            client.connect('127.0.0.1',self.ftp_port)
            client.login(user or pwd.getpwuid(os.geteuid()).pw_name,password or self.password)
            yield client
        finally: client.close()

    def assert_closed(self,port):
        with socket.socket() as sock:
            sock.settimeout(2)
            self.assertNotEqual(sock.connect_ex(('127.0.0.1',port)),0)

    def test_stop_ssh_leaves_ftp_session_and_transfers_running(self):
        self.start(0); self.start(1); self.ssh()
        with self.ftp() as ftp:
            self.stop(0)
            self.assert_closed(self.ssh_port)
            ftp.storbinary('STOR after-ssh-stop.txt',io.BytesIO(b'FTP still running'))
            self.assertEqual((self.share/'after-ssh-stop.txt').read_bytes(),b'FTP still running')
        self.stop(1); self.assert_closed(self.ftp_port)

    def test_stop_ftp_leaves_ssh_running(self):
        self.start(0); self.start(1)
        self.stop(1); self.assert_closed(self.ftp_port); self.ssh()
        self.stop(0); self.assert_closed(self.ssh_port)

    def test_running_settings_locked_and_other_tab_keyboard_keeps_ssh_alive(self):
        self.start(0)
        before=(self.root/'easy-connect.conf').read_bytes()
        self.command('tap 350 460')
        self.assertEqual((self.root/'easy-connect.conf').read_bytes(),before)
        self.tab(1); self.command('tap 100 540'); self.command('hide')
        self.command(f'key {self.new_port}')
        self.assertIn(f'ftp_port={self.new_port}',(self.root/'easy-connect.conf').read_text())
        self.ssh()

    def test_anonymous_then_password_setup_from_ui(self):
        (self.root/'state/ftp-password.hash').unlink()
        self.tab(1); self.command('tap 550 460'); self.start(1)
        with self.ftp('anonymous','unused') as ftp:
            ftp.storbinary('STOR anonymous.txt',io.BytesIO(b'no password required'))
        self.assertEqual((self.share/'anonymous.txt').read_bytes(),b'no password required')
        self.stop(1); self.command('tap 100 460')
        self.command('tap 560 715'); self.command('key '+self.password); self.command('key '+self.password)
        self.start(1)
        with self.ftp() as ftp: self.assertIn('anonymous.txt',ftp.nlst())
        with self.assertRaises(ftplib.error_perm):
            with self.ftp('anonymous','unused'): self.fail('Anonymous login accepted in password mode')

    @unittest.skipUnless(sys.platform.startswith('linux'),'session descendant cleanup requires Linux')
    def test_stop_ssh_closes_active_session_and_keeps_ftp_running(self):
        self.start(0); self.start(1)
        user=pwd.getpwuid(os.geteuid()).pw_name
        client=subprocess.Popen(['ssh','-F','/dev/null','-p',str(self.ssh_port),'-i',str(self.key),
            '-o','IdentityAgent=none','-o','IdentitiesOnly=yes','-o','BatchMode=yes',
            '-o','StrictHostKeyChecking=no','-o',f'UserKnownHostsFile={self.root}/known_hosts',
            f'{user}@127.0.0.1','printf "ready\\n"; exec sleep 120'],
            stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            ready,_,_=select.select([client.stdout],[],[],10)
            self.assertTrue(ready)
            self.assertEqual(client.stdout.readline(),'ready\n')
            self.stop(0)
            self.assertNotEqual(client.wait(timeout=10),0)
            with self.ftp() as ftp: self.assertEqual(Path(ftp.pwd()).resolve(),self.share.resolve())
        finally:
            if client.poll() is None: client.terminate(); client.wait(timeout=5)
            client.stdout.close(); client.stderr.close()

if __name__=='__main__': unittest.main()
