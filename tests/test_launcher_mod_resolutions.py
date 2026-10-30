"""Render resolution: the built-in sizes plus those of the enabled mods' patch files (an ultrawide mod)."""
import unittest

from test_launcher_save import LauncherOverTemporaryFiles, launcher

PATCH = ('<Patch><TitleID><ID>CUSA05670</ID></TitleID>'
         '<Metadata Name="Resolution Patch - 3840x1080" AppVer="01.07"><PatchList><Line Type="bytes" Pattern="aa bb" Value="90"/></PatchList></Metadata></Patch>')


class ModResolutionsTest(LauncherOverTemporaryFiles):
    def test_region_mods_follow_the_game_selected_in_the_launcher(self):
        game = self.tmp / 'game'
        (game / 'JAPANESE').mkdir(parents=True)
        folder = launcher.DATA_DIR / 'mods/JP/Pack/JAPANESE'
        folder.mkdir(parents=True)
        (folder / 'text.wad').write_bytes(b'local mod')
        self.app.var('game_dir', 'app').set(str(game))
        self.app.refresh_lists()
        self.assertEqual(self.app.mod_order, ['JP'])
        self.app.select_mod('JP')
        self.assertEqual(self.app.selected_mod, 'JP')
        other = self.tmp / 'other'
        other.mkdir()
        self.app.var('game_dir', 'app').set(str(other))
        self.app.refresh_lists()
        self.assertEqual(self.app.mod_order, [])
        self.assertIsNone(self.app.selected_mod)

    def make_mod(self, name='Ultrawide'):
        folder = launcher.DATA_DIR / 'mods' / name / 'patches'
        folder.mkdir(parents=True)
        (folder / 'u.xml').write_text(PATCH)
        from test_launcher_controls import sfo_bytes
        game = self.tmp / 'game'; (game / 'sce_sys').mkdir(parents=True, exist_ok=True)
        (game / 'eboot.bin').write_bytes(b'synthetic executable for UI preflight')
        (game / 'sce_sys/param.sfo').write_bytes(sfo_bytes(TITLE_ID='CUSA05670', APP_VER='01.07'))
        self.app.var('game_dir', 'app').set(str(game))

    def sizes(self):
        return [value for value, _text in self.app.resolution_options()]

    def test_the_list_follows_the_mods(self):
        builtin = [value for value, _text in launcher.RESOLUTION_CHOICES]
        self.assertEqual(self.sizes(), builtin)  # no mod yet
        self.make_mod()
        self.app.refresh_lists()  # the Mods page lists the new folder
        self.assertEqual(self.sizes(), builtin + ['3840x1080'])
        label = dict(self.app.resolution_options())['3840x1080'][0]
        self.assertEqual(label, '3840 × 1080 · 32:9 (mod)')
        self.app.mod_vars['Ultrawide'].set(False)  # unticked on the Mods page
        self.assertEqual(self.sizes(), builtin)
        self.app.mod_vars['Ultrawide'].set(True)
        self.app.var('mods_enabled', 'app').set(False)  # "Load mods" off
        self.assertEqual(self.sizes(), builtin)

    def test_a_saved_mod_resolution_stays_chosen_while_its_mod_is_on(self):
        self.make_mod()
        self.app.refresh_lists()
        self.app.var('render_resolution', 'ini').set('3840x1080')
        self.root.update()
        self.assertEqual(self.app.var('render_resolution', 'ini').get(), '3840x1080')
        self.assertIn('3840 × 1080 · 32:9 (mod)', self.app.summary())


if __name__ == '__main__':
    unittest.main()
