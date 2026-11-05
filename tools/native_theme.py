"""Inspect native caption resources without distributing firmware theme assets."""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import zlib

RESOURCE_NAMES = ('home', 'refresh', 'ci_diagnostic', 'ci_search_for_updates', 'info')


def unpack(blob, size, offset, packed):
    if size <= 0 or size > 16 * 1024 * 1024 or offset < 32 or offset + packed > len(blob):
        raise ValueError('Invalid theme resource bounds')
    content = zlib.decompress(blob[offset:offset + packed])
    if len(content) != size:
        raise ValueError('Invalid theme resource size')
    return content


def read_theme(path):
    blob = Path(path).read_bytes()
    if len(blob) < 32 or not blob.startswith(b'PocketBookTheme\x01'):
        raise ValueError('Unsupported PocketBook theme')
    config = unpack(blob, *struct.unpack_from('<III', blob, 20))
    values = {}
    for line in config.decode('utf-8-sig').splitlines():
        line = line.strip()
        if line and not line.startswith(('#', '//', '/*', '*')) and '=' in line:
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    caption_keys = ('application.caption.font', 'application.caption.height',
                    'application.caption.text.uppercase')
    if any(key not in values for key in caption_keys):
        raise ValueError('Missing native application caption style')
    bitmaps, metadata = {}, {}
    for name in RESOURCE_NAMES:
        start = 0
        while True:
            index = blob.find((name + ':4\0').encode(), start)
            if index < 0:
                raise ValueError('Missing native theme bitmap: ' + name)
            start = index + 1
            try:
                data = unpack(blob, *struct.unpack_from('<III', blob, index - 12))
                width, height, depth, stride = struct.unpack_from('<HHHH', data)
                if not width or not height or depth != 8 or stride < width or len(data) != 8 + height * stride:
                    raise ValueError('Invalid native bitmap')
            except (ValueError, struct.error, zlib.error):
                continue
            bitmaps[name] = data
            metadata[name] = {'width': width, 'height': height, 'depth': depth,
                              'sha256': hashlib.sha256(data).hexdigest()}
            break
    return {'theme_sha256': hashlib.sha256(blob).hexdigest(),
            'caption': {key: values[key] for key in caption_keys},
            'resources': metadata}, config, bitmaps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('theme', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    metadata, config, bitmaps = read_theme(args.theme)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'theme.cfg').write_bytes(config)
        for name, data in bitmaps.items():
            (args.output / (name + '.bitmap')).write_bytes(data)
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
