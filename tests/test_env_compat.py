"""Environment compatibility at the launcher, restart and native process boundaries."""
from paths import ROOT
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'launcher'))
import run
import wipeout_oc_launcher_win as launcher
from test_probe import package
from environment import clear_derived, normalize


DERIVED_INPUTS = (
    ({'GOW3_DERIVED': 'GOW3_VBLANK_HZ', 'GOW3_VBLANK_HZ': '99'}, None),
    ({'GOW3_DERIVED': 'GOW3_VBLANK_HZ', 'GOW3_VBLANK_HZ': '99',
      'WIPEOUT_OC_VBLANK_HZ': '144'}, '144'),
    ({'WIPEOUT_OC_DERIVED': 'WIPEOUT_OC_VBLANK_HZ', 'WIPEOUT_OC_VBLANK_HZ': '99',
      'GOW3_VBLANK_HZ': '144'}, '144'),
    ({'WIPEOUT_OC_DERIVED': '', 'GOW3_DERIVED': 'GOW3_VBLANK_HZ',
      'GOW3_VBLANK_HZ': '144'}, '144'),
)


class EnvironmentCompatibilityTests(unittest.TestCase):
    def test_isa_override_prefers_new_even_zero_or_empty(self):
        for env, wanted in (({}, True), ({'GOW3_ISA_REWRITE': '0'}, False),
                            ({'WIPEOUT_OC_ISA_REWRITE': '0'}, False),
                            ({'GOW3_ISA_REWRITE': '1', 'WIPEOUT_OC_ISA_REWRITE': '0'}, False),
                            ({'GOW3_ISA_REWRITE': '0', 'WIPEOUT_OC_ISA_REWRITE': ''}, True)):
            with self.subTest(env=env):
                self.assertIs(run.isa_rewrite_wanted(['MOVBE'], env), wanted)

    def test_import_time_data_dir_prefers_new_by_presence(self):
        code = (f'import sys; sys.path.insert(0, {str(ROOT / "launcher")!r}); '
                'import wipeout_oc_launcher_win as app; print(app.DATA_DIR)')
        for old, new, wanted in ((None, None, str(ROOT)), ('old', None, 'old'), (None, 'new', 'new'),
                                 ('old', 'new', 'new'), ('old', '', '.'), ('old', '0', '0')):
            env = dict(os.environ)
            for key in ('GOW3_DATA_DIR', 'WIPEOUT_OC_DATA_DIR'):
                env.pop(key, None)
            if old is not None:
                env['GOW3_DATA_DIR'] = old
            if new is not None:
                env['WIPEOUT_OC_DATA_DIR'] = new
            result = subprocess.run([sys.executable, '-B', '-c', code], env=env, capture_output=True,
                                    text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), wanted)

    def test_ui_off_and_empty_controls_cannot_inherit_either_prefix(self):
        suffixes = ('HDR', 'FRAME_STATS', 'GPU_PROFILE', 'VK_VALIDATION', 'FRAME_CAPTURE_KEY',
                    'UPSCALER_KEY', 'KEY_MAP', 'PAD_MAP', 'PATCH_DIR', 'MUSIC_DIR', 'GPU_ID',
                    'GLYPH_STYLE', 'NO_MSAA', 'DRAW_PIPE', 'READBACKS', 'USER_DIR')
        inherited = {prefix + key: '1' for prefix in ('GOW3_', 'WIPEOUT_OC_') for key in suffixes}
        settings = {**launcher.APP_DEFAULTS, 'game_dir': '', 'user_dir': '', 'hdr': True,
                    'controls': {'key': {'cross': 'Q'}, 'pad': {'cross': 'b'}}}
        diagnostic_flags = ('frame_stats', 'gpu_profile', 'vk_validation', 'frame_capture_key', 'upscaler_key')
        settings.update({key: True for key in diagnostic_flags})
        with mock.patch.dict(os.environ, inherited):
            enabled = launcher.game_environment(settings)
            self.assertEqual(enabled.get('WIPEOUT_OC_HDR'), '1')
            self.assertIn('cross=Q', enabled.get('WIPEOUT_OC_KEY_MAP', '').split(','))
        settings.update({key: False for key in diagnostic_flags})
        settings.update(hdr=False, controls={'key': {
            'cross': 'Space', 'circle': 'Left Shift', 'square': 'E', 'triangle': 'Q',
            'l1': '1', 'r1': '3', 'l2': 'R', 'r2': 'F', 'l3': 'Z', 'r3': 'C',
            'options': 'Return', 'touchpad_left': 'Tab', 'touchpad_right': 'Backspace',
            'up': 'I', 'down': 'K', 'left': 'J', 'right': 'L'}})
        with mock.patch.dict(os.environ, enabled, clear=True):
            disabled = launcher.game_environment(settings)
        for suffix in suffixes:
            with self.subTest(suffix=suffix):
                self.assertNotIn('GOW3_' + suffix, disabled)
                self.assertNotIn('WIPEOUT_OC_' + suffix, disabled)

    def test_ui_ingress_cleans_derived_before_alias_normalization(self):
        for inherited, wanted in DERIVED_INPUTS:
            with self.subTest(inherited=inherited), mock.patch.dict(os.environ, inherited, clear=True):
                env = launcher.game_environment({**launcher.APP_DEFAULTS, 'game_dir': ''})
                self.assertEqual(env.get('WIPEOUT_OC_VBLANK_HZ'), wanted)
                clear_derived(env)
                normalize(env)
                self.assertEqual(env.get('WIPEOUT_OC_VBLANK_HZ'), wanted)

    def test_run_script_ingress_cleans_derived_before_alias_normalization(self):
        for inherited, wanted in DERIVED_INPUTS:
            with self.subTest(inherited=inherited), mock.patch.dict(os.environ, inherited, clear=True), \
                    mock.patch.object(run.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout='fixture')) as child:
                self.assertEqual(run.run_script('fixture.py', capture=True), 'fixture')
                env = child.call_args.kwargs['env']
                self.assertEqual(env.get('WIPEOUT_OC_VBLANK_HZ'), wanted)
                self.assertNotIn('WIPEOUT_OC_DERIVED', env)
                self.assertNotIn('GOW3_DERIVED', env)

    def test_vulkan_entry_cleans_derived_before_alias_normalization(self):
        import wipeout_oc_vulkan
        for inherited, wanted in DERIVED_INPUTS:
            with self.subTest(inherited=inherited):
                env = dict(inherited)
                wipeout_oc_vulkan.configure(env, manifest_dirs=(), library_dirs=())
                self.assertEqual(env.get('WIPEOUT_OC_VBLANK_HZ'), wanted)
                self.assertNotIn('WIPEOUT_OC_DERIVED', env)
                self.assertNotIn('GOW3_DERIVED', env)

    def test_derived_values_are_recomputed_across_two_restarts(self):
        cases = (
            ({'GOW3_DERIVED': 'GOW3_DMEM_MB,GOW3_VBLANK_HZ,GOW3_ENGINE_FPS',
              'GOW3_DMEM_MB': '99', 'GOW3_VBLANK_HZ': '99', 'GOW3_ENGINE_FPS': '99'}, '60'),
            ({'GOW3_DERIVED': 'GOW3_DMEM_MB,GOW3_VBLANK_HZ,GOW3_ENGINE_FPS',
              'GOW3_DMEM_MB': '99', 'GOW3_VBLANK_HZ': '99', 'GOW3_ENGINE_FPS': '99',
              'WIPEOUT_OC_VBLANK_HZ': '144'}, '144'),
            ({'WIPEOUT_OC_DERIVED': 'WIPEOUT_OC_DMEM_MB,WIPEOUT_OC_ENGINE_FPS,WIPEOUT_OC_VBLANK_HZ',
              'WIPEOUT_OC_DMEM_MB': '99', 'WIPEOUT_OC_ENGINE_FPS': '99',
              'WIPEOUT_OC_VBLANK_HZ': '99', 'GOW3_VBLANK_HZ': '144'}, '144'),
        )
        for overrides, wanted_vblank in cases:
            with self.subTest(overrides=overrides):
                with tempfile.TemporaryDirectory() as tmp:
                    folder = Path(tmp)
                    game = folder / 'game'
                    game.mkdir()
                    (game / 'eboot.bin').write_bytes(b'fixture')
                    probe = folder / 'probe'
                    probe.touch()
                    env = {'GOW3_GAME_DIR': str(game), 'GOW3_DATA_DIR': str(folder),
                           'GOW3_DISCORD': '0'}
                    env.update(overrides)
                    previous = Path.cwd()
                    self.addCleanup(os.chdir, previous)
                    process = mock.Mock(pid=123)
                    process.wait.return_value = 0
                    def preparation(name, *args, **kwargs):
                        return str(game) if name == 'mods.py' else None
                    with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(run, 'PORT', folder), \
                            mock.patch.object(run, 'find_executable', return_value=probe), \
                            mock.patch.object(run, 'run_script', side_effect=preparation), \
                            mock.patch.object(run, 'glyph_mounts', return_value=([], None)), \
                            mock.patch.object(run, 'pause_menu_args', return_value=[]), \
                            mock.patch.object(run.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout='CPU: complete\n')), \
                            mock.patch.object(run.subprocess, 'Popen', return_value=process), \
                            mock.patch.object(sys, 'argv', ['run.py']):
                        for _ in range(2):
                            self.assertEqual(run.main(), 0)
                            self.assertNotIn('GOW3_DERIVED', os.environ)
                            self.assertNotIn('GOW3_DMEM_MB', os.environ)
                            self.assertNotIn('WIPEOUT_OC_DMEM_MB', os.environ)
                            self.assertEqual(os.environ.get('WIPEOUT_OC_VBLANK_HZ'), wanted_vblank)
                            self.assertEqual(os.environ.get('WIPEOUT_OC_ENGINE_FPS'), '60')
                            if wanted_vblank == '144':
                                self.assertNotIn('WIPEOUT_OC_VBLANK_HZ', os.environ['WIPEOUT_OC_DERIVED'].split(','))
                            self.assertTrue(all(name.startswith('WIPEOUT_OC_') for name in
                                                os.environ['WIPEOUT_OC_DERIVED'].split(',')))
                    os.chdir(previous)


EXE = ROOT / 'out' / ('gow3-probe.exe' if sys.platform == 'win32' else 'gow3-probe')


@unittest.skipUnless(EXE.is_file(), 'build native probe first')
class NativeEnvironmentTests(unittest.TestCase):
    def test_c_reader_and_gpu_paths_before_main_use_presence_precedence(self):
        cases = ((None, None, 0, 'user'), ('1', None, 1, 'old'), (None, '1', 1, 'new'),
                 ('0', '1', 1, 'new'), ('1', '0', 0, '0'), ('1', '', 0, 'new'))
        for old, new, neo, directory in cases:
            with self.subTest(old=old, new=new), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                for name in ('old', 'new', 'user', '0', 'xdg'):
                    (folder / name).mkdir()
                env = {k: v for k, v in os.environ.items() if not k.startswith(('GOW3_', 'WIPEOUT_OC_'))}
                env.update(GOW3_DMEM_MB='128', XDG_DATA_HOME=str(folder / 'xdg'))
                if old is not None:
                    env.update(GOW3_NEO=old, GOW3_GPU_USER_DIR=str(folder / 'old'))
                if new is not None:
                    env.update(WIPEOUT_OC_NEO=new,
                               WIPEOUT_OC_GPU_USER_DIR=str(folder / 'new') if new in ('1', '') else new)
                # Call the real C Neo-mode service; a wrong result executes UD2 instead of the terminal import.
                code = (b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\x83\xf8' +
                        bytes([neo]) + b'\x74\x02\x0f\x0b\xff\x25\x0d\0\0\0')
                image = folder / 'boot.bin'
                image.write_bytes(package(code, [(32, 1, 0, 0), (40, 1, 1, 0)],
                                          ['WslcK1FQcGI#libkernel', 'env-fixture-done'], 1))
                result = subprocess.run([str(EXE), str(image), '--cpu-only'], cwd=folder, env=env,
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
                self.assertIn('env-fixture-done', result.stdout)
                selected = folder / directory if directory != 'empty' else folder / 'xdg/shadPS4'
                self.assertTrue((selected / 'custom_trophy/Notice.txt').is_file(), selected)
                if directory != 'old':
                    self.assertFalse((folder / 'old/custom_trophy/Notice.txt').exists())


    def test_empty_canonical_gpu_path_has_the_same_policy_as_empty_legacy(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            old = folder / 'old'
            old.mkdir()
            exits = []
            for canonical in (False, True):
                work = folder / ('canonical' if canonical else 'legacy')
                work.mkdir()
                (work / 'xdg').mkdir()
                env = {key: value for key, value in os.environ.items()
                       if not key.startswith(('GOW3_', 'WIPEOUT_OC_'))}
                env['XDG_DATA_HOME'] = str(work / 'xdg')
                env['GOW3_GPU_USER_DIR'] = str(old) if canonical else ''
                if canonical:
                    env['WIPEOUT_OC_GPU_USER_DIR'] = ''
                done = subprocess.run([str(EXE), '--cpu-features'], cwd=work, env=env,
                                      capture_output=True, text=True, timeout=20)
                exits.append(done.returncode)
            self.assertEqual(exits[1], exits[0])
            self.assertFalse((old / 'custom_trophy/Notice.txt').exists())


class ShellEnvironmentTests(unittest.TestCase):
    def test_shell_ingress_cleans_derived_before_alias_normalization(self):
        script = ('source "$1"; printf "%s|%s|%s" "${WIPEOUT_OC_VBLANK_HZ-<missing>}" '
                  '"${WIPEOUT_OC_DERIVED-<missing>}" "${GOW3_DERIVED-<missing>}"')
        for inherited, wanted in DERIVED_INPUTS:
            with self.subTest(inherited=inherited):
                env = {key: value for key, value in os.environ.items()
                       if not key.startswith(('GOW3_', 'WIPEOUT_OC_'))}
                env.update(inherited)
                done = subprocess.run([shutil.which('bash'), '-c', script, 'environment-test',
                                       (ROOT / 'scripts/environment.sh').as_posix()], env=env,
                                      capture_output=True, text=True, timeout=10)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(done.stdout, ('<missing>' if wanted is None else wanted) + '|<missing>|<missing>')

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux shell entry point')
    def test_run_wrapper_consumes_legacy_and_exports_only_canonical(self):
        with tempfile.TemporaryDirectory() as tmp:
            python = Path(tmp) / 'python'
            python.write_text('#!/usr/bin/env bash\nprintf "%s|%s\\n" "${WIPEOUT_OC_DATA_DIR-<missing>}" "${GOW3_DATA_DIR-<missing>}"\n')
            python.chmod(0o755)
            for old, new, wanted in ((None, None, '.'), ('old', None, 'old'),
                                     (None, 'new', 'new'), ('old', 'new', 'new'),
                                     ('old', '', '.'), ('old', '0', '0')):
                with self.subTest(old=old, new=new):
                    env = {k: v for k, v in os.environ.items() if not k.startswith(('GOW3_', 'WIPEOUT_OC_'))}
                    env.update(GOW3_PREBUILT='1', WIPEOUT_OC_PREBUILT='1', PYTHON=str(python))
                    if old is not None:
                        env['GOW3_DATA_DIR'] = old
                    if new is not None:
                        env['WIPEOUT_OC_DATA_DIR'] = new
                    done = subprocess.run(['bash', str(ROOT / 'run.sh')], env=env,
                                          capture_output=True, text=True, timeout=10)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    self.assertEqual(done.stdout.strip(), wanted + '|<missing>')


class FsrPatchCompatibilityTests(unittest.TestCase):
    def test_patch_transitions_preserve_local_edits_and_reject_conflicts(self):
        import hashlib
        patches = sorted((ROOT / 'gpu/patches/fsr-vulkan').glob('*.patch'))
        files = set()
        for patch in patches:
            for line in patch.read_text().splitlines():
                if line.startswith('+++ b/'):
                    files.add(line[6:])
        source = ROOT / 'gpu/third_party/fsr-vulkan'
        for state in ('fresh', 'old-applied', 'old-applied-conflict'):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as tmp:
                target = Path(tmp)
                for name in files:
                    path = target / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes((source / name).read_bytes())
                # A Windows source mirror has no .git; reconstruct only this disposable fixture.
                for patch in reversed(patches):
                    check = subprocess.run(['git', '-C', str(target), 'apply', '--reverse', '--check', str(patch)],
                                           capture_output=True)
                    if check.returncode == 0:
                        subprocess.run(['git', '-C', str(target), 'apply', '--reverse', str(patch)], check=True)
                provider = target / 'src/ffx_vk_fsr4_v07.c'
                with provider.open('ab') as output:
                    output.write(b'\n/* local edit outside patch contexts */\n')
                marker = target / 'local-edit.txt'
                marker.write_text('keep this user file')
                if state != 'fresh':
                    for patch in patches:
                        if not patch.name.startswith('0004-'):
                            subprocess.run(['git', '-C', str(target), 'apply', str(patch)], check=True)
                if state == 'old-applied-conflict':
                    provider.write_bytes(provider.read_bytes().replace(
                        b'#include <stdlib.h>', b'#include <stdlib.h> /* local conflicting edit */', 1))
                    before = {str(path.relative_to(target)): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in target.rglob('*') if path.is_file()}
                    done = subprocess.run([shutil.which('bash'),
                                           (ROOT / 'scripts/apply_fsr_patches.sh').as_posix(), target.as_posix()],
                                          capture_output=True, text=True, timeout=20)
                    self.assertNotEqual(done.returncode, 0)
                    after = {str(path.relative_to(target)): hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in target.rglob('*') if path.is_file()}
                    self.assertEqual(after, before)  # includes any unexpected .rej or partial file
                    continue
                def apply():
                    done = subprocess.run([shutil.which('bash'), (ROOT / 'scripts/apply_fsr_patches.sh').as_posix(), target.as_posix()],
                                          capture_output=True, text=True, timeout=20)
                    self.assertEqual(done.returncode, 0, done.stderr)
                apply()
                before = {name: hashlib.sha256((target / name).read_bytes()).hexdigest() for name in files}
                apply()
                self.assertEqual(before, {name: hashlib.sha256((target / name).read_bytes()).hexdigest() for name in files})
                self.assertEqual(marker.read_text(), 'keep this user file')
                self.assertTrue(provider.read_text().endswith('/* local edit outside patch contexts */\n'))
                current = ROOT / 'gpu/patches/fsr-vulkan/0004-port-environment-compatibility.patch'
                subprocess.run(['git', '-C', str(target), 'apply', '--reverse', '--check', str(current)], check=True)


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux AppImage build wrapper')
class NixWrapperEnvironmentTests(unittest.TestCase):
    def test_wrapper_passes_presence_selected_fsr_models_flag_explicitly(self):
        import shutil
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('packaging', 'scripts', 'out/gpu', 'bin'):
                (root / name).mkdir(parents=True)
            for name in ('packaging/appimage.sh', 'scripts/environment.sh'):
                shutil.copy2(ROOT / name, root / name)
            for name in ('out/gow3-probe', 'out/gpu/libgow3gpu.so'):
                (root / name).touch()
            readelf = root / 'bin/readelf'
            readelf.write_text('#!/usr/bin/env bash\nprintf "Library runpath: [/nix/store/fixture/lib]\\n"\n')
            nix = root / 'bin/nix'
            nix.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@" > "$NIX_ARGS_FILE"\ntouch gow3.AppImage\n')
            readelf.chmod(0o755)
            nix.chmod(0o755)
            for old, new, wanted in ((None, None, 'false'), ('1', None, 'true'),
                                     (None, '1', 'true'), ('1', '', 'false'), ('1', '0', 'false')):
                with self.subTest(old=old, new=new):
                    env = {k: v for k, v in os.environ.items() if not k.startswith(('GOW3_', 'WIPEOUT_OC_'))}
                    args = root / 'args'
                    env.update(PATH=str(root / 'bin') + os.pathsep + env['PATH'], NIX_ARGS_FILE=str(args))
                    if old is not None:
                        env['GOW3_PACKAGE_FSR411'] = old
                    if new is not None:
                        env['WIPEOUT_OC_PACKAGE_FSR411'] = new
                    done = subprocess.run(['bash', str(root / 'packaging/appimage.sh')], env=env,
                                          capture_output=True, text=True, timeout=10)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    self.assertIn('packageFsr411 = ' + wanted + ';', args.read_text())
