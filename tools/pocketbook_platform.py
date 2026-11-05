"""One shared ARM build profile and generated C device table."""
from pathlib import Path
import argparse
import json
import re
import shlex

ROOT = Path(__file__).resolve().parents[1]


def load_profile(root=ROOT):
    profile = json.loads((root / 'config/pocketbook.json').read_text())
    if profile.get('schema') != 1 or profile.get('family') != 'pocketbook-armv7-softfp':
        raise ValueError('Unsupported PocketBook platform profile')
    if not re.fullmatch(r'\d+\.\d+\.\d+', profile['version']):
        raise ValueError('Invalid release version')
    models = set()
    for device in profile['devices']:
        if device['model'] in models or not re.fullmatch(r'[0-9]+[A-Z0-9]*', device['model']):
            raise ValueError('Invalid or duplicate model')
        if device['minimum_api'] not in (610, 611):
            raise ValueError('Invalid minimum API level')
        models.add(device['model'])
    if not models:
        raise ValueError('Empty device registry')
    return profile


def header(profile):
    rows = '\n'.join('    {%s, %s, %d},' % (json.dumps(d['model']), json.dumps(d['name']), d['minimum_api'])
                     for d in profile['devices'])
    return ('/* Generated from config/pocketbook.json; do not edit. */\n'
            '#define EC_VERSION ' + json.dumps(profile['version']) + '\n'
            '#define EC_FAMILY ' + json.dumps(profile['family']) + '\n'
            'static const ec_device_profile ec_devices[] = {\n' + rows + '\n};\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cflags', action='store_true')
    parser.add_argument('--header', type=Path)
    args = parser.parse_args()
    profile = load_profile()
    if args.cflags:
        print(shlex.join(profile['cpu_flags']))
    if args.header:
        text = header(profile)
        args.header.parent.mkdir(parents=True, exist_ok=True)
        if not args.header.exists() or args.header.read_text() != text:
            args.header.write_text(text)
