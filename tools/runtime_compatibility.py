"""Inspect ELF compatibility using firmware files only; never execute host ldd."""
from pathlib import Path
import hashlib
import os
import re
import struct
import subprocess


def runtime_fingerprint(runtime, names):
    runtime = Path(runtime).resolve()
    result = {}
    for name in sorted(names):
        path = (runtime / name).resolve()
        if runtime not in path.parents or not path.is_file():
            raise ValueError('Firmware input missing or outside runtime: ' + name)
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(chunk)
        result[name] = digest.hexdigest()
    return result


def elf_identity(path):
    with Path(path).open('rb') as stream:
        header = stream.read(64)
    if len(header) < 52 or header[:4] != b'\x7fELF' or header[4] not in (1, 2) or header[5] not in (1, 2):
        raise ValueError('Not a supported ELF file: ' + str(path))
    order = '<' if header[5] == 1 else '>'
    bits = 32 if header[4] == 1 else 64
    machine = struct.unpack_from(order + 'H', header, 18)[0]
    flags = struct.unpack_from(order + 'I', header, 36 if bits == 32 else 48)[0]
    return {'class': bits, 'machine': machine, 'endianness': header[5],
            'float_abi': ('hard' if flags & 0x400 else 'soft') if machine == 40 else None}


def elf_stack_policy(path):
    """Inspect PT_GNU_STACK without relying on the host's ELF tools."""
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if len(header) < 52 or header[:4] != b'\x7fELF' or header[4] not in (1, 2) or header[5] not in (1, 2):
            raise ValueError('Not a supported ELF file: ' + str(path))
        order = '<' if header[5] == 1 else '>'
        wide = header[4] == 2
        if wide and len(header) < 64:
            raise ValueError('Truncated ELF header: ' + str(path))
        offset = struct.unpack_from(order + ('Q' if wide else 'I'), header, 32 if wide else 28)[0]
        size, count = struct.unpack_from(order + 'HH', header, 54 if wide else 42)
        minimum = 56 if wide else 32
        if count and size < minimum:
            raise ValueError('Invalid ELF program header size: ' + str(path))
        for index in range(count):
            stream.seek(offset + size * index)
            program = stream.read(minimum)
            if len(program) != minimum:
                raise ValueError('Truncated ELF program header: ' + str(path))
            if struct.unpack_from(order + 'I', program)[0] == 0x6474e551:
                flags = struct.unpack_from(order + 'I', program, 4 if wide else 24)[0]
                return 'executable' if flags & 1 else 'non-executable'
    return 'unspecified'


def check_runtime(application, runtime, readelf, firmware):
    runtime = Path(runtime).resolve()
    env = dict(os.environ, LC_ALL='C')

    def read(path, *options):
        return subprocess.check_output([str(readelf), *options, str(path)], text=True, env=env)

    expected = {'class': firmware['elf_class'], 'machine': firmware['elf_machine'],
                'endianness': 1, 'float_abi': firmware['float_abi']}

    def identity(path):
        actual = elf_identity(path)
        if actual != expected:
            raise ValueError('ELF ABI mismatch for ' + str(path) + ': ' + str(actual) + '; expected ' + str(expected))

    def resolve(name):
        # Library paths in the report may not escape the extracted runtime.
        for directory in ('lib', 'usr/lib', 'ebrmain/lib'):
            candidate = (runtime / directory / name).resolve()
            if runtime in candidate.parents and candidate.is_file():
                return candidate
        raise ValueError('Firmware library is missing: ' + name)

    identity(application)
    headers = read(application, '-l', '--wide')
    interpreter = re.search(r'Requesting program interpreter: ([^\]]+)\]', headers)
    if not interpreter or interpreter[1].lstrip('/') != firmware['loader']:
        raise ValueError('Executable loader does not match target: ' + (interpreter[1] if interpreter else 'missing'))
    loader = resolve(Path(firmware['loader']).name)
    identity(loader)
    inspected = {}
    symbol_tables = {}

    def inspect(path):
        path = Path(path).resolve()
        if path in inspected:
            return inspected[path]
        identity(path)
        dynamic = read(path, '-d', '--wide')
        needed = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic)
        # Needed version names belong to a specific library, not a global union.
        definitions, requirements = set(), {}
        section, library = None, None
        for line in read(path, '--version-info', '--wide').splitlines():
            if line.startswith('Version definition section'):
                section = 'definitions'
            elif line.startswith('Version needs section'):
                section = 'needs'
            elif line.startswith('Version symbols section'):
                section = None
            if section == 'needs':
                match = re.search(r'File: (\S+)', line)
                if match:
                    library = match[1]
                    requirements.setdefault(library, set())
            match = re.search(r'Name: (\S+)', line)
            if match and section == 'definitions':
                definitions.add(match[1])
            elif match and section == 'needs' and library:
                requirements[library].add(match[1])
        inspected[path] = definitions  # Break dependency cycles before traversal.
        imported, exported = set(), set()
        for line in read(path, '--dyn-syms', '--wide').splitlines():
            fields = line.split()
            if len(fields) < 8 or not fields[0].rstrip(':').isdigit():
                continue
            name, separator, version = fields[7].partition('@')
            symbol = (name, version.lstrip('@') if separator else '')
            if fields[6] == 'UND' and fields[4] == 'GLOBAL':
                imported.add(symbol)
            elif fields[6] != 'UND' and fields[4] in ('GLOBAL', 'WEAK', 'UNIQUE') and fields[5] in ('DEFAULT', 'PROTECTED'):
                exported.add(symbol)
        symbol_tables[path] = (imported, exported)
        for name in set(needed) | set(requirements):
            dependency = resolve(name)
            available = inspect(dependency)
            missing = requirements.get(name, set()) - available
            if missing:
                raise ValueError(str(path) + ': ' + name + ' lacks versions ' + ', '.join(sorted(missing)))
        return definitions

    inspect(application)
    inspect(loader)
    exports = set().union(*(exported for _, exported in symbol_tables.values()))
    exported_names = {name for name, _ in exports}
    for path, (imported, _) in symbol_tables.items():
        missing = [(name, version) for name, version in imported
                   if (version and (name, version) not in exports) or (not version and name not in exported_names)]
        if missing:
            formatted = ', '.join(name + ('@' + version if version else '') for name, version in sorted(missing))
            raise ValueError(str(path) + ': unresolved required symbols: ' + formatted)
    libraries = sorted(str(path.relative_to(runtime)) for path in inspected if runtime in path.parents)
    return {'status': 'passed', 'abi': expected, 'loader': interpreter[1],
            'libraries': libraries, 'files_sha256': runtime_fingerprint(runtime, libraries),
            'stack_policy': {'application': elf_stack_policy(application), 'libraries': {
                name: elf_stack_policy(runtime / name) for name in libraries}},
            'scope': 'ELF ABI, loader, dependency closure, symbol versions and required dynamic symbols; hardware behavior is untested'}
