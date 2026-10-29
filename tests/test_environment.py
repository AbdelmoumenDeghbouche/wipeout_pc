"""Pure environment contracts; no launcher, native process, display or game access."""
import unittest

import paths  # noqa: F401 (puts scripts/ on sys.path)
from environment import clear_derived, normalize


class DerivedEnvironmentTests(unittest.TestCase):
    def test_legacy_derived_name_preserves_explicit_canonical_override(self):
        env = {'GOW3_DERIVED': 'GOW3_VBLANK_HZ', 'GOW3_VBLANK_HZ': '99',
               'WIPEOUT_OC_VBLANK_HZ': '144'}
        clear_derived(env)
        normalize(env)
        self.assertEqual(env, {'WIPEOUT_OC_VBLANK_HZ': '144'})

    def test_canonical_derived_name_preserves_explicit_legacy_override(self):
        env = {'WIPEOUT_OC_DERIVED': 'WIPEOUT_OC_VBLANK_HZ', 'WIPEOUT_OC_VBLANK_HZ': '99',
               'GOW3_VBLANK_HZ': '144'}
        clear_derived(env)
        normalize(env)
        self.assertEqual(env, {'WIPEOUT_OC_VBLANK_HZ': '144'})

    def test_two_restarts_preserve_unlisted_override_in_either_direction(self):
        for marker, derived, explicit in (
                ('GOW3_DERIVED', 'GOW3_VBLANK_HZ', 'WIPEOUT_OC_VBLANK_HZ'),
                ('WIPEOUT_OC_DERIVED', 'WIPEOUT_OC_VBLANK_HZ', 'GOW3_VBLANK_HZ')):
            with self.subTest(marker=marker):
                env = {marker: derived, derived: '99', explicit: '144'}
                for _ in range(2):
                    clear_derived(env)
                    normalize(env)
                    self.assertEqual(env, {'WIPEOUT_OC_VBLANK_HZ': '144'})
                    # The next run inherits a different derived default alongside the explicit rate.
                    env.update(WIPEOUT_OC_DERIVED='WIPEOUT_OC_ENGINE_FPS', WIPEOUT_OC_ENGINE_FPS='60')

    def test_canonical_marker_selects_its_list_instead_of_legacy_list(self):
        env = {'GOW3_DERIVED': 'GOW3_VBLANK_HZ', 'GOW3_VBLANK_HZ': '144',
               'WIPEOUT_OC_DERIVED': 'WIPEOUT_OC_ENGINE_FPS', 'WIPEOUT_OC_ENGINE_FPS': '99'}
        clear_derived(env)
        normalize(env)
        self.assertEqual(env, {'WIPEOUT_OC_VBLANK_HZ': '144'})

    def test_empty_canonical_marker_does_not_fall_back_to_legacy_list(self):
        env = {'GOW3_DERIVED': 'GOW3_VBLANK_HZ', 'GOW3_VBLANK_HZ': '144',
               'WIPEOUT_OC_DERIVED': ''}
        clear_derived(env)
        normalize(env)
        self.assertEqual(env, {'WIPEOUT_OC_VBLANK_HZ': '144'})
