#!/usr/bin/env python3
"""Set an application-specific SSH or FTP password in an Easy Connect state directory."""
import argparse
import getpass
import hashlib
import os
from pathlib import Path
import secrets
import sys
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('state', type=Path)
    parser.add_argument('--service', choices=('ssh', 'ftp'), default='ssh')
    parser.add_argument('--stdin', action='store_true', help='read one password line from stdin')
    args = parser.parse_args()
    if args.stdin:
        password = sys.stdin.readline().rstrip('\n')
    else:
        password = getpass.getpass(f'New {args.service.upper()} password: ')
        if password != getpass.getpass('Confirm password: '):
            parser.error('Passwords do not match')
    encoded = password.encode('utf-8')
    if not 12 <= len(encoded) <= 1024 or '\x00' in password or '\r' in password:
        parser.error('Password must contain 12–1024 UTF-8 bytes, without NUL or CR')
    salt = secrets.token_bytes(16)
    iterations = 210000
    digest = hashlib.pbkdf2_hmac('sha256', encoded, salt, iterations)
    args.state.mkdir(mode=0o700, parents=True, exist_ok=True)
    name = 'password.hash' if args.service == 'ssh' else 'ftp-password.hash'
    fd, temporary = tempfile.mkstemp(prefix='.password-', dir=args.state)
    try:
        with os.fdopen(fd, 'w') as output:
            output.write(f'ec1${iterations}${salt.hex()}${digest.hex()}\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, args.state / name)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f'Saved {args.state / name}; restart services to use the new password.')

if __name__ == '__main__':
    main()
