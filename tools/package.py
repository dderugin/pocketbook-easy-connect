#!/usr/bin/env python3
"""Build a separate ZIP; never include device credentials or generated host keys."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
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
    manifest = {'application':'Easy Connect','version':'0.1.0',
                'target':'PocketBook PB632 / U632.6.11.2001',
                'device_validation':'not-performed',
                'sdk':json.loads((ROOT / 'sdk.json').read_text()),
                'binaries':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in binaries.items()},
                'dependencies':json.loads((ROOT / 'dependencies.json').read_text())}
    (share / 'build-info.json').write_text(json.dumps(manifest,indent=2)+'\n')
    dest = ROOT / 'dist/easy-connect-pb632-0.1.0.zip'
    dest.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(dest,'w',zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                archive.write(path,path.relative_to(stage))
    dest.with_suffix('.zip.sha256').write_text(hashlib.sha256(dest.read_bytes()).hexdigest()+'  '+dest.name+'\n')
    print(dest)

if __name__ == '__main__':
    main()
