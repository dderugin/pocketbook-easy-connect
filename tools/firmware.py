"""Pinned firmware extraction, adapted from pocketbook-coolreader b51d5301.
Proprietary firmware inputs remain in local caches and never enter release ZIPs.
"""
from pathlib import Path
import gzip, hashlib, json, re, shutil, struct, subprocess, zipfile

def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def embedded_version(data):
    # Vendor fwinfo.xml embeds unescaped QML in <description>; the complete
    # document is not necessarily valid XML. Read only its version field.
    prefix = data.split(b'<description', 1)[0]
    versions = re.findall(rb'<version>\s*(U[0-9A-Za-z_.]+)\s*</version>', prefix)
    if len(versions) != 1:
        raise ValueError('Missing or ambiguous embedded firmware version')
    return versions[0].decode('ascii')


def run(command, timeout=60, env=None):
    try:
        result = subprocess.run([str(x) for x in command], capture_output=True,
                                text=True, errors='replace', timeout=timeout, env=env)
        return {'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    except subprocess.TimeoutExpired as error:
        def decode(value):
            return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
        return {'exit_code': None, 'timeout': True,
                'stdout': decode(error.stdout), 'stderr': decode(error.stderr)}


def checked(command, log):
    result = run(command, timeout=300)
    log.write_text(result['stdout'] + result['stderr'])
    if result['exit_code'] != 0:
        raise ValueError('Extraction failed; see ' + str(log))


def extract(device, archive_dir, output):
    archive = archive_dir / device['archive']['filename']
    if sha256(archive) != device['archive']['sha256']:
        raise ValueError('Firmware archive checksum mismatch')
    marker = output / 'extracted.json'
    identity = {'archive_sha256': device['archive']['sha256'], 'extractor_schema': 1}
    if marker.exists():
        if json.loads(marker.read_text()) != identity:
            raise ValueError('Output directory contains another firmware; use a new directory')
        return output / 'runtime', output / 'ebrmain/cramfs'
    raw = output / 'SWUPDATE.BIN'
    with zipfile.ZipFile(archive) as source:
        fwinfo = source.read('fwinfo.xml')
        if embedded_version(fwinfo) != device['software_version']:
            raise ValueError('Embedded firmware version mismatch')
        (output / 'fwinfo.xml').write_bytes(fwinfo)
        if not raw.exists():
            with source.open('SWUPDATE.BIN') as src, raw.open('wb') as dst:
                shutil.copyfileobj(src, dst)
    with raw.open('rb') as stream:
        header = stream.read(1024)
        model = b'PocketBook' + device['model'].encode()
        if header[:16] != b'PocketBookUpdate' or header[16:48].split(b'\0')[0] != model:
            raise ValueError('Firmware header model mismatch')
        sections = {kind: (offset, size) for kind, _, offset, size in struct.iter_unpack('<4I', header[256:])}
        for kind, name in ((0x72, 'rootfs'), (0x65, 'ebrmain')):
            offset, size = sections[kind]
            if not size or offset + 1024 + size > raw.stat().st_size:
                raise ValueError('Invalid firmware section bounds')
            image = output / (name + '.img')
            if image.exists():
                continue
            compressed = output / (name + '.gz')
            stream.seek(offset + 1024)
            with compressed.open('wb') as dst:
                remaining = size
                while remaining:
                    chunk = stream.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise ValueError('Truncated firmware section')
                    dst.write(chunk)
                    remaining -= len(chunk)
            temporary = image.with_suffix('.img.part')
            with gzip.open(compressed, 'rb') as src, temporary.open('wb') as dst:
                shutil.copyfileobj(src, dst)
            temporary.replace(image)
    runtime = output / 'runtime'
    ebrmain = output / 'ebrmain'
    runtime.mkdir(exist_ok=True)
    ebrmain.mkdir(exist_ok=True)
    checked(['debugfs', '-R', 'rdump /lib ' + str(runtime), output / 'rootfs.img'], output / 'rootfs-extract.log')
    checked(['debugfs', '-R', 'dump /cramfs.img ' + str(ebrmain / 'cramfs.img'), output / 'ebrmain.img'], output / 'ebrmain-extract.log')
    if not (ebrmain / 'cramfs.img').is_file():
        raise ValueError('Unsupported firmware layout: /cramfs.img not found')
    tree = ebrmain / 'cramfs'
    if not tree.exists():
        checked(['unsquashfs', '-no-progress', '-processors', '2', '-d', tree, ebrmain / 'cramfs.img'], output / 'squashfs-extract.log')
    shutil.copytree(tree / 'lib', runtime / 'lib', dirs_exist_ok=True, symlinks=True)
    listing = run(['debugfs', '-R', 'ls -l /lib', output / 'ebrmain.img'])['stdout']
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) < 9 or fields[1] != '120777':
            continue
        name = fields[-1]
        details = run(['debugfs', '-R', 'stat /lib/' + name, output / 'ebrmain.img'])['stdout']
        link = re.search(r'Fast link dest: "([^"]+)"', details)
        if link:
            candidate = runtime / 'lib' / Path(link[1]).name
            alias = runtime / 'lib' / name
            if candidate.is_file() and not alias.exists():
                alias.unlink(missing_ok=True)
                alias.symlink_to(candidate.name)
    marker.write_text(json.dumps(identity, indent=2) + '\n')
    return runtime, tree




def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('fetch', 'prepare'))
    parser.add_argument('--archives', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--runtime-cache', type=Path)
    args = parser.parse_args()
    args.archives = args.archives.resolve()
    if args.output: args.output = args.output.resolve()
    if args.runtime_cache: args.runtime_cache = args.runtime_cache.resolve()
    root = Path(__file__).resolve().parents[1]
    devices = json.loads((root / 'config/firmware.json').read_text())['devices']
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
    for device in devices:
        archive = args.archives / device['archive']['filename']
        if args.action == 'fetch':
            archive.parent.mkdir(parents=True, exist_ok=True)
            if not archive.exists():
                partial = archive.with_suffix('.zip.part')
                subprocess.run(['curl', '-fL', '--retry', '3', '--continue-at', '-', device['archive']['url'], '-o', str(partial)], check=True)
                if sha256(partial) != device['archive']['sha256']:
                    raise ValueError('Firmware checksum mismatch')
                partial.replace(archive)
            if sha256(archive) != device['archive']['sha256']:
                raise ValueError('Firmware checksum mismatch')
        else:
            if args.output is None:
                parser.error('--output is required for prepare')
            output = args.output / device['id']
            cached = args.runtime_cache / device['id'] if args.runtime_cache else None
            if cached and (cached / 'extracted.json').is_file():
                output = cached
            else:
                output.mkdir(parents=True, exist_ok=True)
            runtime, tree = extract(device, args.archives, output)
            if embedded_version((output / 'fwinfo.xml').read_bytes()) != device['software_version']:
                raise ValueError('Cached firmware version mismatch')
            # These stay in the private build cache. The installed app obtains
            # the active firmware theme through InkView, never bundled copies.
            theme = args.output / (device['id'] + '-native-theme.pbt')
            theme.unlink(missing_ok=True)
            checked(['debugfs', '-R', 'dump /themes/Line.pbt ' + str(theme), output / 'ebrmain.img'],
                    args.output / (device['id'] + '-theme-extract.log'))
            print(device['id'] + ': ' + str(runtime), flush=True)
            (args.output / (device['id'] + '-runtime.json')).write_text(json.dumps({
                'runtime': str(runtime), 'tree': str(tree), 'theme': str(theme), 'firmware': device['software_version'],
                'archive_sha256': device['archive']['sha256']}, indent=2) + '\n')

if __name__ == '__main__':
    main()
