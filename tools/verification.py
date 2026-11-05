"""Release admission: one identical set of binaries verified on every pinned runtime."""
from pathlib import Path
import hashlib
import json
from source_metadata import source_metadata

def binaries(root, build):
    return {'easy-connect': build / 'easy-connect', 'easy-connect.app': build / 'easy-connect.app',
            **{name: root / 'work/stage/bin' / name for name in ('dropbear', 'dropbearkey', 'vsftpd')}}

def hashes(paths):
    return {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}

def verified_report(path, root, profile, paths):
    report = json.loads(path.read_text())
    expected = {d['model'] for d in profile['devices']}
    if (report.get('status') != 'passed' or report.get('family') != profile['family'] or
        report.get('version') != profile['version'] or report.get('source') != source_metadata(root) or
        report.get('binaries') != hashes(paths) or report.get('profile') != profile or
        set(report.get('devices', {})) != expected or
        any(item.get('status') != 'passed' for item in report.get('devices', {}).values()) or
        report.get('native_tests', {}).get('status') != 'passed'):
        raise ValueError('Missing, failed or stale shared-device verification')
    return report
