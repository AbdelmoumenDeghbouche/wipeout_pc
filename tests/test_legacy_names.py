"""Updating from the earlier name keeps settings and variables."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
import legacy_names as ln  # noqa: E402


class Legacy(unittest.TestCase):
    def test_environment(self):
        env = {ln.OLD_PREFIX + 'A': 'old', ln.OLD_PREFIX + 'B': 'old', ln.NEW_PREFIX + 'B': 'new', 'PATH': 'x'}
        ln.adopt_environment(env)
        self.assertEqual((env[ln.NEW_PREFIX + 'A'], env[ln.NEW_PREFIX + 'B'], env['PATH']), ('old', 'new', 'x'))

    def test_settings_move_once(self):
        with tempfile.TemporaryDirectory() as folder:
            old, new = Path(folder) / ln.OLD_SETTINGS, Path(folder) / ln.NEW_SETTINGS
            old.write_text('vsync=0')
            ln.adopt_settings(folder)
            self.assertEqual((old.exists(), new.read_text()), (False, 'vsync=0'))
            old.write_text('stale')
            ln.adopt_settings(folder)
            self.assertEqual((old.read_text(), new.read_text()), ('stale', 'vsync=0'))

    def test_nothing_to_move(self):
        with tempfile.TemporaryDirectory() as folder:
            ln.adopt_settings(folder)
            self.assertEqual(list(Path(folder).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
