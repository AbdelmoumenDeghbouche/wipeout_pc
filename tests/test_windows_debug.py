"""The Windows build mirror must never copy local data or erase build outputs."""
import paths  # noqa: F401
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import os
import sys

import windows_debug


@unittest.skipUnless(sys.platform == 'linux', 'WSL-side synchronization tests')
class WindowsDebugTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'source'
        self.target = Path(self.temp.name) / 'Windows build'
        self.root.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)

    def track(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        subprocess.run(['git', '-C', str(self.root), 'add', '--', name], check=True)
        return path

    def test_copies_current_tracked_source_but_no_packages_or_secrets(self):
        source = self.track('src/example.c', 'old')
        source.write_text('working tree edit')
        (self.root / 'local.env').write_text('private')
        (self.root / 'pkg').mkdir()
        (self.root / 'pkg/game.pkg').write_bytes(b'game')
        windows_debug.sync(self.root, self.target)
        self.assertEqual((self.target / 'src/example.c').read_text(), 'working tree edit')
        self.assertFalse((self.target / 'local.env').exists())
        self.assertFalse((self.target / 'pkg').exists())
        self.assertFalse((self.target / '.git').exists())

    def test_removes_stale_source_but_preserves_outputs_and_saves(self):
        source = self.track('src/example.c', 'first')
        windows_debug.sync(self.root, self.target)
        for name in ('out/probe.exe', 'user-debug/save.bin'):
            path = self.target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('keep')
        source.unlink()
        self.track('src/next.c', 'next')
        windows_debug.sync(self.root, self.target)
        self.assertFalse((self.target / 'src/example.c').exists())
        self.assertEqual((self.target / 'src/next.c').read_text(), 'next')
        self.assertEqual((self.target / 'out/probe.exe').read_text(), 'keep')
        self.assertEqual((self.target / 'user-debug/save.bin').read_text(), 'keep')

    def test_refuses_an_unowned_directory_or_source_overlap(self):
        self.target.mkdir()
        (self.target / 'important.txt').write_text('keep')
        with self.assertRaises(ValueError):
            windows_debug.sync(self.root, self.target)
        with self.assertRaises(ValueError):
            windows_debug.sync(self.root, self.root / 'mirror')
        self.assertEqual((self.target / 'important.txt').read_text(), 'keep')

    def test_rejects_source_symlinks_outside_the_repository(self):
        private = Path(self.temp.name) / 'private.txt'
        private.write_text('private')
        (self.root / 'linked.txt').symlink_to(private)
        subprocess.run(['git', '-C', str(self.root), 'add', 'linked.txt'], check=True)
        with self.assertRaises(ValueError):
            windows_debug.sync(self.root, self.target)
        self.assertFalse((self.target / 'linked.txt').exists())

    def test_quotes_powershell_literals(self):
        self.assertEqual(windows_debug.ps_quote("C:\\O'Brien\\source"), "'C:\\O''Brien\\source'")

    def test_rounded_windows_timestamps_do_not_force_a_full_copy(self):
        source = self.track('src/example.c', 'unchanged')
        stamp = 1_700_000_000_123_456_789
        os.utime(source, ns=(stamp, stamp))
        windows_debug.sync(self.root, self.target)
        rounded = stamp // 1_000_000_000 * 1_000_000_000
        os.utime(self.target / 'src/example.c', ns=(rounded, rounded))
        with mock.patch.object(windows_debug.shutil, 'copy2', side_effect=AssertionError('unnecessary copy')):
            windows_debug.sync(self.root, self.target)

    def test_same_size_source_edit_within_one_second_is_copied(self):
        source = self.track('src/example.c', 'first')
        stamp = 1_700_000_000_123_456_789
        os.utime(source, ns=(stamp, stamp))
        windows_debug.sync(self.root, self.target)
        source.write_text('other')
        os.utime(source, ns=(stamp + 1, stamp + 1))
        windows_debug.sync(self.root, self.target)
        self.assertEqual((self.target / 'src/example.c').read_text(), 'other')


if __name__ == '__main__':
    unittest.main()
