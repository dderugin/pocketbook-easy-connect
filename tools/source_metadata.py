"""Bind binaries and verification to only the distributed application sources."""
from pathlib import Path
import hashlib
import subprocess

def source_files(root):
    files = []
    for directory in ('src', 'adapters', 'tools', 'data', 'tests', 'config'):
        files.extend(p for p in (root / directory).rglob('*') if p.is_file()
                     and '__pycache__' not in p.parts and p.suffix not in ('.pyc', '.pyo'))
    files.extend(root / name for name in ('CMakeLists.txt', 'dependencies.json', 'sdk.json', 'LICENSE', 'README.md'))
    return sorted(files, key=lambda p: p.relative_to(root).as_posix())

def source_metadata(root):
    digest = hashlib.sha256()
    for path in source_files(root):
        digest.update(path.relative_to(root).as_posix().encode() + b'\0' + path.read_bytes())
    commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=root, capture_output=True, text=True)
    status = subprocess.run(['git', 'status', '--porcelain'], cwd=root, capture_output=True, text=True)
    return {'commit': commit.stdout.strip() if commit.returncode == 0 else None,
            'dirty': bool(status.stdout) if status.returncode == 0 else None,
            'tree_sha256': digest.hexdigest()}
