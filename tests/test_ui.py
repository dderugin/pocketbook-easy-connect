#!/usr/bin/env python3
"""Drive the real device UI callbacks through the test-only InkView surface."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile
import time
import unittest
from ui_test_support import wait_exit_scan

BINARY=Path(sys.argv.pop(1)).resolve()

class UITests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='easy-connect-ui-')
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def run_ui(self,events, *, width=None, height=None, address='192.0.2.1', scanner=None, scan_timeout=None, helper=False, model=None, firmware=None, expected=0, exit_timeout=500, wait_exit=True):
        script=self.root/'events.txt'
        script.write_text('\n'.join(events)+'\n')
        environment = dict(os.environ, EC_UI_ROOT=str(self.root), EC_UI_EVENTS=str(script), EC_UI_ADDRESS=address, EC_UI_CONTROLLER=str(BINARY.with_name("easy-connect")))
        if model is not None:
            environment['EC_UI_MODEL'] = model
        if firmware is not None:
            environment['EC_UI_FIRMWARE'] = firmware
        environment['EC_UI_SCANNER'] = str(scanner or self.root / 'missing-scanner')
        environment['EC_UI_EXIT_SCAN_TIMEOUT_MS'] = str(exit_timeout)
        if helper:
            environment['EC_UI_HELPER'] = '1'
        if scan_timeout is not None:
            environment['EC_UI_SCAN_TIMEOUT_MS'] = str(scan_timeout)
        if width is not None:
            environment['EC_UI_WIDTH'] = str(width)
        if height is not None:
            environment['EC_UI_HEIGHT'] = str(height)
        before = time.monotonic()
        result=subprocess.run([str(BINARY)],env=environment,
                              capture_output=True,text=True,timeout=15)
        result.ui_exit_seconds = time.monotonic() - before
        if wait_exit:
            wait_exit_scan(self.root)
        self.assertEqual(result.returncode,expected,result.stderr)
        return result

    def test_shared_models_accept_changed_firmware_build_numbers(self):
        for model, firmware in (("PB632", "U632.6.11.9999"), ("PocketBook 634", "U634.6.10.9999"),
                                ("PB700", "U700.6.12.1"), ("PocketBook 743g (PB743g)", "U743G.6.11.9999")):
            text = self.root / "shared.txt"
            result = self.run_ui([f"text {text}"], model=model, firmware=firmware)
            self.assertIn("SSH daemon", text.read_text())
            self.assertIn("firmware:", result.stderr)

    def test_unknown_models_and_old_or_invalid_firmware_do_not_start_ui(self):
        for model, firmware in (("PB1030", "U1030.6.11.1"), ("PB743g", "U743g.6.9.1"),
                                ("PB632", "invalid")):
            text = self.root / "rejected.txt"
            result = self.run_ui(["press start", f"text {text}"], model=model, firmware=firmware, expected=1)
            self.assertIn("Unknown model or unsupported firmware API", result.stdout)
            self.assertFalse(text.exists())
            self.assertFalse((self.root / "state/password.hash").exists())

    def test_factory_install_prompts_for_password_without_missing_file_error(self):
        defaults = Path(__file__).resolve().parents[1] / 'data/easy-connect.conf.default'
        (self.root / 'easy-connect.conf.default').write_bytes(defaults.read_bytes())
        text = self.root / 'first-run.txt'
        result = self.run_ui(['press start', 'key first setup password',
                             'key first setup password', f'text {text}'])
        self.assertEqual(result.stdout.count('KEYBOARD masked'), 2)
        self.assertIn('Password: configured', text.read_text())
        self.assertNotIn('No such file', result.stderr)
        self.assertTrue((self.root / 'state/password.hash').is_file())
        self.assertEqual((self.root / 'easy-connect.conf.default').read_bytes(), defaults.read_bytes())

    def test_tabs_and_corresponding_details(self):
        ssh,ftp=self.root/'ssh.txt',self.root/'ftp.txt'
        self.run_ui([f'text {ssh}','press ftp-tab',f'text {ftp}'])
        self.assertIn('SSH daemon',ssh.read_text())
        self.assertIn('Public keys: 0 entries',ssh.read_text())
        self.assertNotIn('FTP daemon',ssh.read_text())
        self.assertIn('FTP daemon',ftp.read_text())
        self.assertIn('No password',ftp.read_text())
        self.assertIn('Starting folder:',ftp.read_text())

    def test_home_and_task_switcher_resume_restores_drawing_input_and_status_timer(self):
        for event in ('foreground', 'activate', 'focus'):
            with self.subTest(event=event):
                text = self.root / 'resumed.txt'
                events = ['press ftp-tab']
                for _ in range(3):
                    events += ['hide', 'background', event, 'press ssh-tab',
                               'press ssh-port', 'key 2233', 'press ftp-tab']
                events += ['press ssh-tab', 'network 192.0.2.78', 'tick', f'text {text}']
                self.run_ui(events)
                screen = text.read_text()
                self.assertIn('SSH daemon', screen)
                self.assertIn('SSH port: 2233', screen)
                self.assertIn('192.0.2.78', screen)
                self.assertIn('EASY CONNECT', screen)

    def test_background_and_exit_preserve_inkview_helper_children(self):
        self.run_ui(['background', 'hide', 'show', 'press ftp-tab',
                     'background', 'show', 'press exit'], helper=True)

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

    def test_start_explains_missing_ssh_key_without_creating_credentials(self):
        text = self.root / 'missing-key.txt'
        result = self.run_ui(['press start', f'text {text}'])
        self.assertIn('Add a .pub key', text.read_text())
        self.assertNotIn('No such file', result.stderr)
        self.assertFalse((self.root / 'state/authorized_keys').exists())

    def test_start_prompts_for_missing_password_for_each_service(self):
        for service in ('ssh', 'ftp'):
            with self.subTest(service=service):
                text = self.root / (service + '-setup.txt')
                events = ['press ssh-password'] if service == 'ssh' else ['press ftp-tab']
                result = self.run_ui(events + ['press start', 'key new setup password',
                    'key new setup password', f'text {text}'])
                self.assertEqual(result.stdout.count('KEYBOARD masked'), 2)
                name = 'password.hash' if service == 'ssh' else 'ftp-password.hash'
                self.assertTrue((self.root / 'state' / name).is_file())
                self.assertIn('Password: configured', text.read_text())
                self.assertNotIn('No such file', result.stderr)
                self.assertIn('STOPPED', text.read_text())

    def test_toolbar_icons_stay_at_top_after_refresh_keyboard_and_resume(self):
        import xml.etree.ElementTree as ET
        for width, height in ((720, 960), (1072, 1448), (1264, 1680), (1404, 1872), (1448, 1072), (1680, 1264), (1872, 1404)):
            with self.subTest(width=width, height=height):
                snapshots = [self.root / f'frame-{i}.svg' for i in range(6)]
                self.run_ui([f'snapshot {snapshots[0]}', 'repaint',
                    f'snapshot {snapshots[1]}', 'press ssh-port', 'cancel',
                    'hide', 'show', f'snapshot {snapshots[2]}', 'orientation',
                    'press ftp-tab', 'press ssh-tab', 'press ftp-tab', 'repaint',
                    f'snapshot {snapshots[3]}', 'background', 'foreground',
                    f'snapshot {snapshots[4]}', 'hide', 'activate',
                    f'snapshot {snapshots[5]}'],
                    width=width, height=height)
                positions = []
                for snapshot in snapshots:
                    elements = list(ET.parse(snapshot).getroot().iter())
                    icons = [e for e in elements if e.attrib.get('data-resource')]
                    self.assertEqual({e.attrib['data-resource'] for e in icons}, {'home','refresh','ci_diagnostic'})
                    caption = [e for e in elements if e.attrib.get('data-theme-font')]
                    self.assertEqual(len(caption),1)
                    self.assertEqual(caption[0].attrib['data-theme-font'],'application.caption.font')
                    self.assertEqual(caption[0].attrib['font-weight'],'normal')
                    self.assertEqual(caption[0].text,'EASY CONNECT')
                    self.assertTrue(all(0 <= int(e.attrib['data-y']) < height * .1 for e in icons))
                    positions.append([tuple(sorted(e.attrib.items())) for e in icons] +
                                     [tuple(sorted(caption[0].attrib.items()))])
                self.assertTrue(all(p == positions[0] for p in positions))

    def test_offline_controls_are_blocked_then_enable_automatically(self):
        offline, online = self.root / 'offline.txt', self.root / 'online.txt'
        offline_ssh = self.root / 'offline-ssh.txt'
        self.run_ui(['press ssh-password', 'press start', 'press ftp-tab',
                     f'text {offline}', 'press ftp-anonymous', 'press ftp-port',
                     'press ssh-tab', f'text {offline_ssh}', 'press save-diagnostics',
                     'network 192.0.2.42', 'tick', 'press ssh-port', 'key 2233',
                     'press ftp-tab', f'text {online}'], address='')
        self.assertIn('Waiting for an IP address', offline.read_text())
        self.assertIn('FTP daemon', offline.read_text())
        self.assertIn('SSH daemon', offline_ssh.read_text())
        self.assertNotIn('Login as anonymous', offline.read_text())
        self.assertTrue((self.root / 'diagnostics.txt').is_file())
        self.assertIn('192.0.2.42', online.read_text())
        self.assertIn('FTP daemon', online.read_text())
        self.assertIn('ssh_port=2233', (self.root / 'easy-connect.conf').read_text())

    def test_network_loss_disables_controls_on_next_status_tick(self):
        text = self.root / 'lost.txt'
        self.run_ui(['network', 'tick', 'press ssh-password', 'press start', f'text {text}'])
        self.assertIn('Waiting for an IP address', text.read_text())
        self.assertFalse((self.root / 'easy-connect.conf').exists())

    def test_named_controls_follow_scaled_screen_geometry(self):
        for width, height in ((600, 800), (1072, 1448), (1264, 1680), (1404, 1872), (1448, 1072), (1680, 1264), (1872, 1404)):
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

    def make_scanner(self, body):
        scanner = self.root / 'scanner.app'
        scanner.write_text(f'#!{sys.executable}\n'
            'import os, signal, sys, time\nfrom pathlib import Path\n'
            f'Path({str(self.root / "scanner.pid")!r}).write_text(str(os.getpid()))\n'
            f'with open({str(self.root / "launches")!r}, "a") as f: f.write("start\\n")\n'
            + body + '\n')
        scanner.chmod(0o755)
        return scanner

    def assert_scanner_gone(self):
        pid = int((self.root / 'scanner.pid').read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_library_refresh_available_on_both_tabs_offline(self):
        scanner = self.make_scanner('print("Scan finished", flush=True)\ntime.sleep(60)')
        for tab in ('ssh', 'ftp'):
            with self.subTest(tab=tab):
                text = self.root / 'refresh.txt'
                self.run_ui([f'press {tab}-tab', 'press refresh-library', 'wait 450',
                             f'text {text}'], address='', scanner=scanner)
                self.assertIn('Library refresh finished', text.read_text())
                self.assert_scanner_gone()
        self.assertEqual((self.root / 'launches').read_text().count('start'), 4)

    def test_library_refresh_survives_background_without_duplicate_launch(self):
        scanner = self.make_scanner('print("Scan finished", flush=True)\ntime.sleep(60)')
        text = self.root / 'refresh.txt'
        self.run_ui(['press refresh-library', 'press refresh-library', 'background',
                     'hide', 'wait 450', 'show', 'press ftp-tab', f'text {text}',
                     'press save-diagnostics'], scanner=scanner)
        self.assertIn('Library refresh finished', text.read_text())
        self.assertEqual((self.root / 'launches').read_text().count('start'), 2)
        self.assertIn('Scan finished', (self.root / 'library-scan.log').read_text())
        self.assertIn('Library scan log:', (self.root / 'diagnostics.txt').read_text())
        self.assert_scanner_gone()

    def test_library_refresh_missing_or_failed_scanner_reports_error(self):
        text = self.root / 'refresh.txt'
        self.run_ui(['press refresh-library', f'text {text}'], address='')
        self.assertIn('Library scanner unavailable', text.read_text())
        scanner = self.make_scanner('print("scanner failure", flush=True)\nsys.exit(7)')
        self.run_ui(['press refresh-library', 'wait 450', f'text {text}'], scanner=scanner)
        self.assertIn('Library refresh failed', text.read_text())
        self.assertNotIn('Library refresh finished', text.read_text())
        self.assertIn('scanner failure', (self.root / 'library-scan.log').read_text())
        self.assert_scanner_gone()

    def test_library_refresh_timeout_and_exit_cleanup(self):
        scanner = self.make_scanner('time.sleep(60)')
        text = self.root / 'timeout.txt'
        self.run_ui(['press refresh-library', 'wait 600', f'text {text}'],
                    scanner=scanner, scan_timeout=150)
        self.assertIn('Library refresh timed out', text.read_text())
        self.assert_scanner_gone()
        self.run_ui(['press refresh-library', 'wait 200', 'press exit'], scanner=scanner)
        self.assert_scanner_gone()

    def test_library_refresh_forces_cleanup_when_scanner_ignores_termination(self):
        scanner = self.make_scanner('signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                                    'print("Scan finished", flush=True)\ntime.sleep(60)')
        text = self.root / 'forced-cleanup.txt'
        self.run_ui(['press refresh-library', 'wait 1700', f'text {text}'], scanner=scanner)
        self.assertIn('Library refresh finished', text.read_text())
        self.assert_scanner_gone()

    def test_library_refresh_rejects_unsafe_log_without_touching_target(self):
        scanner = self.make_scanner('sys.exit(0)')
        target = self.root / 'keep.txt'
        target.write_text('keep this')
        (self.root / 'library-scan.log').symlink_to(target)
        text = self.root / 'unsafe-log.txt'
        self.run_ui(['press refresh-library', f'text {text}'], scanner=scanner)
        self.assertIn('Cannot open library-scan.log', text.read_text())
        self.assertEqual(target.read_text(), 'keep this')
        self.assert_scanner_gone()
        (self.root / 'library-scan.log').unlink()
        os.mkfifo(self.root / 'library-scan.log')
        self.run_ui(['press refresh-library', f'text {text}'], scanner=scanner)
        self.assertIn('Cannot create a regular library scan log', text.read_text())
        self.assert_scanner_gone()

    def test_library_refresh_allows_start_and_handles_exec_failure(self):
        scanner = self.make_scanner('time.sleep(60)')
        text = self.root / 'busy.txt'
        self.run_ui(['press refresh-library', 'press start', f'text {text}'], scanner=scanner)
        self.assertIn('Add a .pub key', text.read_text())
        self.assertNotIn('Wait for the library refresh', text.read_text())
        scanner.write_text('invalid executable contents\n')
        self.run_ui(['press refresh-library', 'wait 450', f'text {text}'], scanner=scanner)
        self.assertIn('Library refresh failed', text.read_text())

    def test_library_refresh_completion_marker_across_chunks_and_repeated_refresh(self):
        scanner = self.make_scanner('sys.stdout.write("x" * 4090 + "Scan finished\\n")\n'
                                    'sys.stdout.flush()\ntime.sleep(60)')
        text = self.root / 'done.txt'
        self.run_ui(['press refresh-library', 'wait 450', 'press refresh-library',
                     'wait 450', f'text {text}'], scanner=scanner)
        self.assertIn('Library refresh finished', text.read_text())
        self.assertEqual((self.root / 'launches').read_text().count('start'), 3)
        self.assert_scanner_gone()

    def test_home_back_and_firmware_exit_each_refresh_once_even_offline(self):
        scanner = self.make_scanner('print("Scan finished", flush=True)\ntime.sleep(60)')
        for action in ('press home', 'back', 'quit'):
            result = self.run_ui([action], address='', scanner=scanner)
            self.assertEqual(result.stderr.count('Exit library refresh handed off'), 1)
            self.assertIn('status=0', (self.root / 'library-exit-scan.log').read_text())
            self.assert_scanner_gone()
        self.assertEqual((self.root / 'launches').read_text().count('start'), 3)

    def test_exit_does_not_wait_for_scan_or_leave_ui_pipes_open(self):
        finish = self.root / 'finish-exit-scan'
        scanner = self.make_scanner(f'while not Path({str(finish)!r}).exists(): time.sleep(.02)\n'
                                    'print("Scan finished", flush=True)\ntime.sleep(60)')
        try:
            result = self.run_ui(['press home'], scanner=scanner, exit_timeout=3000, wait_exit=False)
            self.assertLess(result.ui_exit_seconds, 1.5)
            deadline = time.monotonic() + 1
            while not (self.root / 'scanner.pid').exists() and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertTrue((self.root / 'scanner.pid').exists())
            self.assertNotIn('Exit library refresh finished', (self.root / 'library-exit-scan.log').read_text())
        finally:
            finish.touch()
            wait_exit_scan(self.root)
        self.assert_scanner_gone()

    def test_repeated_exit_scans_do_not_share_completion_markers(self):
        scanner = self.make_scanner(f'finish=Path({str(self.root)!r})/f"finish-{{os.getpid()}}"\n'
                                    'while not finish.exists(): time.sleep(.02)\n'
                                    'print("Scan finished",flush=True)\ntime.sleep(60)')
        pids=[]
        try:
            for _ in range(2):
                self.run_ui(['press home'],scanner=scanner,exit_timeout=3000,wait_exit=False)
                deadline=time.monotonic()+1
                while time.monotonic()<deadline:
                    pid=int((self.root/'scanner.pid').read_text()) if (self.root/'scanner.pid').exists() else 0
                    if pid and pid not in pids: break
                    time.sleep(.02)
                self.assertTrue(pid and pid not in pids)
                pids.append(pid)
            (self.root/f'finish-{pids[0]}').touch()
            time.sleep(.35)
            # The latest scan must still wait for its own completion message.
            os.kill(pids[1],0)
            self.assertNotIn('Exit library refresh finished',(self.root/'library-exit-scan.log').read_text())
        finally:
            for pid in pids: (self.root/f'finish-{pid}').touch()
            wait_exit_scan(self.root)
        self.assert_scanner_gone()

    def test_exit_scan_timeout_stops_its_scanner(self):
        scanner = self.make_scanner('signal.signal(signal.SIGTERM, signal.SIG_IGN)\ntime.sleep(60)')
        self.run_ui(['press home'], scanner=scanner, exit_timeout=150)
        self.assertIn('status=124', (self.root / 'library-exit-scan.log').read_text())
        self.assert_scanner_gone()

    def test_exit_scan_rejects_symlinked_log_without_changing_target(self):
        scanner = self.make_scanner('sys.exit(0)')
        target = self.root / 'keep-exit.txt'
        target.write_text('keep this')
        (self.root / 'library-exit-scan.log').symlink_to(target)
        result = self.run_ui(['press home'], scanner=scanner)
        self.assertIn('Cannot open library-exit-scan.log', result.stderr)
        self.assertEqual(target.read_text(), 'keep this')
        self.assertFalse((self.root / 'scanner.pid').exists())

    def test_pointer_input_is_ignored_while_keyboard_is_open(self):
        text = self.root / 'editing.txt'
        self.run_ui(['press ssh-port', 'press ftp-tab', 'key 2233', f'text {text}'])
        self.assertIn('SSH daemon', text.read_text())
        self.assertNotIn('FTP daemon', text.read_text())
        self.assertIn('ssh_port=2233', (self.root / 'easy-connect.conf').read_text())

if __name__=='__main__':
    unittest.main()
