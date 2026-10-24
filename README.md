# Easy Connect for PocketBook

Standalone SSH and FTP communication app for **PocketBook Touch HD 3 / PB632,
firmware U632.6.11.2001**. Independent of CoolReader; it reuses the same
B288 / Cortex-A7 softfp SDK target and USB installation layout.

[Download](https://pocketbook.nimbushub.eu/easy-connect.html) · [Installation and connection guide](https://pocketbook.nimbushub.eu/easy-connect.html#installation) · [GitHub repository](https://github.com/dderugin/pocketbook-easy-connect)

**Development release with limited physical-device testing on PB632.**

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

The Linux ARM build uses the real B288 SDK. On a physical PB632, the app runs
as `reader` (UID 101); anonymous FTP access and SSH commands/basic shell access
have been confirmed. Firmware denied PTY allocation, so the SSH hint opens a
shell over pipes. Full device acceptance testing, the latest UI corrections,
and other models/firmware remain unverified. Host integration tests cover SSH
and FTP transfers as a normal user. No root or jailbreak is needed for FTP.

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

Use a **Linux x86_64** build host and SDK-B288-6.10. The SDK is pinned separately
in [sdk.json](sdk.json), including its URL and SHA-256. It comes from the
[Sean-on-Git community mirror](https://github.com/Sean-on-Git/PocketBook-SDK/releases/tag/6.10),
which is also cited by [KOReader's InkView collection](https://github.com/koreader/pocketbook-inkview).
This is not an official successor repository. SDK 6.11 from that mirror targets
RK3566 and is not the default for this B288 build.

On Ubuntu 24.04:

```sh
sudo apt-get install build-essential cmake python3 libarchive-tools

# Explicit network steps; a cached SDK is always checked again before use.
python3 tools/sdk.py fetch
python3 tools/dependencies.py fetch
bsdtar -xf work/sdk/SDK-B288-6.10.7z -C work/sdk
bash tools/build-device.sh
```

The SDK download is about 613 MiB. Downloads stream to a temporary file and enter
the cache only after checksum verification. `python3 tools/sdk.py verify` checks
an existing download offline. Keep SDK symlinks intact when extracting; use
`bsdtar` as above. The build defaults to `work/sdk/SDK-B288-6.10`; set
`POCKETBOOK_SDK=/absolute/path/to/SDK-B288-6.10` to use an existing installation.
Build commands never download the SDK automatically. The SDK's Linux binaries
cannot run natively on macOS; use Linux or the GitHub Actions ARM build.

The controller and daemon adapters link the SDK's ARM OpenSSL crypto archive
statically (OpenSSL 3.4.1, with its license notice in the companion archive). InkView headers and
its link library are located inside the SDK sysroot; the reader supplies InkView at runtime. The build rejects an unexpected
shared OpenSSL dependency and writes ELF headers, interpreter, dependencies,
ARM attributes and symbol versions to `build-device/arm-report.txt`.

The build creates `dist/easy-connect-pb632-0.1.0.zip` and its SHA-256 checksum.
The **B288 ARM build** workflow performs the same build and uploads a development
artifact. The installation ZIP contains only seven files: the application launcher,
five ARM executables, and `easy-connect.conf.default`. A separate
`easy-connect-pb632-0.1.0-sources.zip` contains licenses, complete application/daemon
sources, pinned daemon archives, build checksums and SDK metadata. Keep this
companion archive on the computer; it does not need to be installed on the reader.
The SDK archive itself is excluded from both ZIPs.

Compare `arm-report.txt` with the actual firmware before installation. A successful
cross build does not establish firmware compatibility or physical-reader support.

### Local Docker build (including macOS)

Docker runs the required Linux x86_64 toolchain, including on Apple Silicon:

```sh
docker build --platform linux/amd64 -f tools/Dockerfile -t easy-connect-builder .
docker run --rm --platform linux/amd64 -v "$PWD:/project" easy-connect-builder
```

The first run downloads and verifies the pinned SDK (about 613 MiB) and daemon
sources. Later runs reuse the `work/` cache. The ZIP and checksum are written to
`dist/`, and the ARM compatibility report to `build-device/arm-report.txt`.

## Install and configure

For an upgrade, press **Exit** in Easy Connect before connecting USB and replacing
its files. Home and the task switcher can leave the existing process running;
reopening it does not guarantee that the new executable has loaded. If the app
was running during an update or is unresponsive, safely eject USB and fully
power the reader off and on before testing the installed build.

1. Extract/merge the ZIP into the reader's USB storage root.
2. Factory configuration is included as `system/share/easy-connect/easy-connect.conf.default`.
   The app reads it when no saved `easy-connect.conf` exists. Saved settings always
   take precedence; changing a setting in the UI writes `easy-connect.conf`.
3. SSH and FTP initially use separate application passwords. Press **Start** in
   each tab to set its password, then press **Start** again to run the service.
   For SSH key access, select **Public key** and import your `.pub` key over USB.
4. Safely eject USB, connect the reader to Wi-Fi using its settings, then open
   **Applications → EasyConnect**.
5. Start the desired service. Closing or backgrounding the app stops services.

The ZIP includes factory configuration only, with no shared password or anonymous
access enabled. It never includes saved `easy-connect.conf`, credentials or host
keys, so upgrades preserve them. The app creates state and daemon configuration
files as needed; missing credentials prompt for setup instead of being shipped as
empty placeholder files. It does not modify CoolReader files or firmware system accounts.
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

The interface checks the device IP address every second. While no address is
available, service buttons and settings are disabled and a Wi-Fi connection
hint is shown. Controls enable automatically when an address appears; no manual
refresh is required. Tabs, Refresh Library, Exit and Save diagnostics remain available offline.

Changes save immediately and survive app restarts. Configuration for a running
daemon is locked; stop that daemon before editing it. Stopping SSH leaves FTP
running, and stopping FTP leaves SSH running. Keyboard overlays do not stop the
other daemon. Closing or backgrounding the whole app stops both, except for the
firmware scanner's temporary screen takeover during Refresh Library.

Each password is entered twice with masked input and saved as a salted verifier.
Existing passwords are never displayed. Public key import is currently via USB;
the UI reports the file location and entry count.

### Refresh the PocketBook Library

After copying, renaming or removing books over FTP/SSH, tap **Refresh Library** in
either tab. SSH/FTP can remain running, including active transfers. The action
also works without Wi-Fi. It asks the firmware's `/ebrmain/bin/scanner.app` to rescan;
the firmware may display a scanning screen. Open Library after it finishes.

While a refresh runs, the button shows **Refreshing...**. Services can still be
started or stopped. Existing connections remain active while the firmware scanner
temporarily takes the screen, including until Easy Connect returns to the foreground.
Files still being uploaded may need another refresh after the upload completes.
A background supervisor watches for the scanner's completion message,
then dismisses only the scanner instance it started. It never kills the system's
persistent scanner. A two-minute timeout and app-exit cleanup prevent a stuck
temporary scanner from remaining open. The app reports failures and keeps
`system/share/easy-connect/library-scan.log` for diagnostics. Missing firmware
scanner support is reported on screen. Refreshing does not reset the Library
database or erase reading history. This firmware integration still needs
physical PB632 acceptance testing.

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
ssh -T -p 2222 -i ~/.ssh/pocketbook DEVICE_USER@DEVICE_IP '/bin/sh -i'
```

Compare the server fingerprint with the device screen before accepting it.
SSH sessions have the launching account's permissions; ordinary launches do not
provide root access. Firmware may deny PTY allocation to the normal reader account.
The connection hint uses `ssh -T` to skip PTY allocation and `/bin/sh -i` to show
a shell prompt over ordinary pipes. The shell may print "can't access tty; job
control turned off"; basic commands still work. Terminal applications and job
control require PTY access. Physical-reader verification remains pending. SFTP and SCP support are not included yet.

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
macOS Finder mounts FTP connections read-only. Use an FTP client with upload
support to create, upload, rename or delete files.

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

## TODO

### SDK, dependencies and release build

- [x] Pin the B288 6.10 SDK archive and checksum, with an explicit verified download.
- [x] Add a Linux ARM build/package job using real InkView headers and libraries.
- [ ] Record the exact firmware versions used for validation. Verify the B288/softfp
  configuration before deciding whether separate platform profiles are needed.
- [ ] Check each executable's ELF interpreter, CPU/float ABI, required libraries
  and symbol versions against the target firmware, including libc and InkView.
- [x] Link the SDK's static ARM `libcrypto` into the controller and daemon adapters;
  reject shared OpenSSL dependencies during the device build.
- [ ] Centralize platform flags shared by CMake and dependency builds. Replace
  hardcoded PB632 release names/metadata with platform profiles and a documented
  list of validated model/firmware combinations.

### Verse Pro and InkPad 4

Neither model has been validated with Easy Connect. Matching screen dimensions
in host tests does not establish firmware or binary compatibility.

| Model | Portrait resolution | Remaining UI validation |
| --- | --- | --- |
| Verse Pro (standard, non-Color) | 1072 × 1448 | Verify rendering, touch and keyboard on hardware; exact dimensions already appear in host scaling tests |
| InkPad 4 | 1404 × 1872 | Add an exact-resolution host regression, then verify rendering, touch and keyboard on hardware |

- [ ] Collect each model's exact firmware version and diagnostics from a normal
  app launch; establish the launch account, permissions, storage paths and PTY
  availability. Keep PB632 firmware U632.6.11.2001 as a baseline validation target.
- [ ] Check portrait layout and touch alignment on both models. Validate rotation
  behavior and decide whether to lock portrait orientation or support reflow.
- [ ] Verify SSH commands, interactive PTY shells, key/password/either access,
  credential rotation and host-key/fingerprint persistence across app restarts.
- [ ] Verify password and anonymous FTP, passive transfers, file ownership and
  current-user permissions. Test optional privileged/chroot mode separately.
- [ ] Verify kernel supervision support and session cleanup: independent daemon
  Stop, partial-start rollback, background/exit, and unexpected process death.
- [ ] Exercise Wi-Fi loss/reconnection, sleep/wake, cover events and USB mode;
  confirm the expected service state and cleanup after each transition.
- [ ] Record results per model and firmware, including logs and unresolved issues,
  before marking a release as supported on that combination.

### Follow-up features

- [ ] Add on-device SSH public-key import.
- [ ] Add network lifecycle controls based on the device validation results.
- [ ] Bound service log size and retention.

## Sources and license

New project code: GPL-2.0-or-later; see `LICENSE`. Dropbear retains its upstream
license notices; vsftpd is GPL v2. Build adapters are in `tools/dependencies.py`
and `adapters/password.h`; source archive versions/hashes are in `dependencies.json`.
The application-specific password adapter keeps each daemon's protocol handling
and the controller restricts SSH to the launching UID.

- [Dropbear upstream](https://github.com/mkj/dropbear)
- [vsftpd upstream](https://security.appspot.com/vsftpd.html)
- [vsftpd rootless restrictions](https://security.appspot.com/vsftpd/vsftpd_conf.html)
