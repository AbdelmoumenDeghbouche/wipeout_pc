"""Run the bridge build entry over fake headers/tools; no real SDK or toolchain is copied."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(shutil.which('bash') and (sys.platform != 'win32' or shutil.which('cygpath')),
                     'needs Bash (and MSYS cygpath on Windows)')
class DlssBuildPathTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wipeout-dlss-path-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'packaging/windows').mkdir(parents=True)
        shutil.copyfile(ROOT / 'packaging/windows/build_dlss.sh', self.root / 'packaging/windows/build_dlss.sh')
        shutil.copyfile(ROOT / 'msys2-env.sh', self.root / 'msys2-env.sh')
        self.msys = self.root / 'my MSYS2 root'
        for name in ('clang64/include/vulkan', 'clang64/include/vk_video', 'clang64/bin', 'usr/bin'):
            (self.msys / name).mkdir(parents=True)
        (self.msys / 'clang64/include/vulkan/fixture.h').write_bytes(b'custom Vulkan headers')
        (self.msys / 'clang64/include/vk_video/fixture.h').write_bytes(b'custom video headers')
        self.sdk = self.root / 'fake SDK placeholder'
        self.sdk.mkdir()
        (self.sdk / 'LICENSE.txt').write_bytes(b'fixture license only; no SDK')
        self.tools = self.root / 'fake tools'
        self.tools.mkdir()
        cmake = self.tools / 'cmake'
        cmake.write_text('''#!/usr/bin/env bash
printf '%s\n' "$@" >> "$CMAKE_LOG"
if [[ $1 == --build ]]; then
  mkdir -p out/dlss-bridge/Release
  echo fixture > out/dlss-bridge/Release/wipeout_oc_dlss.dll
fi
''', newline='\n')
        cmake.chmod(0o755)
        if sys.platform != 'win32':
            converter = self.tools / 'cygpath'
            converter.write_text('''#!/usr/bin/env bash
if [[ $1 == -u && $2 == "$FAKE_WINDOWS_MSYS_ROOT" ]]; then
  printf '%s\n' "$FAKE_POSIX_MSYS_ROOT"
else
  printf '%s\n' "$2"
fi
''', newline='\n')
            converter.chmod(0o755)

    def bash_path(self, path):
        if sys.platform != 'win32':
            return path.as_posix()
        return subprocess.check_output([shutil.which('cygpath'), '-u', str(path)], text=True).strip()

    def build(self, windows_style):
        windows_root = str(self.msys) if sys.platform == 'win32' else r'Z:\my MSYS2 root'
        env = {**os.environ, 'MSYS2_ROOT': windows_root if windows_style else self.bash_path(self.msys),
               'FAKE_WINDOWS_MSYS_ROOT': windows_root, 'FAKE_POSIX_MSYS_ROOT': self.bash_path(self.msys),
               'DLSS_SDK_ROOT': self.bash_path(self.sdk), 'DLSS_GENERATOR': 'fixture',
               'CMAKE_EXE': self.bash_path(self.tools / 'cmake'),
               'CMAKE_LOG': self.bash_path(self.root / 'cmake.log'),
               'PATH': str(self.tools) + os.pathsep + os.environ['PATH']}
        done = subprocess.run([shutil.which('bash'), (self.root / 'packaging/windows/build_dlss.sh').as_posix()],
                              cwd=self.root, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual((self.root / 'out/vkinclude/vulkan/fixture.h').read_bytes(), b'custom Vulkan headers')
        self.assertEqual((self.root / 'out/vkinclude/vk_video/fixture.h').read_bytes(), b'custom video headers')
        self.assertTrue((self.root / 'out/wipeout_oc_dlss.dll').is_file())
        self.assertFalse((self.root / 'out/gow3_dlss.dll').exists())
        self.assertEqual((self.sdk / 'LICENSE.txt').read_bytes(), b'fixture license only; no SDK')

    def test_posix_override_with_spaces(self):
        self.build(windows_style=False)

    def test_windows_override_with_spaces(self):
        self.build(windows_style=True)


if __name__ == '__main__':
    unittest.main()
