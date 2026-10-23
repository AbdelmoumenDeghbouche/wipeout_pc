"""Save mount capacity and failure contracts; build savedata-mount-test first."""
from paths import ROOT
import subprocess
import sys
import tempfile
import unittest

NAME = 'savedata-mount-test' + ('.exe' if sys.platform == 'win32' else '')
EXE = next((path for path in (ROOT / 'out' / NAME, ROOT / 'out/gpu' / NAME) if path.is_file()),
           ROOT / 'out' / NAME)


@unittest.skipUnless(EXE.is_file(), 'build savedata-mount-test first (bash build.sh --test)')
class SaveMountTests(unittest.TestCase):
    def run_case(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([str(EXE.resolve()), mode, directory],
                                    capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: save mount', result.stdout)

    def test_sixteen_save_mounts_with_default_mounts(self):
        self.run_case('default')

    def test_sixteen_save_mounts_with_assets_and_pause_menu(self):
        self.run_case('custom')

    def test_full_file_table_does_not_publish_a_new_save_and_can_retry(self):
        self.run_case('full-new')

    def test_full_file_table_preserves_an_existing_save_and_can_retry(self):
        self.run_case('full-existing')

    def test_failed_directory_creation_can_retry(self):
        self.run_case('init-failure')
