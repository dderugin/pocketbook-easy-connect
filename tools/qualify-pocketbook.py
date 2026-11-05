#!/usr/bin/env python3
"""Check all five ARM executables and run isolated service/core probes on four firmware runtimes."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from firmware import run
from pocketbook_platform import ROOT, load_profile
from runtime_compatibility import check_runtime, elf_identity
from source_metadata import source_metadata
from verification import binaries, hashes
from native_theme import read_theme


def emulated_command(qemu, runtime, binary, arguments=(), environment=()):
    command = [qemu['path'], '-cpu', 'cortex-a7', '-L', runtime,
               '-E', 'LD_LIBRARY_PATH=' + str(runtime / 'lib'),
               '-E', 'GLIBC_TUNABLES=glibc.rtld.execstack=2']
    for name, value in environment:
        command.extend(['-E', name + '=' + value])
    # The pinned binfmt image defaults to the kernel's preserve-argv0 calling
    # convention. Stock QEMU uses its normal direct-invocation convention.
    command.extend([binary] * (2 if qemu['preserve_argv0'] else 1))
    return command + list(arguments)


def wrapper(path, binary, runtime, qemu):
    command = emulated_command(qemu, runtime, binary)
    path.write_text('#!/bin/sh\nexec ' + shlex.join(str(x) for x in command) + ' "$@"\n')
    path.chmod(0o755)


def functional_probes(device, paths, build, host_build, runtime, qemu, output):
    cases = {'core': ['test_private_directory_creation_and_reuse',
                       'test_private_copy_rejects_symlinks_without_changing_victim',
                       'test_password_roundtrip_matches_independent_pbkdf2',
                       'test_password_salts_and_service_separation',
                       'test_config_roundtrip_preserves_all_values_and_private_mode'],
             'ssh': ['test_key_only_and_password_rejection', 'test_password_only_and_key_rejection'],
             'ftp': ['test_roundtrip_and_file_owner', 'test_wrong_password_and_username_rejected',
                     'test_anonymous_mode_needs_no_password_file'],
             'background': ['test_detached_controller_and_independent_explicit_stop']}
    passed = []
    with tempfile.TemporaryDirectory(prefix='ec-firmware-probes-') as temporary:
        context = Path(temporary)
        (context / 'tools').mkdir()
        shutil.copy2(ROOT / 'tools/set-password.py', context / 'tools/set-password.py')
        staged = context / 'work/stage/bin'
        staged.mkdir(parents=True)
        for name in ('dropbear', 'dropbearkey', 'vsftpd'):
            wrapper(staged / name, paths[name], runtime, qemu)
        core = context / 'core-driver'
        wrapper(core, build / 'ec-runtime-test', runtime, qemu)
        controller = context / 'controller-driver'
        wrapper(controller, paths['easy-connect'], runtime, qemu)
        # Native controller harness supervises the actual ARM daemons through QEMU.
        # The separate ARM core probes exercise filesystem and password operations.
        for kind, selected in cases.items():
            filename = ROOT / 'tests' / ('test_' + kind + '.py')
            spec = importlib.util.spec_from_file_location('ec_probe_' + device + '_' + kind, filename)
            module = importlib.util.module_from_spec(spec)
            arguments = sys.argv
            try:
                sys.argv = [str(filename), str(core if kind == 'core' else host_build / 'easy-connect')]
                spec.loader.exec_module(module)
            finally:
                sys.argv = arguments
            if kind != 'core': module.ROOT = context
            if kind == 'background': module.SERVER = controller
            cls = getattr(module, {'core': 'CoreTests', 'ssh': 'SSHTests', 'ftp': 'FTPTests', 'background': 'BackgroundTests'}[kind])
            stream = io.StringIO()
            result = unittest.TextTestRunner(stream=stream, verbosity=2).run(unittest.TestSuite(cls(name) for name in selected))
            (output / (kind + '-tests.log')).write_text(stream.getvalue())
            if not result.wasSuccessful() or result.skipped:
                controller_log = Path('/tmp/easy-connect-' + str(os.geteuid()) + '/controller.log')
                if controller_log.is_file(): shutil.copy2(controller_log, output / 'controller.log')
                raise ValueError(device + ' ' + kind + ' firmware probes failed; see ' + str(output / (kind + '-tests.log')))
            passed.extend(kind + ':' + name for name in selected)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--host-build', required=True, type=Path)
    parser.add_argument('--firmware', required=True, type=Path)
    args = parser.parse_args()
    if os.geteuid() == 0: parser.error('Firmware service probes must run as a non-root account')
    profile = load_profile()
    before = source_metadata(ROOT)
    paths = binaries(ROOT, args.build)
    native = json.loads((args.host_build / 'host-tests.json').read_text())
    if native.get('status') != 'passed' or native.get('source') != before:
        raise ValueError('Native tests are missing, stale or failed')
    readelf = Path(os.environ['POCKETBOOK_SDK']) / 'usr/bin/arm-obreey-linux-gnueabi-readelf'
    qemu = os.environ.get('EC_QEMU') or shutil.which('qemu-arm')
    if not qemu: raise ValueError('QEMU ARM is required; use the Docker build wrapper')
    emulator = run([qemu, '--version'])
    match = re.search(r'version\s+(\d+)\.(\d+)', emulator['stdout'])
    if emulator['exit_code'] or not match or tuple(map(int, match.groups())) < (9, 2):
        raise ValueError('QEMU 9.2+ is required for process supervision; use the Docker build wrapper')
    base = json.loads((args.firmware / 'pb632-runtime.json').read_text())
    qemu = {'path': qemu, 'preserve_argv0': False}
    startup_probe = run(emulated_command(qemu, Path(base['runtime']), paths['easy-connect'], ['--version']))
    if startup_probe['exit_code'] or ('Easy Connect ' + profile['version']) not in startup_probe['stdout']:
        qemu['preserve_argv0'] = True
        startup_probe = run(emulated_command(qemu, Path(base['runtime']), paths['easy-connect'], ['--version']))
    if startup_probe['exit_code'] or ('Easy Connect ' + profile['version']) not in startup_probe['stdout']:
        raise ValueError('QEMU could not invoke the baseline ARM controller: ' + startup_probe['stderr'])
    report = {'schema': 1, 'application': 'Easy Connect', 'family': profile['family'],
              'version': profile['version'], 'source': before, 'profile': profile,
              'emulator': {'version': emulator['stdout'].splitlines()[0],
                           'sha256': hashlib.sha256(Path(qemu['path']).read_bytes()).hexdigest(),
                           'preserve_argv0': qemu['preserve_argv0']},
              'binaries': hashes(paths), 'native_tests': native, 'devices': {},
              'device_validation': 'not-performed',
              'limits': ['QEMU probes do not verify physical UI, scanner, Wi-Fi, suspend or USB behavior',
                         'SSH/FTP probes use a native controller harness; background probes use the ARM controller and a native client on loopback']}
    for name, path in paths.items():
        attributes = subprocess.check_output([str(readelf), '-A', str(path)], text=True)
        if 'VFPv4' in attributes or 'Fused-MAC' in attributes:
            raise ValueError('Executable exceeds the NEON/VFPv3 baseline: ' + name)
        dynamic = subprocess.check_output([str(readelf), '-d', str(path)], text=True)
        if re.search(r'NEEDED.*\[lib(?:crypto|ssl)\.', dynamic):
            raise ValueError('Unexpected shared OpenSSL dependency: ' + name)
        symbols = subprocess.check_output([str(readelf), '--dyn-syms', '--wide', str(path)], text=True)
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[6] != 'UND' and re.match(r'(EVP_|CRYPTO_|OPENSSL_|RAND_|SHA[0-9])', fields[7]):
                raise ValueError('Private crypto symbol is exported: ' + name + ': ' + fields[7])
    inputs = json.loads((ROOT / 'config/firmware.json').read_text())['devices']
    for device in inputs:
        print('Checking ' + device['label'] + ': ' + device['software_version'], flush=True)
        entry = json.loads((args.firmware / (device['id'] + '-runtime.json')).read_text())
        if entry['firmware'] != device['software_version'] or entry['archive_sha256'] != device['archive']['sha256']:
            raise ValueError('Firmware provenance mismatch')
        runtime = Path(entry['runtime']).resolve()
        loader = runtime / 'lib/ld-linux.so.3'
        identity = elf_identity(loader)
        abi = {'elf_class': identity['class'], 'elf_machine': identity['machine'],
               'float_abi': identity['float_abi'], 'loader': 'lib/ld-linux.so.3'}
        output = args.build / 'firmware-probes' / device['id']
        output.mkdir(parents=True, exist_ok=True)
        native_theme, _, _ = read_theme(entry['theme'])
        item = {'native_theme': native_theme, 'firmware': device['software_version'], 'archive_sha256': device['archive']['sha256'], 'binaries': {}}
        for name, path in paths.items():
            item['binaries'][name] = check_runtime(path, runtime, readelf, abi)
        item['functional_tests'] = functional_probes(device['id'], paths, args.build, args.host_build, runtime, qemu, output)
        version = run(emulated_command(qemu, runtime, paths['easy-connect'], ['--version']))
        if version['exit_code'] != 0 or profile['version'] not in version['stdout']:
            raise ValueError('ARM controller version probe failed')
        item['controller_startup'] = version
        startup = run(emulated_command(qemu, runtime, paths['easy-connect.app'],
                      environment=[('LD_BIND_NOW', '1'), ('LD_DEBUG', 'libs')]), timeout=8)
        (output / 'ui-startup.log').write_text(startup['stdout'] + startup['stderr'])
        item['ui_startup'] = {'control_transferred': 'transferring control:' in startup['stderr'],
                              'exit_code': startup['exit_code'], 'timeout': startup.get('timeout', False)}
        if not item['ui_startup']['control_transferred']:
            raise ValueError('UI executable did not load on ' + device['id'])
        item['status'] = 'passed'
        report['devices'][device['model'].upper()] = item
        print(device['id'] + ': five ELF checks and ' + str(len(item['functional_tests'])) + ' firmware functional probes passed', flush=True)
    if before != source_metadata(ROOT): raise ValueError('Source changed during firmware verification')
    report['status'] = 'passed'
    (args.build / 'firmware-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    print('All shared firmware checks passed', flush=True)

if __name__ == '__main__': main()
