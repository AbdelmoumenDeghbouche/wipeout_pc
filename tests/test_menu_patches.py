import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import menu_patches

MENU = (b'<Screen>\n    <Slider name="Eliminations" global="NumberOfEliminations">\n'
        b'      <Values minSlide="50" maxSlide="600" default="200" incSlide="25"></Values>\n</Screen>\n')


def archive(path, data, compressed=False):
    """A one-entry PSARC 1.4 archive (64 KiB blocks) holding the racebox menu."""
    block, entry_size = 65536, 30
    toc_len = 32 + entry_size + 2
    entry = (hashlib.md5(menu_patches.MENU.upper().encode()).digest() + struct.pack('>I', 0)
             + len(data).to_bytes(5, 'big') + (toc_len).to_bytes(5, 'big'))
    sizes = struct.pack('>H', len(data) - 1 if compressed else len(data))
    head = struct.pack('>4sHH4sIIIII', b'PSAR', 1, 4, b'zlib', toc_len, entry_size, 1, block, 0)
    path.write_bytes(head + entry + sizes + data)


class MenuPatchTests(unittest.TestCase):
    def test_eliminator_slider_goes_to_2000_in_the_same_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data09.psarc', MENU)
            [(guest, offset, old, new)] = menu_patches.eliminator_patches(folder)
            self.assertEqual(guest, '/app0/data09.psarc')
            self.assertEqual(len(old), len(new))
            raw = (Path(folder) / 'data09.psarc').read_bytes()
            self.assertEqual(raw[offset:offset + len(old)], old)
            patched = raw[:offset] + new + raw[offset + len(new):]
            self.assertIn(b'\n   <Slider name="Eliminations"', patched)
            self.assertIn(b'maxSlide="2000" default="200"', patched)

    def test_limit_takes_one_indentation_space_per_extra_digit(self):
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data09.psarc', MENU)
            [(_g, _o, old, new)] = menu_patches.eliminator_patches(folder, 750)
            self.assertEqual((len(old), new), (len(new), old.replace(b'"600"', b'"750"')))
            [(_g, _o, old, new)] = menu_patches.eliminator_patches(folder, 5000)
            self.assertEqual(new, old[1:].replace(b'"600"', b'"5000"'))
            [(_g, _o, old, new)] = menu_patches.eliminator_patches(folder, 12000)
            self.assertEqual(new, old[2:].replace(b'"600"', b'"12000"'))
            self.assertEqual(menu_patches.eliminator_patches(folder, 600), [])

    def test_compressed_or_missing_menus_are_left_alone(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(menu_patches.eliminator_patches(folder), [])
            archive(Path(folder) / 'data09.psarc', MENU, compressed=True)
            self.assertEqual(menu_patches.eliminator_patches(folder), [])


if __name__ == '__main__':
    unittest.main()
