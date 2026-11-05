#!/usr/bin/env python3
"""Render both tabs from the actual C device UI, using the host InkView stub."""
import argparse
import base64
import html
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path('build'))
    parser.add_argument('--out',type=Path,default=Path('work/ui-preview'))
    parser.add_argument('--theme-reference',type=Path,help='Local native_theme.py output; never distributed')
    parser.add_argument('--native-font',type=Path,help='Local firmware Roboto-Regular.ttf for an exact preview')
    parser.add_argument('--width',type=int,default=720)
    parser.add_argument('--height',type=int,default=960)
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
        environment=dict(os.environ,EC_UI_ROOT=str(root),EC_UI_EVENTS=str(events),
                         EC_UI_WIDTH=str(args.width),EC_UI_HEIGHT=str(args.height),EC_UI_ADDRESS='192.0.2.1')
        if args.theme_reference:
            environment['EC_UI_THEME_REFERENCE']=str(args.theme_reference.resolve())
        subprocess.run([str(binary)],env=environment,check=True,stdout=subprocess.DEVNULL)
    if args.native_font:
        encoded=base64.b64encode(args.native_font.read_bytes()).decode()
        style='<style>@font-face{font-family:Roboto;src:url(data:font/ttf;base64,'+encoded+');}</style>'
        for name in ('ssh','ftp','ftp-anonymous'):
            path=output/f'{name}.svg'
            text=path.read_text();at=text.index('>')+1
            path.write_text(text[:at]+style+text[at:])
    images=[]
    for name in ('ssh','ftp'):
        encoded=base64.b64encode((output/f'{name}.svg').read_bytes()).decode()
        images.append(f'<image x="{len(images)*(args.width+24)}" y="0" width="{args.width}" height="{args.height}" href="data:image/svg+xml;base64,{encoded}"/>')
    (output/'tabs.svg').write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="{2*args.width+24}" height="{args.height}">'
                                 f'<rect width="{2*args.width+24}" height="{args.height}" fill="#ddd"/>'+''.join(images)+'</svg>\n')
    converter=shutil.which('rsvg-convert')
    if converter:
        with tempfile.TemporaryDirectory(prefix='easy-connect-preview-fonts-') as fonts:
            environment=dict(os.environ)
            if args.native_font:
                config=Path(fonts)/'fonts.conf'
                config.write_text('<fontconfig><dir>'+html.escape(str(args.native_font.resolve().parent))+
                                  '</dir><dir>/usr/share/fonts</dir><cachedir>'+fonts+'/cache</cachedir></fontconfig>')
                environment['FONTCONFIG_FILE']=str(config)
            for name in ('ssh','ftp','ftp-anonymous','tabs'):
                subprocess.run([converter,str(output/f'{name}.svg'),'-o',str(output/f'{name}.png')],check=True,env=environment)
    print(output/'tabs.png' if converter else output/'tabs.svg')

if __name__=='__main__': main()
