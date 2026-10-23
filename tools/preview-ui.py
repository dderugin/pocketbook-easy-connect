#!/usr/bin/env python3
"""Render both tabs from the actual C device UI, using the host InkView stub."""
import argparse
import base64
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path('build'))
    parser.add_argument('--out',type=Path,default=Path('work/ui-preview'))
    args=parser.parse_args()
    binary=(args.build/'ec-ui-test').resolve()
    output=args.out.resolve()
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='easy-connect-preview-') as temporary:
        root=Path(temporary)
        (root/'easy-connect.conf').write_text('auth=both\nstate_dir=/mnt/ext1/system/share/easy-connect/state\n')
        events=root/'events.txt'
        events.write_text(f'snapshot {output}/ssh.svg\npress ftp-tab\nsnapshot {output}/ftp.svg\n'
                          f'press ftp-anonymous\nsnapshot {output}/ftp-anonymous.svg\n')
        subprocess.run([str(binary)],env=dict(os.environ,EC_UI_ROOT=str(root),EC_UI_EVENTS=str(events)),check=True,
                       stdout=subprocess.DEVNULL)
    images=[]
    for name in ('ssh','ftp'):
        encoded=base64.b64encode((output/f'{name}.svg').read_bytes()).decode()
        images.append(f'<image x="{len(images)*744}" y="0" width="720" height="960" href="data:image/svg+xml;base64,{encoded}"/>')
    (output/'tabs.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="1464" height="960">'
                                 '<rect width="1464" height="960" fill="#ddd"/>'+''.join(images)+'</svg>\n')
    converter=shutil.which('rsvg-convert')
    if converter:
        for name in ('ssh','ftp','ftp-anonymous','tabs'):
            subprocess.run([converter,str(output/f'{name}.svg'),'-o',str(output/f'{name}.png')],check=True)
    print(output/'tabs.png' if converter else output/'tabs.svg')

if __name__=='__main__': main()
