"""Linux entry points reuse existing data in place and choose the new name on first install."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('vulkan_data_test', ROOT / 'launcher/gow3_vulkan.py')
vulkan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vulkan)


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux data entry points')
class DataDirectoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wipeout-data-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.xdg = self.root / 'xdg'
        self.xdg.mkdir()
        self.app = self.root / 'app'
        self.share = self.app / 'usr/share/gow3'
        (self.share / 'scripts').mkdir(parents=True)
        shutil.copyfile(ROOT / 'scripts/environment.sh', self.share / 'scripts/environment.sh')
        shutil.copyfile(ROOT / 'packaging/linux/AppRun', self.app / 'AppRun')
        launcher = self.share / 'gow3-launcher'
        launcher.write_text('#!/usr/bin/env bash\nprintf "%s" "$WIPEOUT_OC_DATA_DIR"\n'
                            '[[ ${PRINT_CONFIG:-} == 1 ]] && printf "|%s" "${WIPEOUT_OC_CONFIG-<missing>}"\nexit 0\n')
        launcher.chmod(0o755)

    def entry(self, env, nix=False):
        env = {k: v for k, v in {**os.environ, **env}.items()
               if not k.startswith(('GOW3_', 'WIPEOUT_OC_')) or k in env}
        env['XDG_DATA_HOME'] = str(self.xdg)
        if nix:
            text = (ROOT / 'packaging/default.nix').read_text()
            command = re.findall(r"--run '([^\n]+)'", text)[-1].replace("''${", '${')
            command = command.replace('$out/share/gow3', str(self.share))
            args = [shutil.which('bash'), '-c', 'source "$1"; ' + command + '; printf "%s" "$WIPEOUT_OC_DATA_DIR"',
                    'nix-data-test', str(self.share / 'scripts/environment.sh')]
        else:
            args = [shutil.which('bash'), str(self.app / 'AppRun')]
        done = subprocess.run(args, env=env, cwd=self.root, text=True, capture_output=True, timeout=10)
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout

    def test_appimage_and_nix_default_selection(self):
        for nix in (False, True):
            for existing, wanted in (((), 'wipeout_oc'), (('gow3',), 'gow3'),
                                     (('wipeout_oc',), 'wipeout_oc'),
                                     (('gow3', 'wipeout_oc'), 'wipeout_oc')):
                with self.subTest(nix=nix, existing=existing):
                    for child in self.xdg.iterdir():
                        shutil.rmtree(child)
                    for name in existing:
                        (self.xdg / name).mkdir()
                        (self.xdg / name / 'save').write_text(name)
                    self.assertEqual(self.entry({}, nix), str(self.xdg / wanted))
                    for name in existing:
                        self.assertEqual((self.xdg / name / 'save').read_text(), name)

    def test_entry_explicit_aliases_and_empty(self):
        for nix in (False, True):
            for env, wanted in (({'GOW3_DATA_DIR': 'old'}, 'old'),
                                ({'GOW3_DATA_DIR': 'old', 'WIPEOUT_OC_DATA_DIR': 'new'}, 'new'),
                                ({'GOW3_DATA_DIR': 'old', 'WIPEOUT_OC_DATA_DIR': ''}, '')):
                with self.subTest(nix=nix, env=env):
                    self.assertEqual(self.entry(env, nix), wanted)

    def test_appimage_anchors_selected_config_before_changing_cwd(self):
        for env, wanted in (({'GOW3_CONFIG': 'sub/../old.ini'}, str(self.root / 'sub/../old.ini')),
                            ({'GOW3_CONFIG': 'old.ini', 'WIPEOUT_OC_CONFIG': 'sub/../new.ini'}, str(self.root / 'sub/../new.ini')),
                            ({'GOW3_CONFIG': 'old.ini', 'WIPEOUT_OC_CONFIG': ''}, '')):
            with self.subTest(env=env):
                self.assertEqual(self.entry({**env, 'PRINT_CONFIG': '1'}).split('|')[1], wanted)

    def test_run_wrapper_anchors_config_before_changing_cwd(self):
        shutil.copyfile(ROOT / 'run.sh', self.share / 'run.sh')
        python = self.root / 'python'
        python.write_text('#!/usr/bin/env bash\nprintf "%s" "${WIPEOUT_OC_CONFIG-<missing>}"\n')
        python.chmod(0o755)
        for env, wanted in (({'GOW3_CONFIG': 'sub/../old.ini'}, str(self.root / 'sub/../old.ini')),
                            ({'GOW3_CONFIG': 'old.ini', 'WIPEOUT_OC_CONFIG': 'sub/../new.ini'}, str(self.root / 'sub/../new.ini')),
                            ({'GOW3_CONFIG': 'old.ini', 'WIPEOUT_OC_CONFIG': ''}, '')):
            with self.subTest(env=env):
                inherited = {k: v for k, v in os.environ.items() if not k.startswith(('GOW3_', 'WIPEOUT_OC_'))}
                inherited.update(env, PYTHON=str(python), WIPEOUT_OC_PREBUILT='1')
                done = subprocess.run([shutil.which('bash'), str(self.share / 'run.sh')], cwd=self.root,
                                      env=inherited, capture_output=True, text=True, timeout=10)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(done.stdout, wanted)

    def test_vulkan_cache_uses_the_same_selection(self):
        driver = self.root / 'libGLX_nvidia.so.0'
        driver.write_bytes(b'fixture')
        for existing, wanted in (((), 'wipeout_oc'), (('gow3',), 'gow3'),
                                 (('gow3', 'wipeout_oc'), 'wipeout_oc')):
            with self.subTest(existing=existing):
                for child in self.xdg.iterdir():
                    shutil.rmtree(child)
                for name in existing:
                    (self.xdg / name).mkdir()
                env = {'XDG_DATA_HOME': str(self.xdg)}
                from unittest import mock
                with mock.patch.object(vulkan, 'host_nvidia', return_value=({'ICD': {}}, driver)):
                    vulkan.configure(env, (), ())
                self.assertTrue((self.xdg / wanted / 'vulkan/nvidia').is_dir())
                self.assertEqual(sorted(p.name for p in self.xdg.iterdir()), sorted(set(existing) | {wanted}))


if __name__ == '__main__':
    unittest.main()
