# Easy Connect for PocketBook

Standalone SSH and FTP communication app for **PocketBook Touch HD 3 / PB632,
firmware U632.6.11.2001**. Independent of CoolReader; it reuses the same
B288 / Cortex-A7 softfp SDK target and USB installation layout.

**Development version, not yet validated on a physical reader.**

## Implemented

- Portable C controller and diagnostics command.
- Dropbear SSH: public key, app-specific password, or either method.
- Persistent per-device Ed25519 host keys and host-key fingerprint output.
- Configuration validation, private runtime staging, exclusive session lock,
  foreground server supervision, startup checks, and shutdown on Ctrl-C/SIGTERM.
- Two InkView tabs, SSH and FTP, each with daemon status, independent Start/Stop
  buttons, connection details and saved configuration.
- On-device password setup with masked entry and confirmation, plus SSH public
  key count/location and server fingerprint.
- vsftpd FTP as the **launching user**, with either a separate application
  password or explicit anonymous access. Optional privileged launches can use chroot.
- Separate device build/package scripts, checksummed source dependencies, and
  automated controller/real SSH integration tests.

The InkView target and ARM dependency linking still need testing with the B288 SDK
and physical reader. Normal PocketBook app privileges are unknown until the
diagnostic app is run. SSH and FTP transfers are tested locally as a normal user;
physical-reader verification is pending. No root or jailbreak is needed for FTP.

## Build and test on a computer

Requirements: CMake 3.18+, C compiler, Make, Python 3, OpenSSL development files,
and OpenSSH client/keygen for integration tests. No Python is needed on the reader.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

# Explicit network step; subsequent builds use the verified offline cache.
python3 tools/dependencies.py fetch
python3 tools/dependencies.py build --service dropbear
python3 tests/test_ssh.py build/easy-connect
python3 tools/dependencies.py build --service vsftpd
python3 tests/test_ftp.py build/easy-connect
python3 tests/test_sessions.py build/ec-ui-test

# Render both tabs from the actual C drawing code (SVG, plus PNG if rsvg-convert exists).
python3 tools/preview-ui.py --build build --out work/ui-preview
```

For Homebrew OpenSSL on macOS, append
`--openssl-prefix /opt/homebrew/opt/openssl@3` to the dependency build command
(use the actual installed prefix). Linux builds need OpenSSL development headers.
If CMake cannot find OpenSSL, configure with `-DOPENSSL_ROOT_DIR=/path/to/openssl`.
Linux and macOS support controller, SSH and FTP development tests. The macOS
vsftpd build disables unavailable wtmp logging and Linux-style address-space caps;
these compatibility changes do not apply to the Linux/ARM build.
Tests listen only on loopback and use disposable keys and credentials.
CTest includes controller, core filesystem/credential, UI, and offline packaging
and dependency tests. The integration commands above exercise real daemons and
independent service lifecycles. See [the test coverage matrix](tests/README.md)
for covered behavior and the remaining SDK/device validation gaps.

## Build for the reader

Use a Linux build host and **SDK-B288-6.10**, matching the CoolReader project.
The SDK must provide compatible OpenSSL headers/libraries and InkView.
The ARM target has not been built in this development environment.

```sh
python3 tools/dependencies.py fetch
export POCKETBOOK_SDK=/absolute/path/to/SDK-B288-6.10
bash tools/build-device.sh
```

The build creates `dist/easy-connect-pb632-0.1.0.zip` and a SHA-256 checksum.
Packaging rejects host binaries and requires all five ARM executables. The ZIP
includes licenses, complete corresponding source and the pinned upstream archives.
Inspect required ELF libraries against the firmware runtime before device installation;
an ARM ELF header alone does not establish firmware compatibility.

## Install and configure

1. Extract/merge the ZIP into the reader's USB storage root.
2. Optionally copy `system/share/easy-connect/easy-connect.conf.example` to
   `easy-connect.conf` for advanced configuration; the UI saves its own settings.
3. Import SSH public keys over USB if using key access. Passwords may be set in
   the corresponding tab or provisioned with the host tool below.
4. Safely eject USB, connect the reader to Wi-Fi using its settings, then open
   **Applications → EasyConnect**.
5. Start the desired service. Closing or backgrounding the app stops services.

The ZIP never includes live configuration, credentials, or host keys, so upgrades
preserve them. It does not modify CoolReader files or firmware system accounts.
Services do not automatically start at boot. Keep the app in the foreground for
this first version; Wi-Fi keepalive and suspend/USB event behavior need device validation.

### Device interface

The **SSH** tab shows daemon status, its connection command, Start/Stop buttons,
Public key / Password / Key or password selection, editable port, password
configuration status and setup button, and public-key installation details.
The server fingerprint is displayed after the host key has been created.

The **FTP** tab shows daemon status, its connection URL, Start/Stop buttons,
Password / No password selection, editable control/passive ports and starting
folder, and the relevant username/password or anonymous-login details.

Changes save immediately and survive app restarts. Configuration for a running
daemon is locked; stop that daemon before editing it. Stopping SSH leaves FTP
running, and stopping FTP leaves SSH running. Keyboard overlays do not stop the
other daemon. Closing or backgrounding the whole app stops both.

Each password is entered twice with masked input and saved as a salted verifier.
Existing passwords are never displayed. Public key import is currently via USB;
the UI reports the file location and entry count.

### SSH public keys

Generate a client key on the computer:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/pocketbook
```

Copy the **public** `pocketbook.pub` contents to
`system/share/easy-connect/state/authorized_keys` over USB. Multiple keys may be
listed one per line. Keep the private client key on the computer. Use `auth=key`.
The screen reports the actual device account name and address:

```sh
ssh -p 2222 -i ~/.ssh/pocketbook DEVICE_USER@DEVICE_IP
```

Compare the server fingerprint with the device screen before accepting it.
SSH sessions have the launching account's permissions; ordinary launches do not
provide root access. Both command execution and interactive PTY access require
physical-reader verification. SFTP and SCP support are not included yet.

### SSH password

Use **SSH → Password / Key or password → Set password**, or run the host tool
while the reader is mounted over USB:

```sh
python3 tools/set-password.py /path/to/reader/system/share/easy-connect/state
```

The tool prompts twice and writes a salted PBKDF2-HMAC-SHA256 verifier, not the
plaintext password. Set `auth=password` for password-only access or `auth=both`
for password **or** public-key access. `both` requires a nonempty `authorized_keys`
file too. Keys/passwords are copied at service startup: stop and restart services
after changing, resetting or revoking credentials. Missing/corrupt password files
never fall back to firmware account authentication.

### FTP as the current user

```sh
python3 tools/set-password.py /path/to/reader/system/share/easy-connect/state --service ftp
```

Leave `ftp_user=current` (also the default). It resolves to the account launching
Easy Connect, with no account switching or privileged operations. Existing
configurations with `ftp_user=reader` work if that is the launching account;
otherwise change them to `current`. The account must have a non-root UID.
The default `ftp_auth=password` uses the account shown on the device screen and
the separate FTP password. Set it in **FTP → Password → Set password** or with
the host tool above. The default control port is **2121**; passive data ports are
**50000–50009**. Active-mode FTP is disabled.
Plain FTP sends passwords and file contents unencrypted; use it on a trusted LAN.

**FTP → No password** saves `ftp_auth=anonymous`. Clients log in as `anonymous`
(or `ftp`) without an application password. No password file is required. This
mode explicitly permits listing, downloads, uploads, rename, deletion and directory
creation under the same OS account permissions. Anyone who can reach the FTP port
can use these operations; choose this mode only when that access is intended.
Switching back to Password retains the existing password verifier.

In normal-user mode, `ftp_root`, initially `/mnt/ext1`, is the **starting
directory, not a jail**. FTP clients can leave it and access any file readable or
writable by that account, just as they can through SSH. Files remain subject to
the OS account's permissions. Upload, download, rename, delete and directory
creation are supported. Use this mode for a trusted owner of the reader.

The rootless adapter enables authenticated local login in vsftpd's existing
single-process mode. It validates the launching UID and the separate Easy Connect
password, without PAM, system passwords, setuid or chroot. Named login is disabled
in anonymous mode, and anonymous login is disabled in password mode.
The upstream post-login seccomp policy cannot support this pre-login
password verification, so rootless sessions do not use that filter. Upstream
vsftpd 3.0.5 has no ARM seccomp filter in any case.

If Easy Connect is explicitly launched as root, set `ftp_user` to an existing
non-root account. That optional mode retains vsftpd's two-process model and
chroot confinement, including writable chroot support for reader storage; it is
not covered by the rootless transfer tests.

## Command-line usage

```sh
easy-connect --root /absolute/application/folder doctor
easy-connect --root /absolute/application/folder serve ssh
easy-connect --root /absolute/application/folder serve ftp
easy-connect --root /absolute/application/folder serve both
easy-connect --root /absolute/application/folder fingerprint
```

`serve` stays in the foreground; Ctrl-C stops it. Starting `both` is transactional:
if either service fails, both stop. Only one controller instance per UID may run.
`doctor` reports UID/GID, account/shell, network addresses, PTY availability,
storage information, daemon presence, FTP account and confinement mode. The device screen
can export `diagnostics.txt`; the launcher records startup output in `ui.log`.
Service logs live in the reported private `/tmp/easy-connect-UID/session.*/`
directory. They survive service shutdown for diagnosis and disappear on reboot.
Logs currently have no size cap; prolonged debug use needs free-space monitoring.

On Linux, each daemon has its own supervisor and child-subreaper to adopt and
terminate its session descendants independently on normal shutdown. Supervisors
and listeners receive parent-death signals. Abrupt process/kernel failure is not
a guarantee of session cleanup.
On macOS only process-group cleanup is available; detached interactive session
cleanup is a Linux/device acceptance test.

## Credential storage

Runtime copies require an owned Unix directory with mode 0700 and files with
mode 0600. A small Dropbear patch stops ancestor permission checks at the explicitly
selected, validated private key directory; file/owner/mode checks remain enabled.
This lets the runtime live under `/tmp` without broadly disabling key checks.

Persistent `state/` may be on FAT storage, where Unix permissions do not provide
protection. Anyone with USB access or the same device account may read/change
the stored server key and password verifier. Keep the FTP shared root away from
credentials if access should be narrower than full reader administration.
`state_dir` may point to an existing suitable persistent directory discovered
during device diagnostics. This version does not encrypt secrets at rest.

## Next device milestones

1. Run diagnostics under a normal app launch on PB632 firmware 6.11.2001.
2. Build with the B288 SDK and check firmware ELF symbol/library compatibility.
3. Verify SSH commands, PTY, both login methods and fingerprint persistence.
4. Exercise Linux session cleanup, Wi-Fi loss, background, sleep/wake and USB mode.
5. Verify current-user FTP transfers and permissions on the reader; test the
   optional privileged/chroot mode separately.
6. Add on-device public-key import, network lifecycle controls and bounded logs.

## Sources and license

New project code: GPL-2.0-or-later; see `LICENSE`. Dropbear retains its upstream
license notices; vsftpd is GPL v2. Build adapters are in `tools/dependencies.py`
and `adapters/password.h`; source archive versions/hashes are in `dependencies.json`.
The application-specific password adapter keeps each daemon's protocol handling
and the controller restricts SSH to the launching UID.

- [Dropbear upstream](https://github.com/mkj/dropbear)
- [vsftpd upstream](https://security.appspot.com/vsftpd.html)
- [vsftpd rootless restrictions](https://security.appspot.com/vsftpd/vsftpd_conf.html)
