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

    def run_ui(self,events, *, width=None, height=None):
        script=self.root/'events.txt'
        script.write_text('\n'.join(events)+'\n')
        environment = dict(os.environ, EC_UI_ROOT=str(self.root), EC_UI_EVENTS=str(script))
        if width is not None:
            environment['EC_UI_WIDTH'] = str(width)
        if height is not None:
            environment['EC_UI_HEIGHT'] = str(height)
        result=subprocess.run([str(BINARY)],env=environment,
                              capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        return result

    def test_tabs_and_corresponding_details(self):
        ssh,ftp=self.root/'ssh.txt',self.root/'ftp.txt'
        self.run_ui([f'text {ssh}','press ftp-tab',f'text {ftp}'])
        self.assertIn('SSH daemon',ssh.read_text())
        self.assertIn('Public keys: 0 entries',ssh.read_text())
        self.assertNotIn('FTP daemon',ssh.read_text())
        self.assertIn('FTP daemon',ftp.read_text())
        self.assertIn('No password',ftp.read_text())
        self.assertIn('Starting folder:',ftp.read_text())

    def test_access_types_persist_and_survive_reopen(self):
        self.run_ui(['press ssh-password','press ftp-tab','press ftp-anonymous'])
        config=(self.root/'easy-connect.conf').read_text()
        self.assertIn('\nauth=password\n',config)
        self.assertIn('\nftp_auth=anonymous\n',config)
        text=self.root/'reopened.txt'
        self.run_ui(['press ftp-tab',f'text {text}'])
        self.assertIn('Login as anonymous',text.read_text())
        self.assertNotIn('Password: not set',text.read_text())

    def test_invalid_port_preserves_previous_settings(self):
        self.run_ui(['press ssh-password'])
        before=(self.root/'easy-connect.conf').read_bytes()
        text=self.root/'invalid.txt'
        self.run_ui(['press ssh-port','key 2121',f'text {text}'])
        self.assertEqual((self.root/'easy-connect.conf').read_bytes(),before)
        self.assertIn('must not overlap',text.read_text())

    def test_port_edit_is_saved(self):
        self.run_ui(['press ssh-port','key 2233'])
        self.assertIn('ssh_port=2233',(self.root/'easy-connect.conf').read_text())

    def test_ftp_folder_and_passive_ports(self):
        self.run_ui(['press ftp-tab','press ftp-root','key /mnt/ext1/Books',
                     'press ftp-passive-max','key 50050','press ftp-passive-min','key 50020'])
        config=(self.root/'easy-connect.conf').read_text()
        for expected in ('ftp_root=/mnt/ext1/Books','pasv_min=50020','pasv_max=50050'):
            self.assertIn(expected,config)

    def test_masked_password_confirmation_and_verifier(self):
        password='a new on-device test password'
        text=self.root/'password.txt'
        result=self.run_ui(['press ssh-password','press ssh-set-password',f'key {password}',f'key {password}',f'text {text}'])
        self.assertEqual(result.stdout.count('KEYBOARD masked'),2)
        record=(self.root/'state/password.hash').read_text().strip()
        version,iterations,salt,digest=record.split('$')
        self.assertEqual(version,'ec1')
        self.assertEqual(hashlib.pbkdf2_hmac('sha256',password.encode(),bytes.fromhex(salt),int(iterations)).hex(),digest)
        self.assertNotIn(password,record)
        self.assertNotIn(password,text.read_text())
        self.assertIn('Password: configured',text.read_text())

    def test_mismatched_confirmation_preserves_password(self):
        self.run_ui(['press ssh-password','press ssh-set-password','key a long first password','key a long first password'])
        path=self.root/'state/password.hash'
        before=path.read_bytes()
        self.run_ui(['press ssh-set-password','key a different long password','key does not match password'])
        self.assertEqual(path.read_bytes(),before)

    def test_ftp_password_is_separate(self):
        self.run_ui(['press ftp-tab','press ftp-set-password','key FTP has its own password','key FTP has its own password'])
        self.assertTrue((self.root/'state/ftp-password.hash').is_file())
        self.assertFalse((self.root/'state/password.hash').exists())

    def test_keyboard_cancel_and_overlay(self):
        text=self.root/'cancelled.txt'
        self.run_ui(['press ssh-port','hide','cancel','show',f'text {text}'])
        self.assertFalse((self.root/'easy-connect.conf').exists())
        self.assertIn('Edit cancelled.',text.read_text())

    def test_invalid_numeric_inputs_preserve_config(self):
        self.run_ui(['press ssh-port', 'key 2233'])
        before = (self.root / 'easy-connect.conf').read_bytes()
        for value in ('abc', '-1', '+2234', '1023', '65536', '9999999999999999999999', ' 2234'):
            with self.subTest(value=value):
                text = self.root / 'invalid.txt'
                self.run_ui(['press ssh-port', 'key ' + value, f'text {text}'])
                self.assertEqual((self.root / 'easy-connect.conf').read_bytes(), before)
                self.assertIn('Port must be a number', text.read_text())

    def test_relative_folder_and_inverted_passive_range_preserve_config(self):
        self.run_ui(['press ftp-tab', 'press ftp-root', 'key /tmp/books'])
        before = (self.root / 'easy-connect.conf').read_bytes()
        for events in (['press ftp-root', 'key relative/path'], ['press ftp-passive-min', 'key 50010']):
            self.run_ui(['press ftp-tab'] + events)
            self.assertEqual((self.root / 'easy-connect.conf').read_bytes(), before)

    def test_key_or_password_mode_shows_both_access_details(self):
        state = self.root / 'state'
        state.mkdir()
        (state / 'authorized_keys').write_text('# comment\n\nssh-ed25519 one\nssh-rsa two\n')
        text = self.root / 'both.txt'
        self.run_ui(['press ssh-both', f'text {text}'])
        self.assertIn('Public keys: 2 entries', text.read_text())
        self.assertIn('Password: not set', text.read_text())
        self.assertIn('auth=both', (self.root / 'easy-connect.conf').read_text())

    def test_cancel_password_confirmation_preserves_existing_verifier(self):
        self.run_ui(['press ssh-password', 'press ssh-set-password', 'key first long password', 'key first long password'])
        path = self.root / 'state/password.hash'
        before = path.read_bytes()
        self.run_ui(['press ssh-set-password', 'key replacement long password', 'cancel'])
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(path.parent.glob('.password.*')), [])

    def test_short_and_short_unicode_passwords_are_rejected(self):
        for password in ('too short', 'я' * 6):
            with self.subTest(password=password):
                text = self.root / 'short.txt'
                self.run_ui(['press ssh-password', 'press ssh-set-password', 'key ' + password, f'text {text}'])
                self.assertFalse((self.root / 'state/password.hash').exists())
                self.assertIn('Use at least 12 characters', text.read_text())

    def test_auth_changes_preserve_unedited_advanced_settings(self):
        settings = ('bind_address=127.0.0.1\nssh_port=2233\nftp_port=2133\n'
                    'pasv_min=51000\npasv_max=51001\nftp_root=/tmp/books\n'
                    f'state_dir={self.root}/custom state\nftp_user=current\n')
        path = self.root / 'easy-connect.conf'
        path.write_text(settings)
        self.run_ui(['press ssh-both', 'press ftp-tab', 'press ftp-anonymous'])
        for line in settings.splitlines():
            if not line.startswith('ftp_user='):
                self.assertIn(line, path.read_text())
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    @unittest.skipIf(os.geteuid() == 0, 'read-only directory requires non-root user')
    def test_save_failure_preserves_file_and_displayed_settings(self):
        self.run_ui(['press ssh-port', 'key 2233'])
        path = self.root / 'easy-connect.conf'
        before = path.read_bytes()
        text = Path(self.temp.name).with_name(Path(self.temp.name).name + '-failure.txt')
        self.addCleanup(text.unlink, missing_ok=True)
        self.root.chmod(0o500)
        try:
            # The test script must be created before making the directory read-only.
            script = self.root / 'events.txt'
            script.chmod(0o600)
            self.run_ui(['press ssh-port', 'key 2234', f'text {text}'])
            self.assertIn('cannot create settings file', text.read_text())
            self.assertIn('SSH port: 2233', text.read_text())
            self.assertEqual(path.read_bytes(), before)
        finally:
            self.root.chmod(0o700)

    def test_diagnostics_button_exports_without_password_material(self):
        self.run_ui(['press ssh-password', 'press ssh-set-password', 'key diagnostics test password',
                     'key diagnostics test password', 'press save-diagnostics'])
        report = (self.root / 'diagnostics.txt').read_text()
        verifier = (self.root / 'state/password.hash').read_text().strip()
        self.assertIn('SSH:', report)
        self.assertIn('FTP:', report)
        self.assertNotIn('diagnostics test password', report)
        self.assertNotIn(verifier, report)

    def test_missing_daemon_start_shows_stopped_and_error(self):
        text = self.root / 'failed.txt'
        self.run_ui(['press start', f'text {text}'])
        self.assertIn('STOPPED', text.read_text())
        self.assertNotIn('\nRUNNING\n', text.read_text())
        self.assertNotIn('Daemon started.', text.read_text())

    def test_named_controls_follow_scaled_screen_geometry(self):
        for width, height in ((600, 800), (1072, 1448), (1440, 1920)):
            with self.subTest(width=width, height=height):
                text = self.root / 'scaled.txt'
                self.run_ui(['press ssh-port', 'key 2233', 'press ftp-tab',
                             'press ftp-port', 'key 2133', 'press ftp-anonymous', f'text {text}'],
                            width=width, height=height)
                config = (self.root / 'easy-connect.conf').read_text()
                self.assertIn('ssh_port=2233', config)
                self.assertIn('ftp_port=2133', config)
                self.assertIn('ftp_auth=anonymous', config)
                self.assertIn('Login as anonymous', text.read_text())

    def test_pointer_input_is_ignored_while_keyboard_is_open(self):
        text = self.root / 'editing.txt'
        self.run_ui(['press ssh-port', 'press ftp-tab', 'key 2233', f'text {text}'])
        self.assertIn('SSH daemon', text.read_text())
        self.assertNotIn('FTP daemon', text.read_text())
        self.assertIn('ssh_port=2233', (self.root / 'easy-connect.conf').read_text())

if __name__=='__main__':
    unittest.main()
