"""Run the real package staging/refresh commands over disposable package trees."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('bash'), 'needs Bash')
class PackagePreservationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wipeout-package-fixture-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for folder in ('packaging/windows', 'out/pyenv/Scripts', 'scripts', 'patches', 'launcher', 'tools'):
            (self.root / folder).mkdir(parents=True)
        shutil.copyfile(ROOT / 'packaging/windows/package.sh', self.root / 'packaging/windows/package.sh')
        (self.root / 'msys2-env.sh').write_text(':\n')
        for name in ('out/gow3-probe.exe', 'out/gow3-gpu-capabilities.exe', 'out/gow3-play.exe',
                     'launcher/wipeout_oc.ico', 'launcher/wipeout_oc.png', 'launcher/RussoOne-Regular.ttf',
                     'launcher/OFL-RussoOne.txt', 'scripts/discord_presence.py', 'run.py', 'LICENSE',
                     'README.md', 'packaging/windows/README-Windows.txt'):
            (self.root / name).write_text('fixture\n')
        self.executable('out/pyenv/Scripts/python.exe', '''#!/usr/bin/env bash
if [[ $* == *PyInstaller* ]]; then
  mkdir -p out/pyi-dist/WipEout
  echo launcher > out/pyi-dist/WipEout/WipEout.exe
fi
''')
        self.executable('tools/llvm-strip', '''#!/usr/bin/env bash
[[ $1 == --strip-debug ]] && shift
cp -- "$3" "$2"
''')
        self.executable('tools/ldd', '#!/usr/bin/env bash\nexit 0\n')
        self.executable('tools/cygpath', '#!/usr/bin/env bash\nprintf "%s\\n" "$2"\n')
        self.executable('tools/powershell', '''#!/usr/bin/env bash
if [[ $* == *Compress-Archive* ]]; then touch ../../dist/wipeout-windows.zip; else echo 0; fi
''')

    def executable(self, relative, body):
        path = self.root / relative
        path.write_text(body, newline='\n')
        path.chmod(0o755)

    def package(self):
        env = {**os.environ, 'PATH': str(self.root / 'tools') + os.pathsep + os.environ['PATH'],
               'WINPYTHON': 'fixture', 'USERPROFILE': str(self.root)}
        # Windows Python passes POSIX-style arguments to MSYS Bash.
        return subprocess.run([shutil.which('bash'), 'packaging/windows/package.sh'], cwd=self.root,
                              env=env, capture_output=True, text=True, timeout=30)

    def test_refresh_preserves_player_files_and_removes_old_program(self):
        play = self.root / 'dist/wipeout-windows'
        for uppercase in (False, True):
            with self.subTest(uppercase=uppercase):
                shutil.rmtree(play, ignore_errors=True)
                play.mkdir(parents=True)
                names = ('user', 'mods', 'out', 'gow3.ini', 'wipeout_oc.ini', 'mods.json', 'mods_presets.json', 'patches.json')
                if uppercase:
                    names = tuple(name.upper() for name in names)
                for name in names:
                    if name.lower() in ('user', 'mods', 'out'):
                        (play / name).mkdir()
                        (play / name / 'keep').write_bytes(name.encode())
                    else:
                        (play / name).write_bytes(name.encode())
                (play / 'obsolete.exe').write_text('remove')
                result = self.package()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for name in names:
                    path = play / name / 'keep' if name.lower() in ('user', 'mods', 'out') else play / name
                    self.assertEqual(path.read_bytes(), name.encode())
                self.assertFalse((play / 'obsolete.exe').exists())
                self.assertTrue((play / 'WipEout.exe').is_file())

    def test_forbidden_guard_rejects_both_ini_names_before_zip(self):
        for name in ('gow3.ini', 'wipeout_oc.ini'):
            with self.subTest(name=name):
                bad = self.root / 'scripts' / name
                bad.write_text('private settings')
                result = self.package()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('must not ship', result.stderr)
                self.assertFalse((self.root / 'dist/wipeout-windows.zip').exists())
                bad.unlink()


if __name__ == '__main__':
    unittest.main()
