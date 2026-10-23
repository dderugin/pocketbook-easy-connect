# Host test coverage

Run the portable checks with `ctest --test-dir build --output-on-failure` after
building. Build the pinned daemons with `python3 tools/dependencies.py build`,
then run the three integration commands in the main README. Run integration
tests as a normal user. Every network connection targets loopback; keys,
passwords, files and application roots are disposable fixtures.

| Suite | Behavior covered |
| --- | --- |
| `test_controller.py` | CLI usage and errors, defaults, all configuration fields, comments/CRLF, duplicate/malformed settings, boundary and overlapping ports, current-user resolution, safe diagnostics, host password provisioning |
| `test_core.py` + `core_driver.c` | Real C code: private directories, binary file staging and limits, symlink/FIFO rejection, password lengths and UTF-8, random salts, service separation, atomic replacement/failure cleanup, verifier formats/permissions, key counting, configuration round trips, fingerprint subprocess output and error reporting |
| `test_ui.py` + InkView stub | Actual C drawing and callbacks: SSH/FTP tabs, access options, saved/reopened settings, invalid edits and write failures, advanced-setting preservation, password masking/confirmation/cancel, diagnostics and startup errors |
| `test_ui_layout.c` | Shared control geometry: named controls, bounds, visibility, gaps, non-overlap, and half-open hit regions for every SSH/FTP access mode; UI tests also exercise scaled screens |
| `test_ssh.py` | Real OpenSSH/Dropbear: forced ChaCha20-Poly1305 and AES-CTR ciphers with key/password auth; key/password/either access and rejection, unknown account, Unicode passwords, required credentials, host-key persistence and symlink rejection, fingerprint independently checked by OpenSSH, revocation/reset on restart, duplicate start, occupied ports, TIME_WAIT reuse, active-session termination |
| `test_ftp.py` | Real vsftpd/ftplib: named/anonymous authentication and rejection, passive port range, uploads/downloads/listing/rename/delete/directories, Unicode filenames/passwords, ownership and file permissions, starting directory without chroot, invalid credential/folder sources, disabled active mode, session close and restart |
| `test_sessions.py` | Actual UI callbacks with both real daemons: independent stop, running-setting locks, password setup, failed-start recovery, background/hide/exit cleanup, repeat button actions, partial `serve both` rollback, listener crash/status refresh, abrupt parent death and active-session cleanup |
| `test_tools.py` | Offline SDK streaming download, cache/checksum verification and interrupted-download cleanup; dependency cache/checksum and safe archive extraction, exact patch anchors, minimal install layout and factory configuration, saved-settings preservation, executable modes, all five ARM-header requirements, missing licenses/binaries, separate source/notice archive, manifest/checksum correctness, credential exclusion and stale staging cleanup |

The core driver and InkView stub are host-test executables, excluded from the
device build. Release fixtures use synthetic ELF headers to check packaging
policy; they do not prove that a binary runs on ARM. Dependency tests mock only
downloads and use small local archives; real dependency builds and daemon tests
run separately in CI.

Linux CI runs all suites, including process ancestry and session cleanup tests.
macOS skips Linux-only supervision checks. The separate B288 ARM build workflow
uses the pinned SDK to compile/package all five executables and records their
ELF runtime requirements. It does not execute them on a reader. These suites do
not validate firmware library compatibility, physical InkView rendering,
Wi-Fi/suspend/USB behavior, or device-specific privilege behavior. Those still
require the SDK and a PocketBook reader.
