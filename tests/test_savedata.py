"""Metadata error propagation and preservation; build savedata-test first."""
from paths import ROOT
import subprocess
import sys
import tempfile
import unittest

NAME = 'savedata-test' + ('.exe' if sys.platform == 'win32' else '')
EXE = next((path for path in (ROOT / 'out' / NAME, ROOT / 'out/gpu' / NAME) if path.is_file()),
           ROOT / 'out' / NAME)


@unittest.skipUnless(EXE.is_file(), 'build savedata-test first (bash build.sh --test)')
class SaveDataTests(unittest.TestCase):
    def run_case(self, mode, fault):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([str(EXE.resolve()), mode, fault, directory],
                                    capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: save metadata', result.stdout)

    def test_metadata_write_failures_and_retries(self):
        for mode in ('set', 'mount', 'memory', 'memory-existing'):
            for fault in ('none', 'open', 'write', 'close', 'replace'):
                with self.subTest(mode=mode, fault=fault):
                    self.run_case(mode, fault)

    def test_partial_update_preserves_metadata_when_read_fails(self):
        self.run_case('partial', 'input')

    def test_partial_update_preserves_other_fields(self):
        self.run_case('partial', 'none')

    @unittest.skipIf(sys.platform == 'win32', '/dev/full is a POSIX device')
    def test_real_disk_full_preserves_previous_metadata(self):
        self.run_case('set', 'full')
