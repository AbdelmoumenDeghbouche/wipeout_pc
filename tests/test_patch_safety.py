"""Only resolved signatures or the verified EU build may supply patch destinations."""
from paths import ROOT
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import patches

SHA = '3a52e910f6c450cc2f0c85ef994be5d8c951c326a362bcc4775f29e6ff7f644c'
EU = {'TITLE_ID': 'CUSA05670', 'APP_VER': '01.07', 'TITLE': 'WipEout Omega Collection'}


class PatchSafetyTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.image = b'\0' * 4 + b'\xaa\xbb' + b'\0' * 10
        self.segments = [(0, len(self.image))]

    def xml(self, body, name='test.xml', version='01.00', patch='Safe', ids='CUSA05670'):
        path = self.root / name
        path.write_text(f'<Patch><TitleID><ID>{ids}</ID></TitleID><Metadata Name="{patch}" AppVer="{version}" '
                        f'Note="{name}" AppElf="eboot.bin" isEnabled="true">{body}</Metadata></Patch>')
        return path

    def compile(self, body, image=None):
        skipped = {}
        writes = patches.compile_patches(self.xml(body), ['Safe'], '01.00', self.segments,
                                         self.image if image is None else image, skipped)
        return writes, skipped

    def table(self, **attributes):
        attrs = {'Target': 'rows', 'Size': '16', 'Entry': '8', 'Count': '4', 'Refs': '1', **attributes}
        body = '<Target Name="rows" Pattern="48 8d 05 ?? ?? ?? ??" At="3" Ref="1"/>'
        body += '<Table ' + ' '.join(f'{k}="{v}"' for k, v in attrs.items()) + '/>'
        body += '<PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/></PatchList>'
        image = bytearray(64); image[:7] = bytes.fromhex('488d05') + struct.pack('<i', 128 - 7)
        image[16:18] = bytes.fromhex('aabb')
        return body, bytes(image)

    def test_table_resolves_dependency_not_used_by_a_line(self):
        body, image = self.table()
        writes = patches.compile_patches(self.xml(body), ['Safe'], '01.00', [(0, 64), (128, 144)],
                                         image, {}, reserve=(256, 40))
        self.assertEqual(writes, [(16, b'\x90'), (3, struct.pack('<i', 257))])

    def test_invalid_table_rejects_whole_patch_including_other_lines(self):
        cases = [({'Size': '-16'}, (256, 40)), ({'Entry': '0'}, (256, 40)),
                 ({'Count': '-1'}, (256, 40)), ({'Count': '1'}, (256, 40)),
                 ({'Size': '17'}, (256, 40)), ({'Refs': '0'}, (256, 40)),
                 ({'Refs': '2'}, (256, 40)), ({'Count': str(1 << 63)}, (256, 40)),
                 ({}, (128, 40)), ({}, (-1, 40)), ({}, (256, 32)), ({}, (1 << 63, 40))]
        for attrs, reserve in cases:
            with self.subTest(attrs=attrs, reserve=reserve):
                body, image = self.table(**attrs); skipped = {}
                writes = patches.compile_patches(self.xml(body), ['Safe'], '01.00', [(0, 64), (128, 144)],
                                                 image, skipped, reserve=reserve)
                self.assertEqual(writes, [])
                self.assertIn('Safe', skipped)

    def test_two_tables_cannot_share_the_single_reserve(self):
        body, image = self.table(); image = bytearray(image)
        image[8:15] = bytes.fromhex('488d0d') + struct.pack('<i', 160 - 15)
        body += '<Target Name="other" Pattern="48 8d 0d ?? ?? ?? ??" At="3" Ref="1"/>'
        body += '<Table Target="other" Size="16" Entry="8" Count="4" Refs="1"/>'
        skipped = {}
        writes = patches.compile_patches(self.xml(body), ['Safe'], '01.00', [(0, 64), (128, 176)],
                                         bytes(image), skipped, reserve=(256, 40))
        self.assertEqual(writes, [])
        self.assertIn('Safe', skipped)

    def test_two_selected_patches_cannot_own_the_same_reserve(self):
        body, image = self.table(); image = bytearray(image)
        image[8:15] = bytes.fromhex('488d0d') + struct.pack('<i', 160 - 15)
        first = self.xml(body); tree = ET.parse(first)
        other = ET.fromstring(ET.tostring(next(tree.getroot().iter('Metadata'))))
        other.set('Name', 'Other')
        other.find('Target').set('Pattern', '48 8d 0d ?? ?? ?? ??')
        other.find('PatchList/Line').set('Value', '91')
        tree.getroot().append(other); tree.write(first)
        skipped = {}
        writes = patches.compile_patches(first, ['Safe', 'Other'], '01.00', [(0, 64), (128, 176)],
                                         bytes(image), skipped, reserve=(256, 40))
        self.assertEqual(writes, [(16, b'\x90'), (3, struct.pack('<i', 257))])
        self.assertIn('Other', skipped)

    def test_invalid_table_mod_cannot_replace_valid_builtin(self):
        body, image = self.table(Count='-1')
        base = self.xml('<PatchList><Line Type="bytes" Pattern="aa bb" Value="91"/></PatchList>', 'base.xml')
        mod = self.xml(body, 'mod.xml', '01.07'); skipped = {}
        with mock.patch.object(patches, 'game_sfo', return_value=EU), \
                mock.patch.object(patches, 'eboot_sha256', return_value=None):
            out = patches.merge_patch_files(base, [mod], self.root / 'merged.xml', game=self.root,
                                            image=image, segments=[(0, 64), (128, 144)], skipped=skipped)
        meta = next(ET.parse(out).getroot().iter('Metadata'))
        self.assertEqual(meta.get('Note'), 'base.xml')
        self.assertTrue(any(key.endswith('mod.xml/Safe') for key in skipped))

    def test_table_rejects_source_outside_load_segments(self):
        body, image = self.table(); skipped = {}
        writes = patches.compile_patches(self.xml(body), ['Safe'], '01.00', [(0, 64), (128, 136)],
                                         image, skipped, reserve=(256, 40))
        self.assertEqual(writes, [])
        self.assertIn('Safe', skipped)

    def test_address_requires_all_three_verified_identity_fields(self):
        for fields, sha, allowed in [(EU, SHA, True), ({**EU, 'TITLE_ID': 'CUSA07671'}, SHA, False),
                                     ({**EU, 'APP_VER': '01.00'}, SHA, False),
                                     ({**EU, 'APP_VER': None}, SHA, False),
                                     ({**EU, 'TITLE_ID': None}, SHA, False), (EU, None, False),
                                     (EU, '0' * 64, False),
                                     (EU, 'd85c8135d330c3b601bf5dc3f1dd86bd6119fb8a9fce372bed2c9785f9a79299', False)]:
            with self.subTest(fields=fields, sha=sha), mock.patch.object(patches, 'game_sfo', return_value=fields), \
                    mock.patch.object(patches, 'eboot_sha256', return_value=sha):
                reason = patches.address_patch_problem('game')
                self.assertEqual(reason is None, allowed)
                if not allowed: self.assertIn('Address', reason)

    def test_hash_does_not_reuse_permission_after_same_size_and_mtime_replacement(self):
        game = self.root / 'game'; game.mkdir(); eboot = game / 'eboot.bin'
        eboot.write_bytes(b'first'); stamp = eboot.stat()
        first = patches.eboot_sha256(game)
        eboot.write_bytes(b'other'); os.utime(eboot, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertNotEqual(patches.eboot_sha256(game), first)

    def test_main_preserves_builtin_applied_and_reports_rejected_mod_separately(self):
        base = self.xml('<PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/></PatchList>', 'base.xml')
        out = self.root / 'out'; out.mkdir()
        elf = bytearray(0x100 + len(self.image))
        struct.pack_into('<Q', elf, 0x20, 0x40)
        struct.pack_into('<HH', elf, 0x36, 56, 1)
        struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 5, 0x100, 0, 0, len(self.image), len(self.image), 1)
        elf[0x100:] = self.image; (out / 'eboot.elf').write_bytes(elf)
        for patch, line in [('Safe', '<Line Type="mask" Value="90 ??"/>'),
                            ('Unique', '<Line Type="bytes" Pattern="dd ee" Value="91"/>')]:
            with self.subTest(patch=patch):
                mod = self.xml('<PatchList>' + line + '</PatchList>', 'mod.xml', '01.07', patch=patch)
                args = ['patches.py', '--game-dir', str(self.root), '--out', str(out),
                        '--extra', 'Safe;Unique', '--mod-patches', str(mod)]
                with mock.patch.object(patches, 'game_sfo', return_value=EU), \
                        mock.patch.object(patches, 'eboot_sha256', return_value=None), \
                        mock.patch.object(patches, 'game_profile_for', return_value=({'CUSA05670'}, base, '01.00', 'WipEout')), \
                        mock.patch.dict(os.environ, GOW3_PATCHES_ONLY='1'), mock.patch('sys.argv', args), \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    patches.main()
                status = json.loads((out / 'patch_status.json').read_text())
                self.assertEqual(status['applied'], ['Safe'])
                self.assertTrue(any(key.endswith('mod.xml/' + patch) for key in status['mod_skipped']))
                expected = struct.pack('<8sQQQQ', b'G3PATCH2', 0x400000, 1, 4, 1) + b'\x90'
                self.assertEqual((out / 'patches.bin').read_bytes(), expected)

    def test_unknown_address_is_skipped_even_when_pattern_patch_is_safe(self):
        body = '<PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/>' \
               '<Line Type="bytes" Address="0x400008" Value="91"/></PatchList>'
        writes, skipped = self.compile(body)
        self.assertEqual(writes, [])
        self.assertIn('Address', skipped['Safe'])

    def test_safe_target_has_no_provisional_address_and_chains_existing_line_references(self):
        body = '<Target Name="A" Line="1"/><Target Name="B" Line="2"/><PatchList>' \
               '<Line Type="bytes" Target="A" Value="90"/><Line Type="bytes" Target="B" Value="91"/>' \
               '<Line Type="bytes" Pattern="aa bb" Value="92"/></PatchList>'
        self.assertEqual(self.compile(body), ([(4, b'\x90'), (4, b'\x91'), (4, b'\x92')], {}))

    def test_unused_address_target_does_not_make_safe_pattern_depend_on_it(self):
        body = '<Target Name="unused" Address="0x400008"/><PatchList>' \
               '<Line Type="bytes" Pattern="aa bb" Value="90"/></PatchList>'
        self.assertEqual(self.compile(body), ([(4, b'\x90')], {}))

    def test_bad_dependencies_skip_whole_patch(self):
        for targets, line, reason in [('<Target Name="X" Line="0"/>', 'Target="X"', 'cycl'),
                                     ('<Target Name="X" Line="9"/>', 'Target="X"', 'Line'),
                                     ('', 'Target="absent"', 'absent'),
                                     ('<Target Name="X" Address="0x400008"/>', 'Target="X"', 'Address')]:
            with self.subTest(targets=targets):
                body = targets + f'<PatchList><Line Type="bytes" {line} Value="90"/></PatchList>'
                writes, skipped = self.compile(body)
                self.assertEqual(writes, [])
                self.assertIn(reason, skipped['Safe'])

    def test_missing_ambiguous_and_outside_sites_are_atomic(self):
        for pattern, at, reason in [('dd ee', '0', 'not found'), ('00 00', '0', 'more than once'),
                                    ('aa bb', '100', 'outside')]:
            with self.subTest(pattern=pattern, at=at):
                body = '<PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/>' \
                       f'<Line Type="bytes" Pattern="{pattern}" At="{at}" Value="91"/></PatchList>'
                writes, skipped = self.compile(body)
                self.assertEqual(writes, [])
                self.assertIn(reason, skipped['Safe'])

    def test_unsupported_placeholder_kind_cannot_reuse_an_already_resolved_target(self):
        body = '<Target Name="X" Line="1"/><PatchList><Line Type="bytes" Target="X" Value="90"/>' \
               '<Line Type="bytes" Pattern="aa bb" Value="e9{abs:X:0}"/></PatchList>'
        writes, skipped = self.compile(body)
        self.assertEqual(writes, [])
        self.assertIn('unsupported', skipped['Safe'])

    def test_relative_target_outside_load_segments_is_rejected(self):
        self.image = b'\xe8' + struct.pack('<i', 100) + b'\xee' + bytes(10)
        body = '<Target Name="X" Pattern="e8 ?? ?? ?? ?? ee" At="1" Ref="1"/><PatchList>' \
               '<Line Type="bytes" Pattern="e8 ?? ?? ?? ?? ee" Value="e9{rel:X:0}"/></PatchList>'
        writes, skipped = self.compile(body)
        self.assertEqual(writes, [])
        self.assertIn('outside', skipped['Safe'])

    def test_mod_validation_precedes_same_name_replacement(self):
        base = self.xml('<PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/></PatchList>', 'base.xml')
        invalid = [('<Line Type="bytes16" Pattern="aa bb" Value="0x10000"/>', '01.07', 'CUSA05670'),
                   ('<Line Type="bytes32" Pattern="aa bb" Value="-1"/>', '01.07', 'CUSA05670'),
                   ('<Line Type="bytes" Pattern="aa bb" Value=""/>', '01.07', 'CUSA05670'),
                   ('<Line Type="bytes" Pattern="aa bb"/>', '01.07', 'CUSA05670'),
                   ('<Line Type="mask" Value="90 ??"/>', '01.07', 'CUSA05670'),
                   ('<Line Type="bytes" Address="0x400004" Value="91"/>', '01.07', 'CUSA05670'),
                   ('<Line Type="bytes" Pattern="dd ee" Value="91"/>', '01.07', 'CUSA05670'),
                   ('<Line Type="bytes" Pattern="aa bb" Value="91"/>', '01.00', 'CUSA05670'),
                   ('<Line Type="bytes" Pattern="aa bb" Value="91"/>', '01.07', 'CUSA07671')]
        for line, version, ids in invalid:
            with self.subTest(line=line, version=version, ids=ids), \
                    mock.patch.object(patches, 'game_sfo', return_value=EU), \
                    mock.patch.object(patches, 'eboot_sha256', return_value=None):
                mod = self.xml('<PatchList>' + line + '</PatchList>', 'mod.xml', version, ids=ids)
                log = io.StringIO()
                with contextlib.redirect_stderr(log):
                    merged = patches.merge_patch_files(base, [mod], self.root / 'merged.xml', game='game',
                                                       image=self.image, segments=self.segments)
                self.assertEqual(ET.parse(merged).find('.//Metadata').get('Note'), 'base.xml')
                self.assertIn('mod.xml', log.getvalue())
                self.assertEqual(patches.compile_patches(merged, ['Safe'], '01.00', self.segments,
                                                         self.image, {}), [(4, b'\x90')])

    def test_accepted_real_version_mod_uses_builtin_tag_in_private_merged_copy(self):
        base = self.xml('<PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/></PatchList>', 'base.xml')
        mod = self.xml('<PatchList><Line Type="bytes" Pattern="aa bb" Value="91"/></PatchList>',
                       'mod.xml', '01.07')
        original = mod.read_bytes()
        with mock.patch.object(patches, 'game_sfo', return_value=EU):
            merged = patches.merge_patch_files(base, [mod], self.root / 'merged.xml', game='game',
                                               image=self.image, segments=self.segments)
        self.assertEqual(mod.read_bytes(), original)
        self.assertEqual(ET.parse(merged).find('.//Metadata').get('AppVer'), '01.00')
        self.assertEqual(patches.compile_patches(merged, ['Safe'], '01.00', self.segments, self.image, {}),
                         [(4, b'\x91')])

    def test_ui_preflight_and_runtime_resolution_share_mod_eligibility(self):
        mod = self.xml('<PatchList><Line Type="bytes" Pattern="dd ee" Value="91"/></PatchList>',
                       'resolution.xml', '01.07', patch='Resolution Patch - 3440x1440')
        with mock.patch.object(patches, 'game_sfo', return_value=EU):
            # The UI checks identity and syntax; only the runner has the actual prepared image.
            self.assertEqual(patches.resolution_names([mod], game='game'), ['3440x1440'])
            self.assertEqual(patches.resolution_names([mod], game='game', image=self.image,
                                                      segments=self.segments), [])
            self.assertEqual(patches.default_enabled([mod], '01.07', game='game', image=self.image,
                                                     segments=self.segments), [])
        mod = self.xml('<PatchList><Line Type="bytes" Address="0x400004" Value="91"/></PatchList>',
                       'resolution.xml', '01.07', patch='Resolution Patch - 3440x1440')
        with mock.patch.object(patches, 'game_sfo', return_value=EU), \
                mock.patch.object(patches, 'eboot_sha256', return_value=None):
            self.assertEqual(patches.resolution_names([mod], game='game'), [])
            self.assertEqual(patches.default_enabled([mod], '01.07', game='game'), [])

    def test_external_metadata_uses_real_version_and_same_address_policy(self):
        self.xml('<PatchList><Line Type="bytes" Address="0x400004" Value="90"/></PatchList>',
                 'extra.xml', '01.07')
        with mock.patch.object(patches, 'game_sfo', return_value=EU), \
                mock.patch.object(patches, 'eboot_sha256', return_value=None):
            self.assertEqual(patches.external_patches(self.root, '01.07', ROOT/'patches/WipEout_Omega_Collection.xml',
                                                      {'CUSA05670'}, game='game'), [])
        self.xml('<PatchList><Line Type="bytes" Pattern="aa bb" Value="91"/></PatchList>',
                 'extra.xml', '01.07')
        with mock.patch.object(patches, 'game_sfo', return_value=EU):
            found = patches.external_patches(self.root, '01.07', ROOT/'patches/WipEout_Omega_Collection.xml',
                                              {'CUSA05670'}, game='game')
        self.assertEqual(len(found), 1)
        self.assertEqual(patches.compile_external(found, self.segments, image=self.image), [(4, b'\x91')])


if __name__ == '__main__': unittest.main()
