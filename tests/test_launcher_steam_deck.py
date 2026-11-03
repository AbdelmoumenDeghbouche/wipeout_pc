"""On a Steam Deck the output resolution defaults to 1280x720: the presenter letterboxes 16:9 into
the 16:10 panel, so 720p is all it shows, and it took races from 47-53 to 58-60 FPS (2026-11-02,
docs/steam-deck-performance.md). A saved value is never overridden."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402


class SteamDeckDetectionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def product(self, name):
        path = self.tmp / 'product_name'
        path.write_text(name + '\n', encoding='utf-8')
        return path

    def test_the_lcd_and_oled_decks_are_recognised(self):
        for name in ('Jupiter', 'Galileo'):
            with self.subTest(name=name), mock.patch.object(launcher, 'WINDOWS', False), \
                    mock.patch.object(launcher, 'DMI_PRODUCT_NAME', self.product(name)):
                self.assertTrue(launcher.steam_deck())

    def test_other_machines_windows_and_no_dmi_are_not_a_deck(self):
        with mock.patch.object(launcher, 'WINDOWS', False), \
                mock.patch.object(launcher, 'DMI_PRODUCT_NAME', self.product('ROG Ally RC71L')):
            self.assertFalse(launcher.steam_deck())
        with mock.patch.object(launcher, 'WINDOWS', False), \
                mock.patch.object(launcher, 'DMI_PRODUCT_NAME', self.tmp / 'missing'):
            self.assertFalse(launcher.steam_deck())
        with mock.patch.object(launcher, 'WINDOWS', True), \
                mock.patch.object(launcher, 'DMI_PRODUCT_NAME', self.product('Jupiter')):
            self.assertFalse(launcher.steam_deck())


class OutputDefaultTest(unittest.TestCase):
    def load(self, ini, deck):
        path = Path(tempfile.mkdtemp()) / 'gow3.ini'
        if ini is not None:
            path.write_text(ini, encoding='utf-8')
        with mock.patch.dict(os.environ, {'GOW3_CONFIG': str(path)}), \
                mock.patch.object(launcher, 'steam_deck', return_value=deck):
            return launcher.load_ini()[0]['output_res']

    def test_a_deck_without_a_saved_value_outputs_720p(self):
        self.assertEqual(self.load(None, deck=True), '1280x720')
        self.assertEqual(self.load('vsync=1\n', deck=True), '1280x720')

    def test_other_machines_keep_1080p(self):
        self.assertEqual(self.load(None, deck=False), '1920x1080')

    def test_a_saved_value_is_never_overridden(self):
        for value in ('1920x1080', '2560x1440'):
            with self.subTest(value=value):
                self.assertEqual(self.load(f'output_res={value}\n', deck=True), value)
        self.assertEqual(self.load('output_res=1280x720\n', deck=False), '1280x720')


class PlayPathTest(unittest.TestCase):
    """Steam starts the game with --play, which never opens the window that saves the file, and the
    game reads gow3.ini itself: the Deck default has to be written before the game starts."""
    def write(self, ini, deck):
        path = Path(tempfile.mkdtemp()) / 'gow3.ini'
        if ini is not None:
            path.write_text(ini, encoding='utf-8')
        with mock.patch.dict(os.environ, {'GOW3_CONFIG': str(path)}), \
                mock.patch.object(launcher, 'steam_deck', return_value=deck):
            launcher.write_deck_defaults()
        return path.read_text(encoding='utf-8') if path.is_file() else None

    def test_a_deck_without_a_saved_value_gets_720p_written(self):
        text = self.write('vsync=1\n', deck=True)
        self.assertIn('output_res=1280x720\n', text)
        self.assertIn('vsync=1\n', text)
        self.assertIn('output_res=1280x720\n', self.write(None, deck=True))

    def test_a_saved_value_and_other_machines_are_left_alone(self):
        self.assertEqual(self.write('output_res=1920x1080\n', deck=True), 'output_res=1920x1080\n')
        self.assertEqual(self.write('vsync=1\n', deck=False), 'vsync=1\n')
        self.assertIsNone(self.write(None, deck=False))


if __name__ == '__main__':
    unittest.main()
