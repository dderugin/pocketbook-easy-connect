"""Wait for the bounded exit scanner before deleting disposable UI fixtures."""
import time


def wait_exit_scan(root, timeout=4):
    log = root / 'library-exit-scan.log'
    if not log.exists() or log.is_symlink() or not log.is_file():
        return ''
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text()
        if 'Exit library refresh finished (status=' in text:
            return text
        time.sleep(.02)
    raise AssertionError('Exit scanner did not finish: ' + log.read_text())
