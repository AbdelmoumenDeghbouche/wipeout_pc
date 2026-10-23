"""Extraction and resume contracts using tiny synthetic PFS blocks, without game files."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import pkg_extract


class SyntheticPkg(pkg_extract.Pkg):
    def __init__(self, path, files):
        self.path = path
        self.block_size = 4
        self.entries, self.inodes, self.items = [], [], []
        data = bytearray()
        for name, content in files.items():
            blocks = (len(content) + self.block_size - 1) // self.block_size
            self.inodes.append(dict(size=len(content), flags=0, blocks=blocks,
                                    loc=len(data) // self.block_size))
            self.items.append((name, len(self.inodes) - 1, pkg_extract.PFS_FILE))
            data.extend(content.ljust(blocks * self.block_size, b'\0'))
        path.write_bytes(data)

    def read_blocks(self, first, count):
        with self.path.open('rb') as stream:
            stream.seek(first * self.block_size)
            return stream.read(count * self.block_size)


class PkgExtractionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / 'game'
        self.out.mkdir()
        self.pkg = SyntheticPkg(self.root / 'game.pkg', {'eboot.bin': b'NEW!'})

    def extract(self, pkg=None, progress=None):
        pkg = pkg or self.pkg
        return pkg_extract.extract(pkg, self.out, progress=progress, items=pkg.items)

    def test_existing_same_size_file_without_resume_record_is_replaced(self):
        (self.out / 'eboot.bin').write_bytes(b'OLD!')
        self.extract()
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'NEW!')
        self.assertTrue(pkg_extract.is_extracted(self.pkg.path, self.out))

    def test_changed_package_with_same_size_path_and_time_is_reextracted(self):
        self.extract()
        previous = self.pkg.path.stat()
        self.pkg.path.write_bytes(b'NEXT')
        os.utime(self.pkg.path, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        self.assertFalse(pkg_extract.is_extracted(self.pkg.path, self.out))
        self.extract()
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'NEXT')
        self.assertTrue(pkg_extract.is_extracted(self.pkg.path, self.out))

    def test_other_package_with_equal_size_is_not_reported_as_extracted(self):
        self.extract()
        other = SyntheticPkg(self.root / 'other.pkg', {'eboot.bin': b'ELSE'})
        self.assertFalse(pkg_extract.is_extracted(other.path, self.out))

    def test_resume_skips_completed_file_but_rewrites_unrecorded_same_size_file(self):
        pkg = SyntheticPkg(self.root / 'resume.pkg', {'eboot.bin': b'BOOT', 'z.bin': b'LAST'})
        read_blocks = pkg.read_blocks

        def interrupted(first, count):
            if first == 1:
                raise OSError('interrupted read')
            return read_blocks(first, count)

        with mock.patch.object(pkg, 'read_blocks', side_effect=interrupted):
            with self.assertRaises(OSError):
                self.extract(pkg)
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'BOOT')
        self.assertFalse(pkg_extract.is_extracted(pkg.path, self.out))
        (self.out / 'z.bin').write_bytes(b'OLD!')

        def resumed(first, count):
            if first == 0:
                self.fail('a completed, unchanged file was decompressed again')
            return read_blocks(first, count)

        with mock.patch.object(pkg, 'read_blocks', side_effect=resumed):
            self.extract(pkg)
        self.assertEqual((self.out / 'z.bin').read_bytes(), b'LAST')
        self.assertTrue(pkg_extract.is_extracted(pkg.path, self.out))

    def test_modified_completed_file_is_reextracted(self):
        self.extract()
        target = self.out / 'eboot.bin'
        previous = target.stat()
        target.write_bytes(b'EDIT')
        os.utime(target, ns=(previous.st_atime_ns, previous.st_mtime_ns + 1_000_000_000))
        self.assertFalse(pkg_extract.is_extracted(self.pkg.path, self.out))
        self.extract()
        self.assertEqual(target.read_bytes(), b'NEW!')

    def test_interrupted_package_change_does_not_adopt_old_files(self):
        pkg = SyntheticPkg(self.root / 'update.pkg', {'eboot.bin': b'BASE', 'z.bin': b'OLD!'})
        self.extract(pkg)
        pkg = SyntheticPkg(self.root / 'update.pkg', {'eboot.bin': b'BOOT', 'z.bin': b'LAST'})
        with mock.patch.object(pkg, 'read_blocks', side_effect=[b'BOOT', OSError('interrupted')]):
            with self.assertRaises(OSError):
                self.extract(pkg)
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'BOOT')
        self.assertEqual((self.out / 'z.bin').read_bytes(), b'OLD!')
        self.assertFalse(pkg_extract.is_extracted(pkg.path, self.out))
        self.extract(pkg)
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'BOOT')
        self.assertEqual((self.out / 'z.bin').read_bytes(), b'LAST')
        self.assertTrue(pkg_extract.is_extracted(pkg.path, self.out))

    def test_failed_reextraction_invalidates_completion_and_preserves_destination(self):
        self.extract()
        target = self.out / 'eboot.bin'
        target.write_bytes(b'old')
        with mock.patch.object(self.pkg, 'read_blocks', side_effect=OSError('disk read failed')):
            with self.assertRaises(OSError):
                self.extract()
        self.assertFalse(pkg_extract.is_extracted(self.pkg.path, self.out))
        self.assertEqual(target.read_bytes(), b'old')
        self.extract()
        self.assertEqual(target.read_bytes(), b'NEW!')

    def test_legacy_or_invalid_marker_does_not_authorize_skipping(self):
        markers = [f'{self.pkg.path}\n{self.pkg.path.stat().st_size}\n', '{', '[]', '{}']
        for marker in markers:
            with self.subTest(marker=marker):
                (self.out / 'eboot.bin').write_bytes(b'OLD!')
                (self.out / '.pkg_extracted').write_text(marker, encoding='utf-8')
                self.assertFalse(pkg_extract.is_extracted(self.pkg.path, self.out))
                self.extract()
                self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'NEW!')

    def test_failed_completion_marker_publication_can_resume(self):
        replace = os.replace

        def fail_completion(source, destination):
            if Path(destination).name == '.pkg_extracted':
                state = json.loads(Path(source).read_text(encoding='utf-8'))
                if state.get('complete'):
                    raise OSError('cannot publish completion')
            return replace(source, destination)

        with mock.patch.object(os, 'replace', side_effect=fail_completion):
            with self.assertRaises(OSError):
                self.extract()
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), b'NEW!')
        self.assertFalse(pkg_extract.is_extracted(self.pkg.path, self.out))
        self.extract()
        self.assertTrue(pkg_extract.is_extracted(self.pkg.path, self.out))

    def test_multichunk_file_and_empty_file_are_extracted(self):
        content = bytes(range(256)) * 5
        pkg = SyntheticPkg(self.root / 'large.pkg', {'eboot.bin': content, 'empty.bin': b''})
        progress = []
        self.extract(pkg, progress=lambda done, total, rate: progress.append((done, total)))
        self.assertEqual((self.out / 'eboot.bin').read_bytes(), content)
        self.assertEqual((self.out / 'empty.bin').read_bytes(), b'')
        self.assertEqual(progress[-1], (1280, 1280))
        self.assertTrue(pkg_extract.is_extracted(pkg.path, self.out))


if __name__ == '__main__':
    unittest.main()
