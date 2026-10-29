"""The real native Play entry captures CONFIG before giving its child the installation cwd."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

PLAY = ROOT / 'out/gpu/gow3-play.exe'
CHILD = ROOT / 'out/gpu/settings-paths-test.exe'


@unittest.skipUnless(sys.platform == 'win32' and PLAY.is_file() and CHILD.is_file(),
                     'build the Windows Play stub and settings-paths-test first')
class PlayConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wipeout-play-config-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.caller, self.install = self.root / 'llamador-Ω', self.root / 'instalación-ó'
        self.caller.mkdir()
        self.install.mkdir()
        shutil.copyfile(PLAY, self.install / 'Play WipEout.exe')
        shutil.copyfile(CHILD, self.install / 'WipEout.exe')

    def play(self, overrides):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('GOW3_', 'WIPEOUT_OC_'))}
        env.update(WIPEOUT_OC_DATA_DIR=str(self.install), WIPEOUT_OC_USER_DIR=str(self.root / 'profile'),
                   WIPEOUT_OC_GPU_USER_DIR=str(self.root / 'profile'), WIPEOUT_OC_DISCORD='0')
        env.update(overrides)
        done = subprocess.run([str(self.install / 'Play WipEout.exe')], cwd=self.caller, env=env,
                              capture_output=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return (self.install / 'play-config.txt').read_text(encoding='utf-8')

    def test_selected_relative_alias_is_anchored_before_child_cwd_changes(self):
        for env in ({'WIPEOUT_OC_CONFIG': 'chosen.ini'}, {'GOW3_CONFIG': 'chosen.ini'},
                    {'WIPEOUT_OC_CONFIG': 'chosen.ini', 'GOW3_CONFIG': 'ignored.ini'}):
            with self.subTest(env=env):
                caller, installed = self.caller / 'chosen.ini', self.install / 'chosen.ini'
                caller.write_bytes(b'engine_fps=240\nunknown=caller\n')
                installed.write_bytes(b'engine_fps=60\nunknown=install\n')
                self.assertEqual(self.play(env), str(caller))
                self.assertNotIn('unknown=caller', caller.read_text())  # Native Save reached the caller.
                self.assertEqual(installed.read_bytes(), b'engine_fps=60\nunknown=install\n')

    def test_canonical_empty_keeps_default_and_suppresses_legacy(self):
        chosen = self.caller / 'chosen.ini'
        chosen.write_bytes(b'engine_fps=240\nunknown=caller\n')
        self.assertEqual(self.play({'WIPEOUT_OC_CONFIG': '', 'GOW3_CONFIG': 'chosen.ini'}), '')
        self.assertEqual(chosen.read_bytes(), b'engine_fps=240\nunknown=caller\n')
        self.assertTrue((self.install / 'wipeout_oc.ini').is_file())
        self.assertFalse((self.caller / 'wipeout_oc.ini').exists())

    def test_unicode_parent_components_are_kept_in_selected_path(self):
        (self.caller / 'sub-ó').mkdir()
        chosen = self.caller / 'chosen.ini'
        chosen.write_text('engine_fps=240\n')
        relative = str(Path('sub-ó') / '..' / 'chosen.ini')
        self.assertEqual(self.play({'WIPEOUT_OC_CONFIG': relative}), str(self.caller / relative))

    def test_absolute_override_is_unchanged(self):
        chosen = self.caller / 'chosen.ini'
        chosen.write_text('engine_fps=240\n')
        self.assertEqual(self.play({'WIPEOUT_OC_CONFIG': str(chosen)}), str(chosen))

    def test_drive_relative_and_rooted_overrides_are_anchored(self):
        chosen = self.caller / 'chosen.ini'
        for override in (self.caller.drive + 'chosen.ini', str(chosen)[len(self.caller.drive):]):
            with self.subTest(override=override):
                chosen.write_text('engine_fps=240\n')
                self.assertEqual(self.play({'WIPEOUT_OC_CONFIG': override}), str(chosen))


if __name__ == '__main__':
    unittest.main()
