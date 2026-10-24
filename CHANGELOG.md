# Changelog

## 0.1.0 update — 2026-10-24

- Add Refresh Library to both tabs, including offline use and during SSH/FTP transfers.
- Supervise the firmware scanner asynchronously, report errors/timeouts, preserve
  its log, and clean up only the temporary scan instance on completion or exit.
- Preserve connections while the firmware scanner temporarily takes the foreground;
  services can also be started or stopped during a refresh.
- Add scanner lifecycle and three-button footer regression coverage.
- Restore drawing, touch geometry and status polling after Home/task-switcher
  foreground and activation events, even when the firmware omits EVT_SHOW.
- Limit service cleanup to SSH/FTP supervisors so backgrounding and exit do not
  kill or reap InkView helper children. Stop the library scanner separately.
- Handle focus-only resumes and record bounded lifecycle/touch traces in ui.log.
- Document exiting before USB upgrades and fully restarting if an old instance
  was left running. The updated build was confirmed working on PB632 by the user.

Validation: all five CTest suites, 15 real SSH/FTP UI session tests and ARM ABI
checks passed. The user confirmed the installed build after the Home/reopen fix;
full scanner, suspend and USB lifecycle acceptance testing remains separate.

## 0.1.0 — 2026-10-23

First development release for PocketBook Touch HD 3 / PB632, firmware U632.6.11.2001.

- Independent SSH and FTP tabs, Start/Stop controls, saved settings and password setup.
- SSH public-key/password authentication with persistent Ed25519 host keys.
- Basic SSH shell hint: `ssh -T -p 2222 reader@DEVICE_IP '/bin/sh -i'`.
- Recover from a rejected PTY request so shell and command requests still work.
- Password-protected or explicitly anonymous FTP using the launching account.
- Minimal seven-file install ZIP, safe factory configuration and a separate source/notice archive.
- Offline controls update automatically when an IP appears; tabs remain clickable.
- Disable the system-panel framebuffer offset to prevent footer wraparound.
- Local Docker build and regression coverage for configuration, credentials, UI and packaging.

### Installation

Extract the install ZIP and merge `applications/` and `system/` into USB storage.
Safely eject, connect Wi-Fi, then open **Applications → EasyConnect**. Press
**Start** to configure each service's separate password, then press **Start**
again. Existing settings and credentials take precedence over factory defaults.
The `-sources.zip` is a computer-side companion; do not install it on the reader.

### Validation and limitations

All five CTest suites and 19 host SSH integration tests passed during development.
Anonymous FTP access and SSH basic commands/shell were confirmed on PB632. The
updated interface was subsequently confirmed working by the user; full device
acceptance testing remains incomplete.
Other models and firmware are unverified. Keep the app in the foreground.
FTP is unencrypted; FTPS, SFTP and SCP are not included. Anonymous mode grants
network clients the reader account's permitted file operations. The FTP starting
folder is not a jail. PTY-dependent applications and job control need terminal
permissions that normal firmware launches may not provide. A pipe shell may
print "can't access tty; job control turned off"; basic commands still work.
