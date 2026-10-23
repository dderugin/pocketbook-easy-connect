#!/usr/bin/env python3
"""Offline tests for dependency validation and release contents; no SDK required."""
from pathlib import Path
from unittest import mock
import hashlib
import importlib.util
import io
import json
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location('ec_test_' + name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-dependency-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'adapters').mkdir()
        (self.root / 'adapters/password.h').write_text('test adapter')
        self.tool = load_tool('dependencies')
        self.tool.ROOT = self.root

    def archive(self, members=None):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w:gz') as archive:
            directory = tarfile.TarInfo('upstream')
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o755
            archive.addfile(directory)
            file = tarfile.TarInfo('upstream/source.c')
            payload = b'upstream source'
            file.size = len(payload)
            archive.addfile(file, io.BytesIO(payload))
            for member in members or []:
                archive.addfile(member, io.BytesIO(b'') if member.isfile() else None)
        payload = buffer.getvalue()
        self.tool.LOCK = {'dropbear': {'version': 'test', 'url': 'https://example.invalid/source.tar.gz',
                                     'sha256': hashlib.sha256(payload).hexdigest()}}
        path = self.root / 'work/downloads/source.tar.gz'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return path

    def test_prepare_uses_verified_cache_and_copies_adapter(self):
        self.archive()
        with mock.patch.object(self.tool, 'urlopen', side_effect=AssertionError('unexpected network')):
            source = self.tool.prepare('dropbear')
        self.assertEqual((source / 'source.c').read_text(), 'upstream source')
        self.assertEqual((source / 'ec-password.h').read_text(), 'test adapter')

    def test_missing_cache_explains_fetch_requirement(self):
        path = self.archive()
        path.unlink()
        with self.assertRaisesRegex(RuntimeError, 'fetch first'):
            self.tool.prepare('dropbear')

    def test_checksum_failure_does_not_destroy_previous_build(self):
        path = self.archive()
        source = self.tool.prepare('dropbear')
        marker = source / 'keep'
        marker.write_text('previous build')
        path.write_bytes(b'tampered')
        with self.assertRaisesRegex(RuntimeError, 'Checksum mismatch'):
            self.tool.prepare('dropbear')
        self.assertEqual(marker.read_text(), 'previous build')

    def test_archive_traversal_absolute_and_special_entries_rejected(self):
        for name, kind in (('../escape', tarfile.REGTYPE), ('/absolute', tarfile.REGTYPE),
                           ('upstream/../../escape', tarfile.REGTYPE),
                           ('upstream/link', tarfile.SYMTYPE), ('upstream/hard', tarfile.LNKTYPE),
                           ('upstream/fifo', tarfile.FIFOTYPE)):
            with self.subTest(name=name):
                member = tarfile.TarInfo(name)
                member.type = kind
                member.linkname = '../../escape'
                self.archive([member])
                with self.assertRaisesRegex(RuntimeError, 'Unsafe source archive entry'):
                    self.tool.prepare('dropbear')
                self.assertFalse((self.root / 'escape').exists())
                self.assertEqual(list((self.root / 'work/dependencies/dropbear').iterdir()), [])

    def test_patch_requires_unique_anchor_and_preserves_file_on_failure(self):
        path = self.root / 'source.c'
        for text in ('missing', 'anchor anchor'):
            path.write_text(text)
            with self.assertRaisesRegex(RuntimeError, 'anchor changed'):
                self.tool.checked_replace(path, 'anchor', 'new')
            self.assertEqual(path.read_text(), text)
        path.write_text('before anchor after')
        self.tool.checked_replace(path, 'anchor', 'new')
        self.assertEqual(path.read_text(), 'before new after')

    def test_fetch_reuses_and_verifies_cache_without_network(self):
        path = self.archive()
        with mock.patch.object(sys, 'argv', ['dependencies.py', 'fetch']), \
             mock.patch.object(self.tool, 'urlopen', side_effect=AssertionError('unexpected network')):
            self.tool.main()
            path.write_bytes(b'tampered')
            with self.assertRaisesRegex(RuntimeError, 'Checksum mismatch'):
                self.tool.main()

    def test_download_rejects_unpinned_bytes_without_caching_them(self):
        path = self.archive()
        expected = path.read_bytes()
        path.unlink()
        with mock.patch.object(sys, 'argv', ['dependencies.py', 'fetch']), \
             mock.patch.object(self.tool, 'urlopen', return_value=io.BytesIO(b'tampered')):
            with self.assertRaisesRegex(RuntimeError, 'Checksum mismatch'):
                self.tool.main()
        self.assertFalse(path.exists())
        with mock.patch.object(sys, 'argv', ['dependencies.py', 'fetch']), \
             mock.patch.object(self.tool, 'urlopen', return_value=io.BytesIO(expected)):
            self.tool.main()
        self.assertEqual(path.read_bytes(), expected)


class SdkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-sdk-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tool = load_tool('sdk')
        self.tool.ROOT = self.root
        self.payload = b'verified SDK archive contents'
        self.tool.LOCK = {'url': 'https://example.invalid/sdk.7z',
                          'sha256': hashlib.sha256(self.payload).hexdigest()}
        self.path = self.root / 'work/sdk/sdk.7z'
        # Small blocks ensure streaming and checksum tests cross chunk boundaries.
        self.tool.CHUNK_SIZE = 4

    def test_download_streams_verified_bytes_and_cleans_temporary_files(self):
        with mock.patch.object(self.tool, 'urlopen', return_value=io.BytesIO(self.payload)):
            self.assertEqual(self.tool.fetch(self.path), self.path)
        self.assertEqual(self.path.read_bytes(), self.payload)
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    def test_verified_cache_never_uses_network(self):
        self.path.parent.mkdir(parents=True)
        self.path.write_bytes(self.payload)
        with mock.patch.object(self.tool, 'urlopen', side_effect=AssertionError('network')):
            self.tool.fetch(self.path)
            self.tool.verify(self.path)

    def test_corrupt_cache_is_preserved_and_never_silently_redownloaded(self):
        self.path.parent.mkdir(parents=True)
        self.path.write_bytes(b'tampered')
        with mock.patch.object(self.tool, 'urlopen', side_effect=AssertionError('network')):
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                self.tool.fetch(self.path)
        self.assertEqual(self.path.read_bytes(), b'tampered')

    def test_unverified_download_is_not_published(self):
        with mock.patch.object(self.tool, 'urlopen', return_value=io.BytesIO(b'tampered')):
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                self.tool.fetch(self.path)
        self.assertEqual(list(self.path.parent.iterdir()), [])

    def test_interrupted_download_cleans_partial_archive(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read.side_effect = [b'partial', OSError('connection lost')]
        with mock.patch.object(self.tool, 'urlopen', return_value=response):
            with self.assertRaisesRegex(OSError, 'connection lost'):
                self.tool.fetch(self.path)
        self.assertEqual(list(self.path.parent.iterdir()), [])

    def test_offline_verify_requires_existing_archive(self):
        with mock.patch.object(self.tool, 'urlopen', side_effect=AssertionError('network')):
            with self.assertRaises(FileNotFoundError):
                self.tool.verify(self.path)

    def test_cli_default_cache_and_archive_override(self):
        with mock.patch.object(sys, 'argv', ['sdk.py', 'fetch']), \
             mock.patch.object(self.tool, 'urlopen', return_value=io.BytesIO(self.payload)):
            self.tool.main()
        self.assertEqual(self.path.read_bytes(), self.payload)
        alternate = self.root / 'alternate.7z'
        alternate.write_bytes(self.payload)
        with mock.patch.object(sys, 'argv', ['sdk.py', 'verify', '--archive', str(alternate)]), \
             mock.patch.object(self.tool, 'urlopen', side_effect=AssertionError('network')):
            self.tool.main()


class PackageTests(unittest.TestCase):
    """Minimal ELF headers test packaging policy, not ARM executability."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='easy-connect-package-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tool = load_tool('package')
        self.tool.ROOT = self.root
        for directory in ('src', 'adapters', 'tools', 'tests', 'data', 'build',
                          'work/stage/bin', 'work/stage/licenses/dropbear',
                          'work/stage/licenses/vsftpd', 'work/downloads', 'work/sdk', 'state', 'data/licenses'):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        for directory in ('src', 'adapters', 'tools', 'tests'):
            (self.root / directory / 'source.txt').write_text(directory + ' source')
        for name in ('README.md', 'LICENSE', 'CMakeLists.txt'):
            (self.root / name).write_text(name)
        (self.root / 'sdk.json').write_text(json.dumps({'name': 'test-sdk', 'sha256': 'test'}))
        (self.root / 'data/licenses/openssl.txt').write_text('OpenSSL license')
        (self.root / 'work/sdk/SDK-B288-6.10.7z').write_bytes(b'SDK ARCHIVE MUST NOT SHIP')
        (self.root / 'data/EasyConnect.app').write_text('#!/bin/sh\nexec application\n')
        (self.root / 'data/easy-connect.conf.example').write_text('auth=key\n')
        for name in ('host_ed25519', 'password.hash', 'ftp-password.hash', 'authorized_keys'):
            (self.root / 'state' / name).write_text('LIVE SECRET MUST NOT SHIP')
        (self.root / 'easy-connect.conf').write_text('LIVE SETTINGS MUST NOT SHIP')
        for path in ('work/stage/licenses/dropbear/LICENSE', 'work/stage/licenses/vsftpd/COPYING'):
            (self.root / path).write_text('upstream license')
        dependencies = {}
        for name in ('dropbear', 'vsftpd'):
            payload = (name + ' source archive').encode()
            (self.root / 'work/downloads' / (name + '.tar.gz')).write_bytes(payload)
            dependencies[name] = {'version': 'test', 'url': 'https://example.invalid/' + name + '.tar.gz',
                                  'sha256': hashlib.sha256(payload).hexdigest()}
        (self.root / 'dependencies.json').write_text(json.dumps(dependencies))
        header = bytearray(52)
        header[:6] = b'\x7fELF\x01\x01'
        struct.pack_into('<H', header, 18, 40)
        self.binaries = {name: self.root / directory / name for name, directory in (
            ('easy-connect', 'build'), ('easy-connect.app', 'build'),
            ('dropbear', 'work/stage/bin'), ('dropbearkey', 'work/stage/bin'),
            ('vsftpd', 'work/stage/bin'))}
        for path in self.binaries.values():
            path.write_bytes(header)
        self.zip = self.root / 'dist/easy-connect-pb632-0.1.0.zip'

    def package(self):
        with mock.patch.object(sys, 'argv', ['package.py', '--build', str(self.root / 'build')]):
            self.tool.main()

    def test_release_layout_includes_executables_licenses_and_corresponding_sources(self):
        self.package()
        prefix = 'system/share/easy-connect/'
        with zipfile.ZipFile(self.zip) as archive:
            names = set(archive.namelist())
            for name in self.binaries:
                self.assertIn(prefix + 'bin/' + name, names)
            for path in ('licenses/dropbear/LICENSE', 'licenses/vsftpd/COPYING', 'licenses/openssl/openssl.txt',
                         'source/sdk.json',
                         'source/src/source.txt', 'source/adapters/source.txt',
                         'source/tools/source.txt', 'source/tests/source.txt',
                         'source/CMakeLists.txt', 'source/work/downloads/dropbear.tar.gz',
                         'source/work/downloads/vsftpd.tar.gz', 'easy-connect.conf.example'):
                self.assertIn(prefix + path, names)
            for path in ['applications/EasyConnect.app'] + [prefix + 'bin/' + name for name in self.binaries]:
                self.assertEqual((archive.getinfo(path).external_attr >> 16) & 0o777, 0o755)
            for path in names:
                self.assertFalse(path.startswith('/') or '..' in Path(path).parts)
                self.assertNotIn(b'SDK ARCHIVE MUST NOT SHIP', archive.read(path))
                self.assertNotIn(b'LIVE SECRET MUST NOT SHIP', archive.read(path))
                self.assertNotIn(b'LIVE SETTINGS MUST NOT SHIP', archive.read(path))
            self.assertFalse(any('/state/' in path or path.endswith('/easy-connect.conf') for path in names))

    def test_manifest_and_release_checksum_match_actual_bytes(self):
        self.package()
        with zipfile.ZipFile(self.zip) as archive:
            info = json.loads(archive.read('system/share/easy-connect/build-info.json'))
        self.assertEqual(info['device_validation'], 'not-performed')
        self.assertEqual(info['sdk'], json.loads((self.root / 'sdk.json').read_text()))
        self.assertEqual(info['dependencies'], json.loads((self.root / 'dependencies.json').read_text()))
        self.assertEqual(info['binaries'], {name: hashlib.sha256(path.read_bytes()).hexdigest()
                                            for name, path in self.binaries.items()})
        self.assertEqual(self.zip.with_suffix('.zip.sha256').read_text(),
                         hashlib.sha256(self.zip.read_bytes()).hexdigest() + '  ' + self.zip.name + '\n')

    def test_all_five_binaries_must_be_little_endian_arm32(self):
        for name, path in self.binaries.items():
            original = path.read_bytes()
            invalid = [b'host executable', original[:20]]
            for offset, value in ((4, 2), (5, 2), (18, 62)):
                changed = bytearray(original)
                changed[offset] = value
                invalid.append(changed)
            for payload in invalid:
                with self.subTest(binary=name, payload=bytes(payload[:6])):
                    path.write_bytes(payload)
                    with self.assertRaisesRegex(RuntimeError, 'not a 32-bit little-endian ARM'):
                        self.package()
                    self.assertFalse(self.zip.exists())
            path.write_bytes(original)

    def test_missing_binary_or_license_prevents_release(self):
        for path in (self.binaries['dropbear'], self.root / 'work/stage/licenses'):
            with self.subTest(path=path):
                saved = path.with_name(path.name + '-saved')
                path.rename(saved)
                try:
                    with self.assertRaises(FileNotFoundError):
                        self.package()
                    self.assertFalse(self.zip.exists())
                finally:
                    saved.rename(path)

    def test_tampered_upstream_source_prevents_release(self):
        (self.root / 'work/downloads/dropbear.tar.gz').write_bytes(b'tampered source')
        with self.assertRaisesRegex(RuntimeError, 'source checksum mismatch'):
            self.package()
        self.assertFalse(self.zip.exists())

    def test_repackaging_removes_stale_staged_content(self):
        stage = self.root / 'work/package'
        stage.mkdir()
        (stage / 'stale-secret').write_text('LIVE SECRET MUST NOT SHIP')
        self.package()
        with zipfile.ZipFile(self.zip) as archive:
            self.assertNotIn('stale-secret', archive.namelist())


if __name__ == '__main__':
    unittest.main()
