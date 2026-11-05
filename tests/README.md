# Host test coverage

Run the portable checks with `ctest --test-dir build --output-on-failure` after
building. Build the pinned daemons with `python3 tools/dependencies.py build`,
then run the four integration commands in the main README. Run integration
tests as a normal user. Every network connection targets loopback; keys,
passwords, files and application roots are disposable fixtures.

| Suite | Behavior covered |
| --- | --- |
| `test_controller.py` | CLI usage and errors, defaults, all configuration fields, comments/CRLF, duplicate/malformed settings, boundary and overlapping ports, current-user resolution, safe diagnostics, host password provisioning |
| `test_core.py` + `core_driver.c` | Real C code: private directories, binary file staging and limits, symlink/FIFO rejection, password lengths and UTF-8, random salts, service separation, atomic replacement/failure cleanup, verifier formats/permissions, key counting, configuration round trips, fingerprint subprocess output and error reporting |
| `test_ui.py` + InkView stub | Actual C drawing and callbacks: SSH/FTP tabs, access options, saved/reopened settings, invalid edits and write failures, advanced-setting preservation, password masking/confirmation/cancel, diagnostics and startup errors; library refresh offline/in both tabs, background completion, repeated requests, scanner failure/timeout/exit cleanup, log safety and completion-marker boundaries |
| `test_ui_layout.c` | Shared control geometry: named controls, bounds, visibility, gaps, non-overlap, and half-open hit regions for every SSH/FTP access mode; UI tests also exercise scaled screens |
| `test_ssh.py` | Real OpenSSH/Dropbear: forced ChaCha20-Poly1305 and AES-CTR ciphers with key/password auth; key/password/either access and rejection, unknown account, Unicode passwords, required credentials, host-key persistence and symlink rejection, fingerprint independently checked by OpenSSH, revocation/reset on restart, duplicate start, occupied ports, TIME_WAIT reuse, active-session termination |
| `test_ftp.py` | Real vsftpd/ftplib: named/anonymous authentication and rejection, passive port range, uploads/downloads/listing/rename/delete/directories, Unicode filenames/passwords, ownership and file permissions, starting directory without chroot, invalid credential/folder sources, disabled active mode, session close and restart |
| `test_background.py` | Detached controller, private socket permissions and malformed requests, installation isolation, simultaneous starts, independent Stop, live FTP transfers and staged-credential cleanup; also runs with each ARM firmware runtime |
| `test_sessions.py` | Actual UI callbacks with both real daemons: independent stop, running-setting locks, password setup, failed-start recovery, background/hide/exit persistence and explicit-stop cleanup, repeat button actions, partial `serve both` rollback, listener crash/status refresh, UI restart/kill persistence and active-session cleanup; live SSH sessions and partial FTP uploads survive Home, UI closure/reopening, scanner background/completion and delayed hide events; services can start during a refresh |
| `test_tools.py` | Offline SDK streaming download, cache/checksum verification and interrupted-download cleanup; dependency cache/checksum and safe archive extraction, exact patch anchors, minimal install layout and factory configuration, saved-settings preservation, executable modes, all five ARM-header requirements, missing licenses/binaries, separate source/notice archive, manifest/checksum correctness, credential exclusion and stale staging cleanup |

The InkView stub is host-only. An ARM core driver is built only for runtime probes
and is excluded from the install ZIP. Release fixtures use synthetic ELF headers to check packaging
policy; they do not prove that a binary runs on ARM. Dependency tests mock only
downloads and use small local archives; real dependency builds and daemon tests
run separately in CI.

UI checks also cover repeated Home/task-switcher resumes without EVT_SHOW,
including focus-only returns, drawing, edits, IP polling, toolbar icon position and
daemon restart. Unrelated InkView helper children must survive background,
service and exit cleanup alongside real transfers.
Linux CI runs all suites, including process ancestry and session cleanup tests.
macOS skips Linux-only supervision checks. The shared ARM workflow runs the native suites, checks all five executable
ABIs/dependency closures against four pinned firmware runtimes, and executes
ARM core and real SSH/FTP probes under QEMU. The UI loader check reaches the
application with the firmware libraries; it does not emulate the screen or
reader's kernel policy.

`shared-device` covers model normalization, all four model profiles, alphanumeric
firmware prefixes, changed build numbers, API selection, old/malformed firmware
and unknown-model rejection. UI tests cover each model plus portrait/landscape
geometry at 1072×1448, 1264×1680 and 1404×1872.

Release admission rejects missing, stale, incomplete or failed firmware reports.
Packaging fixtures remain synthetic; they test policy, not ARM execution.
Physical InkView rendering, scanner behavior, Wi-Fi/suspend/USB transitions and
actual firmware account privileges still require a reader. Native theme metadata
is checked against each pinned firmware, without including its theme files in releases.

Exit checks cover Home, Back and firmware close, exactly one final scan, offline
exit, independent worker lifetime, overlapping exit scan logs, timeout/forced cleanup and unsafe log
rejection. A real SSH/FTP session test verifies the final scan runs while both
listeners stay available and can read the last completed upload. Reopened UI
checks cover status, locked settings, independent Stop and stopping offline. Native theme-resource selection, caption style and physical-pixel toolbar geometry
checks cover the supported portrait and landscape screen sizes.
