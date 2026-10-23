#!/usr/bin/env python3
"""Fetch checksum-pinned sources explicitly, or build from the offline cache."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tarfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent
LOCK = json.loads((ROOT / 'dependencies.json').read_text())

def checked_replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f'Upstream patch anchor changed: {path.name}')
    path.write_text(text.replace(old, new))

def prepare(name):
    dependency = LOCK[name]
    archive = ROOT / 'work/downloads' / dependency['url'].rsplit('/', 1)[-1]
    if not archive.exists():
        raise RuntimeError('Missing cached sources; run tools/dependencies.py fetch first')
    if hashlib.sha256(archive.read_bytes()).hexdigest() != dependency['sha256']:
        raise RuntimeError(f'Checksum mismatch: {archive.name}')
    destination = ROOT / 'work' / 'dependencies' / name
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            parts = Path(member.name).parts
            # These upstream archives need only regular files/directories.
            if member.issym() or member.islnk() or not (member.isfile() or member.isdir()) or '..' in parts or member.name.startswith('/'):
                raise RuntimeError(f'Unsafe source archive entry: {member.name}')
        package.extractall(destination)
    source, = [p for p in destination.iterdir() if p.is_dir()]
    shutil.copy2(ROOT / 'adapters/password.h', source / 'ec-password.h')
    return source

def build(name, sdk, prefix):
    if name == 'vsftpd' and not sdk and os.uname().sysname not in ('Linux', 'Darwin'):
        raise RuntimeError('vsftpd development builds support Linux and macOS')
    source = prepare(name)
    env = os.environ.copy()
    cppflags = env.get('CPPFLAGS', '')
    ldflags = env.get('LDFLAGS', '')
    cflags = '-O2 -g'
    crypto_libraries = '-lcrypto'
    configure = []
    if sdk:
        if os.uname().sysname != 'Linux':
            raise RuntimeError('The PocketBook SDK requires a Linux build host')
        sdk = sdk.resolve()
        cc = sdk / 'usr/bin/arm-obreey-linux-gnueabi-gcc'
        sysroot = sdk / 'usr/arm-obreey-linux-gnueabi/sysroot'
        if not cc.is_file():
            raise RuntimeError(f'Missing SDK compiler: {cc}')
        crypto = sysroot / 'usr/lib/libcrypto.a'
        if not crypto.is_file():
            raise RuntimeError(f'Missing SDK static OpenSSL library: {crypto}')
        crypto_libraries = f'{crypto} -ldl -pthread'
        env['CC'] = str(cc)
        env['AR'] = str(sdk / 'usr/bin/arm-obreey-linux-gnueabi-ar')
        env['RANLIB'] = str(sdk / 'usr/bin/arm-obreey-linux-gnueabi-ranlib')
        cflags += f' --sysroot={sysroot} -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=softfp'
        ldflags += f' --sysroot={sysroot} -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=softfp'
        configure = ['--host=arm-obreey-linux-gnueabi']
    elif prefix:
        cppflags += f' -I{prefix}/include'
        ldflags += f' -L{prefix}/lib'
    env.update(CFLAGS=cflags, CPPFLAGS=cppflags, LDFLAGS=ldflags)
    if name == 'dropbear':
        # OpenSSL's ARM assembly exports poly1305_init too. Namespace the bundled
        # LibTomCrypt declaration/definition/callers through its shared header.
        checked_replace(source / 'libtomcrypt/src/headers/tomcrypt_dropbear.h',
                        '#define LTC_NOTHING',
                        '#define LTC_NOTHING\n#define poly1305_init ec_dropbear_poly1305_init')
        checked_replace(source / 'src/svr-authpasswd.c', '#include "runopts.h"',
                        '#include "runopts.h"\n#include "../ec-password.h"')
        checked_replace(source / 'src/svr-authpasswd.c',
                        'passwdcrypt = ses.authstate.pw_passwd;\n\t\ttestcrypt = crypt(password, passwdcrypt);',
                        'passwdcrypt = "easy-connect";\n\t\ttestcrypt = ec_password_matches(password, passwordlen) ? passwdcrypt : "";')
        # Stop at the explicit -D directory, after validating its permissions.
        # Unlike broad FAT patches this retains file/directory owner/mode checks.
        checked_replace(source / 'src/svr-authpubkey.c',
                        'if (strcmp(path, ses.authstate.pw_dir) == 0 || strcmp(path, "/") == 0)',
                        'if (strcmp(path, svr_opts.authorized_keys_dir) == 0 || strcmp(path, ses.authstate.pw_dir) == 0 || strcmp(path, "/") == 0)')
        checked_replace(source / 'src/svr-authpubkey.c',
                        'int ret = DROPBEAR_SUCCESS;\n\n\tTRACE(("enter checkpubkeyperms"))',
                        'int ret = checkfileperm(path);\n\n\tTRACE(("enter checkpubkeyperms"))')
        checked_replace(source / 'src/sysoptions.h',
                        '#if DROPBEAR_SVR_PASSWORD_AUTH && !HAVE_CRYPT',
                        '#if 0 /* Easy Connect uses OpenSSL PBKDF2 instead of crypt(). */')
        # A rejected PTY request must not leave the channel marked as a PTY.
        # PocketBook firmware can deny openpty() to the normal reader account;
        # the subsequent shell/exec request must still work over ordinary pipes.
        checked_replace(source / 'src/svr-chansession.c',
                        'TRACE(("leave sessionpty: failed to allocate pty"))',
                        'm_free(chansess->term);\n\t\tTRACE(("leave sessionpty: failed to allocate pty"))')
        checked_replace(source / 'src/svr-auth.c',
                        'if (!(DROPBEAR_SVR_MULTIUSER && uid == 0) && uid != ses.authstate.pw_uid)',
                        'if (uid != ses.authstate.pw_uid)')
        (source / 'localoptions.h').write_text('''#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
#define MAX_UNAUTH_CLIENTS 3
#define MAX_AUTH_TRIES 3
''')
        if not sdk and os.uname().sysname == 'Darwin':
            with (source / 'localoptions.h').open('a') as options:
                options.write('#define DROPBEAR_SVR_DROP_PRIVS 0\n')
        env['LIBS'] = crypto_libraries
        subprocess.run([str(source / 'configure'), *configure, '--enable-bundled-libtom', '--disable-zlib', '--disable-pam',
                        '--disable-lastlog', '--disable-utmp', '--disable-utmpx',
                        '--disable-wtmp', '--disable-wtmpx'], cwd=source, env=env, check=True)
        makefile = source / 'Makefile'
        makefile.write_text(re.sub(r'(?<!\S)-lcrypt(?=\s|$)', '', makefile.read_text()))
        subprocess.run(['make', f'-j{os.cpu_count() or 2}', 'PROGRAMS=dropbear dropbearkey'], cwd=source, env=env, check=True)
        binaries = ('dropbear', 'dropbearkey')
        license_file = 'LICENSE'
    else:
        # Upstream links a PIE executable; ARM objects must use matching codegen.
        cflags += ' -fPIE'
        checked_replace(source / 'builddefs.h', '#define VSF_BUILD_PAM', '#undef VSF_BUILD_PAM')
        checked_replace(source / 'sysdeputil.c', '#include "sysdeputil.h"', '#include "sysdeputil.h"\n#include "ec-password.h"')
        # Use upstream's Linux capability syscalls, avoiding a firmware libcap dependency.
        checked_replace(source / 'sysdeputil.c', '#define VSF_SYSDEP_HAVE_LIBCAP', '#undef VSF_SYSDEP_HAVE_LIBCAP')
        text = (source / 'sysdeputil.c').read_text()
        anchor = 'vsf_sysdep_check_auth(struct mystr* p_user_str,'
        at = text.index(anchor)
        start = text.index('{', at) + 1
        end = text.index('\n}\n', start)
        # Rootless logins must name the launching account; never switch identity.
        text = text[:start] + '''
  const struct passwd* p_pwd = getpwnam(str_getbuf(p_user_str));
  (void) p_remote_host;
  if (p_pwd == NULL || p_pwd->pw_uid == 0) return 0;
  if (tunable_run_as_launching_user && p_pwd->pw_uid != geteuid()) return 0;
  return ec_password_matches(str_getbuf(p_pass_str), str_getlen(p_pass_str));''' + text[end:]
        (source / 'sysdeputil.c').write_text(text)
        checked_replace(source / 'main.c',
                        'if (tunable_one_process_model)\n  {\n    if (tunable_local_enable)',
                        'if (tunable_one_process_model && !tunable_run_as_launching_user)\n  {\n    if (tunable_local_enable)')
        checked_replace(source / 'oneprocess.c',
                        '    case kVSFLoginNull:\n      /* Fall through. */\n    case kVSFLoginReal:\n      /* Fall through. */',
                        '''    case kVSFLoginReal:
      if (tunable_run_as_launching_user && !vsf_sysutil_running_as_root())
      {
        p_sess->is_anonymous = 0;
        process_post_login(p_sess);
      }
      bug("local one-process login requires an unprivileged launching user");
      break;
    case kVSFLoginNull:
      /* Fall through. */''')
        checked_replace(source / 'oneprocess.c',
                        '        str_chdir(&chdir_str);',
                        '        if (str_chdir(&chdir_str) != 0) die("cannot enter FTP starting directory");')
        checked_replace(source / 'oneprocess.c', '#include "seccompsandbox.h"',
                        '#include "seccompsandbox.h"\n#include "defs.h"\n#include "filestr.h"')
        checked_replace(source / 'oneprocess.c', '  seccomp_sandbox_init();', '''  if (tunable_run_as_launching_user && tunable_userlist_enable)
  {
    if (!tunable_userlist_file ||
        str_fileread(&p_sess->userlist_str, tunable_userlist_file,
                     VSFTP_CONF_FILE_MAX) < 0)
      die("cannot read user list file");
  }
  seccomp_sandbox_init();''')
        # Upstream includes this Linux-only header unconditionally.
        checked_replace(source / 'sysdeputil.c', '#include <sys/prctl.h>',
                        '#ifdef __linux__\n#include <sys/prctl.h>\n#endif')
        if not sdk and os.uname().sysname == 'Darwin':
            checked_replace(source / 'sysdeputil.c', '#define VSF_SYSDEP_HAVE_UTMPX',
                            '#undef VSF_SYSDEP_HAVE_UTMPX')
            # Darwin's shared-cache address space cannot use the Linux RLIMIT_AS cap.
            checked_replace(source / 'sysutil.c', '#ifdef RLIMIT_AS',
                            '#if defined(RLIMIT_AS) && !defined(__APPLE__)')
        # Override the upstream host-probing linker helper for cross compilation.
        # The app-specific adapter removes the only crypt() calls.
        libraries = crypto_libraries
        link = ldflags + ('' if os.uname().sysname == 'Darwin' and not sdk else ' -Wl,-s')
        make = ['make', f'-j{os.cpu_count() or 2}', f'CC={env.get("CC", "cc")}',
                f'CFLAGS={cflags} {cppflags}', f'LINK={link}', f'LIBS={libraries}']
        if not sdk and os.uname().sysname == 'Darwin':
            make.append('LDFLAGS=-Wl,-pie')
        subprocess.run(make, cwd=source, env=env, check=True)
        binaries = ('vsftpd',)
        license_file = 'COPYING'
    output = ROOT / 'work/stage'
    (output / 'bin').mkdir(parents=True, exist_ok=True)
    for binary in binaries:
        shutil.copy2(source / binary, output / 'bin' / binary)
    licenses = output / 'licenses' / name
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / license_file, licenses / license_file)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('fetch', 'build'))
    parser.add_argument('--service', choices=(*LOCK, 'all'), default='all')
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--openssl-prefix', type=Path)
    args = parser.parse_args()
    for name in LOCK if args.service == 'all' else (args.service,):
        if args.action == 'fetch':
            entry = LOCK[name]
            path = ROOT / 'work/downloads' / entry['url'].rsplit('/', 1)[-1]
            path.parent.mkdir(parents=True, exist_ok=True)
            data = path.read_bytes() if path.exists() else urlopen(entry['url'], timeout=60).read()
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise RuntimeError(f'Checksum mismatch: {path.name}')
            path.write_bytes(data)
            print(f'Verified {path.name}')
        else:
            build(name, args.sdk, args.openssl_prefix)

if __name__ == '__main__':
    main()
