#!/usr/bin/env python3
"""Explicitly download or verify the pinned SDK; never download during a build."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'sdk.json').read_text())
CHUNK_SIZE = 1024 * 1024


def verify(path):
    digest = hashlib.sha256()
    with path.open('rb') as archive:
        for chunk in iter(lambda: archive.read(CHUNK_SIZE), b''):
            digest.update(chunk)
    if digest.hexdigest() != LOCK['sha256']:
        raise RuntimeError(f'SDK checksum mismatch: {path}')
    return path


def fetch(path):
    if path.exists():
        return verify(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Stream the large SDK to disk and publish it only after verification.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.sdk-', delete=False) as output:
            temporary = Path(output.name)
            with urlopen(LOCK['url'], timeout=60) as response:
                while chunk := response.read(CHUNK_SIZE):
                    output.write(chunk)
        verify(temporary)
        temporary.replace(path)
        return path
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('fetch', 'verify'))
    parser.add_argument('--archive', type=Path,
                        help='Override the default work/sdk download location')
    args = parser.parse_args()
    archive = args.archive or ROOT / 'work/sdk' / LOCK['url'].rsplit('/', 1)[-1]
    result = fetch(archive) if args.action == 'fetch' else verify(archive)
    print(f'Verified {result}')


if __name__ == '__main__':
    main()
