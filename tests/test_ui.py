#!/usr/bin/env python3
"""Drive the real device UI callbacks through the test-only InkView surface."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

BINARY=Path(sys.argv.pop(1)).resolve()

class UITests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='easy-connect-ui-')
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def run_ui(self,events):
        script=self.root/'events.txt'
        script.write_text('\n'.join(events)+'\n')
        result=subprocess.run([str(BINARY)],env=dict(os.environ,EC_UI_ROOT=str(self.root),EC_UI_EVENTS=str(script)),
                              capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        return result

    def test_tabs_and_corresponding_details(self):
        ssh,ftp=self.root/'ssh.txt',self.root/'ftp.txt'
        self.run_ui([f'text {ssh}','tap 550 130',f'text {ftp}'])
        self.assertIn('SSH daemon',ssh.read_text())
        self.assertIn('Public keys: 0 entries',ssh.read_text())
        self.assertNotIn('FTP daemon',ssh.read_text())
        self.assertIn('FTP daemon',ftp.read_text())
        self.assertIn('No password',ftp.read_text())
        self.assertIn('Starting folder:',ftp.read_text())

    def test_access_types_persist_and_survive_reopen(self):
        self.run_ui(['tap 350 460','tap 550 130','tap 550 460'])
        config=(self.root/'easy-connect.conf').read_text()
        self.assertIn('\nauth=password\n',config)
        self.assertIn('\nftp_auth=anonymous\n',config)
        text=self.root/'reopened.txt'
        self.run_ui(['tap 550 130',f'text {text}'])
        self.assertIn('Login as anonymous',text.read_text())
        self.assertNotIn('Password: not set',text.read_text())

    def test_invalid_port_preserves_previous_settings(self):
        self.run_ui(['tap 350 460'])
        before=(self.root/'easy-connect.conf').read_bytes()
        text=self.root/'invalid.txt'
        self.run_ui(['tap 100 540','key 2121',f'text {text}'])
        self.assertEqual((self.root/'easy-connect.conf').read_bytes(),before)
        self.assertIn('must not overlap',text.read_text())

    def test_port_edit_is_saved(self):
        self.run_ui(['tap 100 540','key 2233'])
        self.assertIn('ssh_port=2233',(self.root/'easy-connect.conf').read_text())

    def test_ftp_folder_and_passive_ports(self):
        self.run_ui(['tap 550 130','tap 100 620','key /mnt/ext1/Books',
                     'tap 550 540','key 50050','tap 350 540','key 50020'])
        config=(self.root/'easy-connect.conf').read_text()
        for expected in ('ftp_root=/mnt/ext1/Books','pasv_min=50020','pasv_max=50050'):
            self.assertIn(expected,config)

    def test_masked_password_confirmation_and_verifier(self):
        password='a new on-device test password'
        text=self.root/'password.txt'
        result=self.run_ui(['tap 350 460','tap 560 610',f'key {password}',f'key {password}',f'text {text}'])
        self.assertEqual(result.stdout.count('KEYBOARD masked'),2)
        record=(self.root/'state/password.hash').read_text().strip()
        version,iterations,salt,digest=record.split('$')
        self.assertEqual(version,'ec1')
        self.assertEqual(hashlib.pbkdf2_hmac('sha256',password.encode(),bytes.fromhex(salt),int(iterations)).hex(),digest)
        self.assertNotIn(password,record)
        self.assertNotIn(password,text.read_text())
        self.assertIn('Password: configured',text.read_text())

    def test_mismatched_confirmation_preserves_password(self):
        self.run_ui(['tap 350 460','tap 560 610','key a long first password','key a long first password'])
        path=self.root/'state/password.hash'
        before=path.read_bytes()
        self.run_ui(['tap 560 610','key a different long password','key does not match password'])
        self.assertEqual(path.read_bytes(),before)

    def test_ftp_password_is_separate(self):
        self.run_ui(['tap 550 130','tap 560 715','key FTP has its own password','key FTP has its own password'])
        self.assertTrue((self.root/'state/ftp-password.hash').is_file())
        self.assertFalse((self.root/'state/password.hash').exists())

    def test_keyboard_cancel_and_overlay(self):
        text=self.root/'cancelled.txt'
        self.run_ui(['tap 100 540','hide','cancel','show',f'text {text}'])
        self.assertFalse((self.root/'easy-connect.conf').exists())
        self.assertIn('Edit cancelled.',text.read_text())

if __name__=='__main__':
    unittest.main()
