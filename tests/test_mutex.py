"""Guest mutex layout and host pthread state must not overlap."""
from paths import ROOT
import subprocess
import sys
import unittest

NAME = 'mutex-test' + ('.exe' if sys.platform == 'win32' else '')
EXE = next((p for p in (ROOT / 'out' / NAME, ROOT / 'out/gpu' / NAME) if p.is_file()),
           ROOT / 'out' / NAME)


@unittest.skipUnless(EXE.is_file(), 'build mutex-test first (build.sh --test or ninja -C out/gpu mutex-test)')
class MutexTests(unittest.TestCase):
    def test_libc_private_flags_preserve_native_mutex(self):
        result = subprocess.run([str(EXE.resolve())], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: guest mutex header', result.stdout)
