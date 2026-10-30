from paths import ROOT
import os
import struct
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
from pathlib import Path

from patches import (compile_patches, external_patches, external_selection, compile_external,
                     game_profile, selected_patches, patch_requirements, GAMES, merge_patch_files,
                     resolution_names, default_enabled)

WIPEOUT = ROOT / 'patches/WipEout_Omega_Collection.xml'
WIPEOUT_OC_IDS = {'CUSA05670'}
SEGMENTS = [(0, 0x6000000)]


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA05670</ID></TitleID>
  <Metadata Title="WipEout Omega Collection" Name="On" Author="x" PatchVer="1.0" AppVer="01.07" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="WipEout Omega Collection" Name="Off" Author="x" PatchVer="1.0" AppVer="01.07" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="WipEout Omega Collection" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.07" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="WipEout Omega Collection" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA05670', 'CUSA99999'))
            (folder / 'broken.xml').write_text('<Patch>')
            # This tests serialization of a trusted synthetic reference, independently of game authorization.
            with mock.patch('patches.address_patch_problem', return_value=None):
                found = external_patches(folder, '01.07', WIPEOUT, WIPEOUT_OC_IDS)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS, allow_address=True)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS, allow_address=True)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(WIPEOUT.parent, '01.07', WIPEOUT, WIPEOUT_OC_IDS), [])


class GameProfileTests(unittest.TestCase):
    def test_profiles_by_title_id(self):
        self.assertEqual(game_profile('CUSA05670')[1:3], (WIPEOUT, '01.00'))
        self.assertIsNone(game_profile('CUSA99999'))
        self.assertIsNone(game_profile('CUSA01623', 'God of War III Remastered'))
        self.assertIsNone(game_profile(None))

    def test_unavailable_frame_rate_patch_falls_back_to_the_highest_lower_one(self):
        from graphics_options import restrict_to_available
        available = {'Bug Fix - Startup Hang', 'Frame Rate Patch - 120 FPS'}
        self.assertEqual(restrict_to_available(['Bug Fix - Startup Hang', 'Frame Rate Patch - 240 FPS'],
                                               available),
                         ['Bug Fix - Startup Hang', 'Frame Rate Patch - 120 FPS'])
        self.assertEqual(restrict_to_available(['Resolution Patch - 8K'], available), [])


    def test_profile_by_title_for_any_region(self):
        from unittest import mock
        import patches
        self.assertEqual(game_profile('CUSA07671')[1], WIPEOUT)
        self.assertEqual(game_profile('CUSA99999', 'WipEout® Omega Collection')[1], WIPEOUT)
        self.assertIsNone(game_profile('CUSA99999', 'Another Game'))
        with mock.patch.object(patches, 'game_sfo', return_value={'TITLE_ID': 'CUSA77777', 'TITLE': 'WipEout Omega Collection'}):
            self.assertTrue(patches.game_patchable('folder'))
        self.assertFalse(patches.game_patchable(None))
        with tempfile.TemporaryDirectory() as folder:
            self.assertFalse(patches.game_patchable(folder))  # no param.sfo: unknown game


class SignatureTests(unittest.TestCase):
    XML = """<?xml version="1.0"?>
<Patch>
  <Metadata Name="Found" AppVer="01.07">
    <PatchList><Line Type="bytes" Address="0x401004" Value="9090" Pattern="11 22 ?? 44 55 66" At="1"/></PatchList>
  </Metadata>
  <Metadata Name="Call" AppVer="01.07">
    <PatchList><Line Type="bytes" Address="0x401010" Value="e8{rel:there:0}" Pattern="aa bb cc dd ee" At="0"/></PatchList>
    <Target Name="there" Address="0x401100" Pattern="e9 ?? ?? ?? ?? 77 88" At="1" Ref="1"/>
  </Metadata>
  <Metadata Name="Missing" AppVer="01.07">
    <PatchList>
      <Line Type="bytes" Address="0x401004" Value="9090" Pattern="11 22 ?? 44 55 66" At="1"/>
      <Line Type="bytes" Address="0x401020" Value="90" Pattern="de ad be ef" At="0"/>
    </PatchList>
  </Metadata>
  <Metadata Name="Data" AppVer="01.07">
    <PatchList><Line Type="bytes32" Address="0x402000" Target="variable" Value="0x2"/></PatchList>
    <Target Name="variable" Address="0x402000" Pattern="44 89 35 ?? ?? ?? ?? c6" At="3" Ref="1"/>
  </Metadata>
</Patch>"""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.xml = Path(directory.name) / 'test.xml'
        self.xml.write_text(self.XML)
        image = bytearray(0x400)
        image[0x40:0x46] = bytes.fromhex('112233445566')
        image[0x80:0x85] = bytes.fromhex('aabbccddee')
        image[0x200:0x207] = bytes.fromhex('e9') + (0x300 - 0x205).to_bytes(4, 'little', signed=True) + bytes.fromhex('7788')
        # mov [rip+disp], r14d storing to 0x3f0, then the c6 of the next instruction.
        image[0x280:0x288] = bytes.fromhex('448935') + (0x3f0 - 0x287).to_bytes(4, 'little', signed=True) + b'\xc6'
        self.image = bytes(image)

    def test_sites_found_anywhere_and_rel32_recomputed(self):
        skipped = {}
        writes = compile_patches(self.xml, ['Found', 'Call', 'Missing'], None, [(0, 0x400)], self.image, skipped)
        call = (0x300 - (0x80 + 5)).to_bytes(4, 'little', signed=True)
        self.assertEqual(writes, [(0x41, b'\x90\x90'), (0x80, b'\xe8' + call)])
        # One site of the patch is missing: the whole patch is left out, with the reason.
        self.assertEqual(skipped, {'Missing': 'signature not found'})

    def test_ambiguous_signature_is_skipped(self):
        image = bytearray(self.image)
        image[0x100:0x106] = image[0x40:0x46]
        skipped = {}
        self.assertEqual(compile_patches(self.xml, ['Found'], None, [(0, 0x400)], bytes(image), skipped), [])
        self.assertEqual(skipped, {'Found': 'signature found more than once'})

    def test_without_the_image_the_reference_addresses_are_used(self):
        writes = compile_patches(self.xml, ['Found', 'Call'], '01.07', [(0, 0x10000)], allow_address=True)
        self.assertEqual(writes[0], (0x1004, b'\x90\x90'))
        self.assertEqual(writes[1][0], 0x1010)

    def test_data_line_writes_at_its_target(self):
        # The variable has no bytes of its own to find: the code that stores to it points at it.
        self.assertEqual(compile_patches(self.xml, ['Data'], None, [(0, 0x400)], self.image, {}),
                         [(0x3f0, (2).to_bytes(4, 'little'))])
        self.assertEqual(compile_patches(self.xml, ['Data'], '01.07', [(0, 0x10000)], allow_address=True),
                         [(0x2000, (2).to_bytes(4, 'little'))])
        image = bytearray(self.image)
        image[0x280:0x288] = bytes(8)  # store not found: the whole patch is left out
        skipped = {}
        self.assertEqual(compile_patches(self.xml, ['Data'], None, [(0, 0x400)], bytes(image), skipped), [])
        self.assertEqual(skipped, {'Data': 'signature not found'})


    def test_requirements_from_patch_notes(self):
        self.xml.write_text('<Patch><Metadata Name="Memory" AppVer="01.07" Note="Requires setting DMEM to 3072 MB"/>'
                            '<Metadata Name="Rate" AppVer="01.07" Note="Requires setting VBlank to 120 FPS"/></Patch>')
        self.assertEqual(patch_requirements(self.xml, ['Memory'], '01.07'), (5056 + 3072, None))
        self.assertEqual(patch_requirements(self.xml, ['Rate'], '01.07'), (None, 120))
        self.assertEqual(patch_requirements(self.xml, [], '01.07'), (None, None))


class ModPatchTests(unittest.TestCase):
    MOD = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA05670</ID></TitleID>
  <Metadata Name="Resolution Patch - 3440x1440" Note="Requires setting DMEM to 4608 MB or more." AppVer="01.07" AppElf="eboot.bin" isEnabled="false">
    <PatchList><Line Type="bytes" Pattern="90 91" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Name="Resolution Patch - 2560x1080" AppVer="01.07" AppElf="eboot.bin" isEnabled="false">
    <PatchList><Line Type="bytes" Pattern="90 91" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Name="Bug Fix - Startup Hang" Note="replaced by the mod" AppVer="01.07" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Pattern="90 91" Address="0x00401002" Value="90"/></PatchList>
  </Metadata>
</Patch>"""
    OTHER_GAME = '<Patch><TitleID><ID>CUSA99999</ID></TitleID><Metadata Name="Elsewhere" AppVer="01.07"><PatchList/></Metadata></Patch>'

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.dir = Path(directory.name)
        self.mod = self.dir / 'mod.xml'
        self.mod.write_text(self.MOD)
        sfo = mock.patch('patches.game_sfo', side_effect=lambda game: {'TITLE_ID': 'CUSA05670', 'APP_VER': '01.07'} if game == 'fixture' else {})
        sfo.start(); self.addCleanup(sfo.stop)

    def test_resolution_names_of_patch_files_smallest_first(self):
        self.assertEqual(resolution_names([self.mod], game="fixture"), ['2560x1080', '3440x1440'])
        self.assertEqual(resolution_names([self.dir / 'missing.xml'], game='fixture'), [])

    def test_merge_adds_replaces_and_skips_other_titles(self):
        other = self.dir / 'other.xml'
        other.write_text(self.OTHER_GAME)
        merged = merge_patch_files(WIPEOUT, [self.mod, other], self.dir / 'out' / 'merged.xml', game='fixture')
        names = [m.get('Name') for m in ET.parse(merged).getroot().iter('Metadata')]
        self.assertIn('Resolution Patch - 3440x1440', names)
        self.assertNotIn('Elsewhere', names)
        self.assertEqual(names.count('Bug Fix - Startup Hang'), 1)  # replaced, not repeated
        note = next(m.get('Note') for m in ET.parse(merged).getroot().iter('Metadata')
                    if m.get('Name') == 'Bug Fix - Startup Hang')
        self.assertEqual(note, 'replaced by the mod')
        # The built-in patches are all still there, and the merged file is selectable like the original.
        self.assertTrue(set(m.get('Name') for m in ET.parse(WIPEOUT).getroot().iter('Metadata')) - {'Bug Fix - Startup Hang'} <= set(names))
        chosen = selected_patches(merged, '01.00', 'Resolution Patch - 3440x1440', only=True)
        self.assertIn('Resolution Patch - 3440x1440', chosen)  # (the startup fix is always added)
        self.assertNotIn('Resolution Patch - 2560x1080', chosen)
        self.assertEqual(patch_requirements(merged, ['Resolution Patch - 3440x1440'], '01.00'), (5056 + 4608, None))

    def test_a_broken_mod_file_is_left_out(self):
        broken = self.dir / 'broken.xml'
        broken.write_text('<Patch><Metadata')
        merged = merge_patch_files(WIPEOUT, [broken, self.mod], self.dir / 'merged.xml', game='fixture')
        self.assertIn('Resolution Patch - 2560x1080',
                      [m.get('Name') for m in ET.parse(merged).getroot().iter('Metadata')])

    def test_default_enabled_patches_of_a_mod(self):
        self.assertEqual(default_enabled([self.mod], '01.07', game='fixture'), ['Bug Fix - Startup Hang'])
        self.assertEqual(default_enabled([self.mod], '01.00', game='fixture'), [])

    # --- the Ultrawide mod (extras/mods/Ultrawide): ULTRAWIDE sizes -> (extra direct memory MiB, k)

    # 16:10, taller than 16:9: only the resolution and the aspect ratio (the camera's fixed horizontal view
    # already shows more at the top and bottom), so no field of view change.


class WipeoutPatchTests(unittest.TestCase):
    """WipEout resolution, high-frame-rate fixes and unlocks."""


    def test_wipeout_profile_and_resolution_patches(self):
        self.assertEqual(game_profile('CUSA07671')[1:3], (WIPEOUT, '01.00'))
        self.assertEqual(game_profile('CUSA05670')[1], WIPEOUT)
        names = [m.get('Name') for m in ET.parse(WIPEOUT).getroot().iter('Metadata')]
        self.assertEqual(names, ['Resolution Patch - 2560x1080', 'Resolution Patch - 5120x2160',
                                 'Resolution Patch - 720p', 'Resolution Patch - 900p',
                                 'Resolution Patch - 1440p', 'Resolution Patch - 4K', 'Resolution Patch - 3440x1440',
                                 'Resolution Patch - 3840x1600', 'Resolution Patch - 5120x1440',
                                 'Native 4K rendering (no checkerboard)',
                                 'Bug Fix - Startup Hang', 'Bug Fix - Effects above 60 FPS',
                                 'Bug Fix - Zone crash above 60 FPS', 'Cheat - Unlock all HD and Fury events',
                                 'Cheat - Top rank in 2048', 'Cheat - Unlock all ships',
                                 'Cheat - Van Uber team (owned DLC)', 'Cheat - VR ships outside VR'])
        # The resolution patches stay off until a resolution is chosen; native 4K rendering is on.
        self.assertEqual(selected_patches(WIPEOUT, '01.00', ''),
                         ['Native 4K rendering (no checkerboard)', 'Bug Fix - Startup Hang',
                          'Bug Fix - Effects above 60 FPS', 'Bug Fix - Zone crash above 60 FPS'])
        self.assertTrue(compile_patches(WIPEOUT, names[1:], '01.00', SEGMENTS, allow_address=True))
        # FSR 3.1 and DLAA read the game's motion and run before its first post pass, in both
        # regions (#85: without them on the EU build, Temporal AA only turned multisampling off).
        import game_profiles
        for title in ('CUSA07671', 'CUSA05670'):
            env = game_profiles.PROFILES[title]['env']
            self.assertEqual((env['WIPEOUT_OC_GAME_MOTION'], env['WIPEOUT_OC_UPSCALE_BEFORE_PS']), ('1', '4f6cc41b'), title)

    def test_wipeout_startup_hang_fix_sets_the_flag_before_the_thread_starts(self):
        # mov [rbx],rax; mov byte [0xaef4c6],1; call 0x3d20e0; cmp byte [0xaef390],0 (#43), even when
        # the launcher's selection leaves it out.
        self.assertIn('Bug Fix - Startup Hang', selected_patches(WIPEOUT, '01.00', '', only=True))
        # The shield and effects fix (#37, #73) too: saved lists from before v0.2.27 leave it out.
        self.assertIn('Bug Fix - Effects above 60 FPS', selected_patches(WIPEOUT, '01.00', '', only=True))
        self.assertEqual(compile_patches(WIPEOUT, ['Bug Fix - Startup Hang'], '01.00', SEGMENTS, allow_address=True),
                         [(0x67a514, bytes.fromhex('488903 c605a84f470001 e8bd7bd5ff 803d664e470000'))])

    def test_wipeout_zone_fix_draws_the_last_opened_list(self):
        # #79: the render thread's step INDEX += 1 (0x9a80ac) becomes INDEX = (NEXT - 1) & 3, NEXT being
        # the game thread's next slot (0xed5dc4); the same 17 bytes, the rel32 to INDEX unchanged.
        self.assertIn('Bug Fix - Zone crash above 60 FPS', selected_patches(WIPEOUT, '01.00', '', only=True))
        self.assertEqual(compile_patches(WIPEOUT, ['Bug Fix - Zone crash above 60 FPS'], '01.00', SEGMENTS, allow_address=True),
                         [(0x665e66, bytes.fromhex('8b0558ff8600 ffc8 83e003 890535223400'))])

    def test_wipeout_cheats_are_off_unless_chosen(self):
        # The Cheats page adds them; the points total returns 99998 (mov eax; ret) and the rank table holds 0.
        self.assertNotIn('Cheat - Top rank in 2048', selected_patches(WIPEOUT, '01.00', ''))
        self.assertEqual(compile_patches(WIPEOUT, ['Cheat - Unlock all HD and Fury events', 'Cheat - Top rank in 2048'],
                                         '01.00', SEGMENTS, allow_address=True),
                         [(0x4a7200, bytes.fromhex('b89e860100c3')), (0x3f5406, bytes.fromhex('31db42895ca8fc'))])
        # The ship cheat drops the grid, medal, loyalty and play-time rules (six stores become nops; Purchase
        # stays), the 2048 ship lookup always takes its 'already open' jump (jne -> jmp), and Tigron's own
        # check (a count above 4) becomes mov r13b,1 (#35, #54).
        writes = compile_patches(WIPEOUT, ['Cheat - Unlock all ships'], '01.00', SEGMENTS, allow_address=True)
        self.assertEqual([o for o, _d in writes], [0x3f9024, 0x3f9183, 0x3f91f8, 0x3f926d, 0x3f92e5, 0x3f935d, 0x5a36ee,
                                                 0x3f9b4d])
        self.assertEqual(writes[6][1], b'\xeb')
        self.assertEqual(writes[7][1], bytes.fromhex('41b5010f1f4000'))

    def test_wipeout_native_4k_keeps_the_4k_mode_flag_false(self):
        # (field & 0xff00) > imm: 0xff00 can never be exceeded, the original 0xeff is from 3840 wide.
        self.assertEqual(compile_patches(WIPEOUT, ['Native 4K rendering (no checkerboard)'], '01.00', SEGMENTS, allow_address=True),
                         [(0x7fb659, (0xff00).to_bytes(4, 'little'))])

    def test_wipeout_resolution_patches_size_output_half_buffers_and_aspect(self):
        size = lambda value: int.from_bytes(bytes.fromhex(value), 'little')
        for meta in ET.parse(WIPEOUT).getroot().iter('Metadata'):
            if not meta.get('Name').startswith('Resolution Patch'):
                continue
            name = meta.get('Name').split(' - ')[1]
            width, height = {'720p': (1280, 720), '900p': (1600, 900), '1440p': (2560, 1440),
                             '4K': (3840, 2160)}.get(name) or map(int, name.split('x'))
            lines = {line.get('Address'): line.get('Value') for line in meta.iter('Line')}
            self.assertEqual(size(lines['0x00bfa2e8']), width, meta.get('Name'))        # output width
            self.assertEqual(size(lines['0x00bfa2f6']), height, meta.get('Name'))       # output height
            for half_width, half_height in [('0x00a44654', '0x00a44659'), ('0x00a44708', '0x00a4470f'),
                                            ('0x00a4473d', '0x00a4474a'), ('0x00a4b51b', '0x00a4b520'),
                                            ('0x00a4b52c', '0x00a4b539'), ('0x00a4b5b9', '0x00a4b5c4')]:
                self.assertEqual((size(lines[half_width]), size(lines[half_height])),
                                 (width // 2, height // 2), meta.get('Name'))
            # Both camera aspect floats, always together (one alone smears the picture); 16:9 keeps the game's.
            cameras = [a for a in ('0x00c60d08', '0x00c60d48') if a in lines]
            self.assertEqual(len(cameras), 0 if width * 9 == height * 16 else 2, meta.get('Name'))
            for address in cameras:
                aspect = struct.unpack('<f', bytes.fromhex(lines[address]))[0]
                self.assertAlmostEqual(aspect, width / height, places=5, msg=meta.get('Name'))

if __name__ == '__main__':
    unittest.main()
