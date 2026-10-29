"""Configuration selection never overwrites or silently ignores a player's settings."""
from paths import ROOT
import errno
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT))
import wipeout_oc_launcher_win as launcher
import graphics_options
import run


OLD_BYTES = b'# preserved\r\nrender_resolution=4K\r\noutput_res=3840x2160\r\nps4_pro=0\r\nneo=0\r\nunknown=untouched\r\n'


class ConfigFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wipeout-config-')
        self.addCleanup(self.tmp.cleanup)
        self.data = Path(self.tmp.name) / 'configuración Ω'
        self.data.mkdir()
        self.old, self.new = self.data / 'gow3.ini', self.data / 'wipeout_oc.ini'
        self.host_env = dict(os.environ)
        self.env = mock.patch.dict(os.environ, {}, clear=True)
        self.env.start()
        self.addCleanup(self.env.stop)
        self.folder = mock.patch.object(launcher, 'DATA_DIR', self.data)
        self.folder.start()
        self.addCleanup(self.folder.stop)


class ConfigPathsTests(ConfigFixture):
    def test_old_only_copies_bytes_on_first_use_and_keeps_original_independent(self):
        self.old.write_bytes(OLD_BYTES)
        self.assertEqual(launcher.ini_path(), self.new)
        self.assertEqual(self.new.read_bytes(), OLD_BYTES)
        self.new.write_bytes(b'new settings')
        self.assertEqual(self.old.read_bytes(), OLD_BYTES)

    def test_new_only_and_both_prefer_new_without_merge(self):
        self.new.write_bytes(b'new')
        for legacy in (False, True):
            with self.subTest(legacy=legacy):
                if legacy:
                    self.old.write_bytes(OLD_BYTES)
                self.assertEqual(launcher.ini_path(), self.new)
                self.assertEqual(self.new.read_bytes(), b'new')

    def test_missing_default_does_not_create_file_until_save(self):
        self.assertEqual(launcher.ini_path(), self.new)
        self.assertFalse(self.new.exists())
        launcher.save_ini({'render_resolution': '4K'}, [])
        self.assertIn('render_resolution=4K', self.new.read_text())

    def test_override_aliases_presence_empty_and_missing(self):
        self.old.write_bytes(OLD_BYTES)
        cases = (({'GOW3_CONFIG': 'legacy.ini'}, 'legacy.ini'),
                 ({'WIPEOUT_OC_CONFIG': 'canonical.ini'}, 'canonical.ini'),
                 ({'GOW3_CONFIG': 'legacy.ini', 'WIPEOUT_OC_CONFIG': 'canonical.ini'}, 'canonical.ini'),
                 ({'GOW3_CONFIG': 'legacy.ini', 'WIPEOUT_OC_CONFIG': ''}, None))
        for env, name in cases:
            with self.subTest(env=env), mock.patch.dict(os.environ, env, clear=True):
                result = launcher.ini_path()
                self.assertEqual(result, Path.cwd() / name if name else self.new)
                if name:
                    self.assertFalse(self.new.exists(), 'an explicit missing override must not migrate')

    def test_override_keeps_symlink_and_parent_components(self):
        actual = self.data / 'actual' / 'sub'
        actual.mkdir(parents=True)
        link = self.data / 'link'
        try:
            link.symlink_to(actual, target_is_directory=True)
        except OSError as error:
            self.skipTest(f'symlinks unavailable: {error}')
        (actual.parent / 'chosen.ini').write_text('engine_fps=240\n')
        relative = str(Path(os.path.relpath(link)) / '..' / 'chosen.ini')
        os.environ['WIPEOUT_OC_CONFIG'] = relative
        selected = launcher.ini_path()
        self.assertEqual(selected, Path.cwd() / relative)
        self.assertEqual(launcher.load_ini()[0]['engine_fps'], '240')

    def symlink_parent(self):
        base = self.data / 'base'
        physical = base / 'actual'
        (physical / 'sub').mkdir(parents=True)
        (base / 'link').symlink_to(physical / 'sub', target_is_directory=True)
        base.chmod(0o555)
        self.addCleanup(base.chmod, 0o755)
        return physical, base / 'link' / '..'

    @unittest.skipIf(sys.platform == 'win32', 'POSIX symlink and directory permissions')
    def test_migration_temp_uses_writable_physical_parent_without_changing_selected_path(self):
        physical, lexical = self.symlink_parent()
        (physical / 'gow3.ini').write_bytes(OLD_BYTES)
        with mock.patch.object(launcher, 'DATA_DIR', lexical):
            self.assertEqual(launcher.ini_path(), lexical / 'wipeout_oc.ini')
        self.assertEqual((physical / 'wipeout_oc.ini').read_bytes(), OLD_BYTES)
        self.assertEqual((physical / 'gow3.ini').read_bytes(), OLD_BYTES)

    @unittest.skipIf(sys.platform == 'win32', 'POSIX symlink and directory permissions')
    def test_save_temp_uses_writable_physical_parent_for_exact_override(self):
        physical, lexical = self.symlink_parent()
        os.environ['WIPEOUT_OC_CONFIG'] = str(lexical / 'chosen.ini')
        launcher.save_ini({'engine_fps': '240'}, [])
        self.assertIn('engine_fps=240', (physical / 'chosen.ini').read_text())
        self.assertEqual(launcher.ini_path(), lexical / 'chosen.ini')

    def test_ui_environment_and_run_keep_same_absolute_target_before_chdir(self):
        os.environ['WIPEOUT_OC_CONFIG'] = os.path.relpath(self.data / 'chosen.ini')
        selected = Path.cwd() / os.environ['WIPEOUT_OC_CONFIG']
        env = launcher.game_environment(launcher.APP_DEFAULTS)
        self.assertEqual(env['WIPEOUT_OC_CONFIG'], str(selected))
        cwd = Path.cwd()
        self.addCleanup(os.chdir, cwd)
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(run, 'PORT', self.data), \
                mock.patch.object(run, 'find_executable', return_value=None):
            with self.assertRaises(SystemExit):
                run.main()
            self.assertEqual(os.environ['WIPEOUT_OC_CONFIG'], str(selected))

    def test_io_errors_propagate_instead_of_defaults(self):
        self.new.mkdir()
        with self.assertRaises(OSError):
            launcher.load_ini()
        for function, args in ((graphics_options.read_ini, (self.new,)),
                               (graphics_options.write_keys, (self.new, {'engine_fps': '120'}))):
            with self.subTest(function=function.__name__), self.assertRaises(OSError):
                function(*args)

    def test_migration_permission_error_is_not_missing(self):
        self.old.write_bytes(OLD_BYTES)
        real_open = Path.open
        def denied(path, *args, **kwargs):
            if path == self.old:
                raise PermissionError(errno.EACCES, 'permission denied', str(path))
            return real_open(path, *args, **kwargs)
        with mock.patch.object(Path, 'open', denied), self.assertRaises(PermissionError):
            launcher.ini_path()
        self.assertFalse(self.new.exists())
        self.assertEqual(self.old.read_bytes(), OLD_BYTES)

    def test_interrupted_copy_removes_temporary_and_keeps_original(self):
        self.old.write_bytes(OLD_BYTES)
        with mock.patch.object(shutil, 'copyfileobj', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            launcher.ini_path()
        self.assertEqual(list(self.data.iterdir()), [self.old])
        self.assertEqual(self.old.read_bytes(), OLD_BYTES)

    def test_destination_created_during_copy_is_preserved(self):
        self.old.write_bytes(OLD_BYTES)
        copy = shutil.copyfileobj
        def concurrent(source, dest, *args):
            copy(source, dest, *args)
            self.new.write_bytes(b'independent new settings')
        with mock.patch.object(shutil, 'copyfileobj', concurrent):
            self.assertEqual(launcher.ini_path(), self.new)
        self.assertEqual(self.new.read_bytes(), b'independent new settings')
        self.assertEqual(self.old.read_bytes(), OLD_BYTES)
        self.assertEqual(len(list(self.data.iterdir())), 2)

    def test_copy_and_publish_io_failures_leave_no_partial_destination(self):
        import config_path
        self.old.write_bytes(OLD_BYTES)
        publish = 'rename' if os.name == 'nt' else 'link'
        def partial(source, dest):
            dest.write(b'partial')
            raise OSError(errno.ENOSPC, 'disk full')
        for stage, patch in (('copy', mock.patch.object(shutil, 'copyfileobj', side_effect=partial)),
                             ('flush', mock.patch.object(config_path.os, 'fsync', side_effect=OSError(errno.EIO, 'I/O error'))),
                             ('publish', mock.patch.object(config_path.os, publish, side_effect=OSError(errno.EACCES, 'denied')))):
            with self.subTest(stage=stage), patch, self.assertRaises(OSError):
                launcher.ini_path()
            self.assertFalse(self.new.exists())
            self.assertEqual(self.old.read_bytes(), OLD_BYTES)
            self.assertEqual(list(self.data.iterdir()), [self.old])

    def test_collect_reports_read_error_in_gui(self):
        app = object.__new__(launcher.Launcher)
        app.gather = mock.Mock(return_value={'ini': {}})
        app.messagebox = mock.Mock()
        with mock.patch.object(launcher, 'load_ini', side_effect=PermissionError('denied')):
            self.assertFalse(app.collect())
        app.messagebox.showerror.assert_called_once()

    def test_startup_read_error_is_shown_and_gui_is_closed(self):
        root = mock.Mock()
        root.tk.call.return_value = 'win32'
        tk = types.ModuleType('tkinter')
        tk.Tk = mock.Mock(return_value=root)
        tk.filedialog, tk.font, tk.messagebox, tk.ttk = (mock.Mock() for _ in range(4))
        with mock.patch.dict(sys.modules, {'tkinter': tk}), mock.patch.object(launcher, 'WINDOWS', True), \
                mock.patch.object(launcher.sys, 'argv', ['launcher']), \
                mock.patch.object(launcher, 'CONFIG_FILE', self.data / 'launcher.json'), \
                mock.patch.object(launcher, 'LANG', 'en'), mock.patch.object(launcher, 'FONT_LOADED', False), \
                mock.patch.object(launcher, 'register_font'), mock.patch.object(launcher, 'pick_fonts'), \
                mock.patch.object(launcher, 'load_mods'), mock.patch.object(launcher, 'windows_language', return_value='en'), \
                mock.patch.object(launcher, 'Launcher', side_effect=PermissionError('cannot read settings')):
            self.assertEqual(launcher.main(), 1)
        tk.messagebox.showerror.assert_called_once()
        root.destroy.assert_called_once()
        root.mainloop.assert_not_called()


NATIVE = ROOT / 'out/gpu' / ('settings-paths-test.exe' if sys.platform == 'win32' else 'settings-paths-test')
PROBE = ROOT / 'out' / ('gow3-probe.exe' if sys.platform == 'win32' else 'gow3-probe')


@unittest.skipUnless(NATIVE.is_file(), 'build settings-paths-test first')
class NativeConfigPathsTests(ConfigFixture):
    # Reuse the disposable Unicode directory, never the installed game or its profile.
    def process_env(self, env):
        safe = {k: v for k, v in self.host_env.items() if not k.startswith(('GOW3_', 'WIPEOUT_OC_'))}
        safe.update(WIPEOUT_OC_USER_DIR=str(self.data / 'profile'), WIPEOUT_OC_GPU_USER_DIR=str(self.data / 'profile'),
                    WIPEOUT_OC_DISCORD='0', APPDATA=str(self.data / 'profile'))
        return {**safe, **env}

    def native(self, env, *args):
        return subprocess.run([str(NATIVE), '--load', *args], cwd=self.data, env=self.process_env(env),
                              capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)

    def test_native_override_selection_and_save(self):
        self.old.write_bytes(OLD_BYTES)
        base = dict(os.environ)
        for env, selected in (({'GOW3_CONFIG': 'legacy.ini'}, self.data / 'legacy.ini'),
                              ({'WIPEOUT_OC_CONFIG': 'canonical.ini'}, self.data / 'canonical.ini'),
                              ({'GOW3_CONFIG': 'legacy.ini', 'WIPEOUT_OC_CONFIG': ''}, self.new)):
            with self.subTest(env=env):
                done = self.native({**base, **env}, '--save')
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertTrue(selected.is_file())
                self.assertEqual(self.old.read_bytes(), OLD_BYTES)

    def test_ui_to_run_to_native_save_uses_selected_override(self):
        os.environ['WIPEOUT_OC_CONFIG'] = str(self.data / 'chosen-Ω.ini')
        selected = Path(os.environ['WIPEOUT_OC_CONFIG'])
        env = launcher.game_environment(launcher.APP_DEFAULTS)
        cwd = Path.cwd()
        try:
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(run, 'PORT', self.data), \
                    mock.patch.object(run, 'find_executable', return_value=None):
                with self.assertRaises(SystemExit):
                    run.main()
                done = self.native(dict(os.environ), '--save')
        finally:
            os.chdir(cwd)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertTrue(selected.is_file())
        self.assertFalse(self.old.exists())
        self.assertFalse(self.new.exists())

    def test_native_missing_override_and_save_failure_are_explicit(self):
        self.old.write_bytes(OLD_BYTES)
        done = self.native({**os.environ, 'WIPEOUT_OC_CONFIG': str(self.data / 'missing/target.ini')}, '--save')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn('Settings: cannot write', done.stderr)
        self.assertFalse(self.new.exists())
        self.assertEqual(self.old.read_bytes(), OLD_BYTES)

    def test_native_read_failure_returns_error_without_abort(self):
        done = self.native({**os.environ, 'WIPEOUT_OC_CONFIG': str(self.data)})
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn('Settings: cannot read', done.stderr)

    def test_native_keeps_parent_components_in_selected_path(self):
        (self.data / 'sub').mkdir()
        (self.data / 'chosen.ini').write_text('engine_fps=240\n')
        relative = str(Path('sub') / '..' / 'chosen.ini')
        done = self.native({**os.environ, 'WIPEOUT_OC_CONFIG': relative})
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn(str(self.data / relative), done.stdout)

    @unittest.skipUnless(PROBE.is_file(), 'build native probe first')
    def test_native_init_failure_reaches_probe_error_without_gpu_startup(self):
        from test_probe import package
        image = self.data / 'boot.bin'
        image.write_bytes(package(b'\xc3'))
        env = {**os.environ, 'WIPEOUT_OC_CONFIG': str(self.data),
               'WIPEOUT_OC_DATA_DIR': str(self.data), 'WIPEOUT_OC_USER_DIR': str(self.data / 'profile'),
               'WIPEOUT_OC_GPU_USER_DIR': str(self.data / 'profile'), 'WIPEOUT_OC_DMEM_MB': '128',
               'WIPEOUT_OC_DISCORD': '0'}
        done = subprocess.run([str(PROBE), str(image)], cwd=self.data, env=self.process_env(env),
                              capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn('Settings: cannot read', done.stderr)
        self.assertIn('GPU initialization failed', done.stderr)


if __name__ == '__main__':
    unittest.main()
