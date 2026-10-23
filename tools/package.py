#!/usr/bin/env python3
"""Build a minimal installation ZIP and a separate source/notice archive."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    binaries = {'easy-connect': args.build / 'easy-connect',
                'easy-connect.app': args.build / 'easy-connect.app'}
    for name in ('dropbear', 'dropbearkey', 'vsftpd'):
        binaries[name] = ROOT / 'work/stage/bin' / name
    for name, path in binaries.items():
        data = path.read_bytes()
        if data[:6] != b'\x7fELF\x01\x01' or len(data)<52 or struct.unpack_from('<H', data, 18)[0] != 40:
            raise RuntimeError(f'{name} is not a 32-bit little-endian ARM ELF binary')
    stage = ROOT / 'work/package'
    if stage.exists():
        shutil.rmtree(stage)
    share = stage / 'system/share/easy-connect'
    (share / 'bin').mkdir(parents=True)
    (stage / 'applications').mkdir()
    for name, path in binaries.items():
        shutil.copy2(path, share / 'bin' / name)
        (share / 'bin' / name).chmod(0o755)
    shutil.copy2(ROOT / 'data/EasyConnect.app', stage / 'applications/EasyConnect.app')
    (stage / 'applications/EasyConnect.app').chmod(0o755)
    shutil.copy2(ROOT / 'data/easy-connect.conf.default', share)
    shutil.copy2(ROOT / 'data/easy-connect.conf.example', share)
    shutil.copy2(ROOT / 'README.md', share)
    shutil.copy2(ROOT / 'LICENSE', share)
    shutil.copy2(ROOT / 'dependencies.json', share)
    shutil.copytree(ROOT / 'work/stage/licenses', share / 'licenses')
    shutil.copytree(ROOT / 'data/licenses', share / 'licenses/openssl')
    # Distribute complete corresponding source, including upstream archives.
    source = share / 'source'
    source.mkdir()
    for directory in ('src', 'adapters', 'tools', 'data', 'tests'):
        shutil.copytree(ROOT / directory, source / directory,
                        ignore=shutil.ignore_patterns('__pycache__'))
    for name in ('CMakeLists.txt', 'dependencies.json', 'sdk.json', 'LICENSE', 'README.md'):
        shutil.copy2(ROOT / name, source / name)
    downloads = source / 'work/downloads'
    downloads.mkdir(parents=True)
    for dependency in json.loads((ROOT / 'dependencies.json').read_text()).values():
        name = dependency['url'].rsplit('/', 1)[-1]
        archive = ROOT / 'work/downloads' / name
        if hashlib.sha256(archive.read_bytes()).hexdigest() != dependency['sha256']:
            raise RuntimeError(f'Upstream source checksum mismatch: {name}')
        shutil.copy2(archive, downloads / name)
    source_commit = source_dirty = None
    if shutil.which('git'):
        revision = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
                                  capture_output=True, text=True)
        source_commit = revision.stdout.strip() if revision.returncode == 0 else None
        status = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain'],
                                capture_output=True, text=True)
        source_dirty = bool(status.stdout.strip()) if status.returncode == 0 else None
    manifest = {'source_commit': source_commit, 'source_dirty': source_dirty, 'application':'Easy Connect','version':'0.1.0',
                'target':'PocketBook PB632 / U632.6.11.2001',
                'device_validation':'not-performed',
                'sdk':json.loads((ROOT / 'sdk.json').read_text()),
                'binaries':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in binaries.items()},
                'dependencies':json.loads((ROOT / 'dependencies.json').read_text())}
    (share / 'build-info.json').write_text(json.dumps(manifest,indent=2)+'\n')
    dest = ROOT / 'dist/easy-connect-pb632-0.1.0.zip'
    dest.parent.mkdir(exist_ok=True)
    runtime = [stage / 'applications/EasyConnect.app', share / 'easy-connect.conf.default']
    runtime += [share / 'bin' / name for name in binaries]
    companion = dest.with_name(dest.stem + '-sources.zip')
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(runtime):
            archive.write(path, path.relative_to(stage))
    # Keep corresponding source, notices and build evidence off the reader.
    with zipfile.ZipFile(companion, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(share.rglob('*')):
            if path.is_file() and path not in runtime:
                archive.write(path, path.relative_to(share))
    for package in (dest, companion):
        package.with_suffix('.zip.sha256').write_text(
            hashlib.sha256(package.read_bytes()).hexdigest() + '  ' + package.name + '\n')
        print(package)

if __name__ == '__main__':
    main()
