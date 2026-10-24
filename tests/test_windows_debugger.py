"""A debugger detach must leave the native Windows target alive."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(sys.platform == 'win32' and shutil.which('gdb') and shutil.which('clang'),
                     'requires native Windows GDB and Clang')
class WindowsDebuggerTests(unittest.TestCase):
    def test_detach_keeps_the_target_running(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, executable = root / 'target.c', root / 'target.exe'
            source.write_text('#include <windows.h>\nint main(void) { Sleep(INFINITE); return 0; }\n')
            subprocess.run(['clang', '-g', str(source), '-o', str(executable)], check=True,
                           capture_output=True, timeout=30)
            target = subprocess.Popen([str(executable)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                result = subprocess.run(['gdb', '--batch', '-nx', '-q', '-p', str(target.pid),
                                         '-ex', 'thread apply all bt', '-ex', 'detach'],
                                        capture_output=True, text=True, errors='replace', timeout=45)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                try:
                    code = target.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
                else:
                    self.fail(f'Debugger killed its target: {code:#x}\n{result.stdout}\n{result.stderr}')
            finally:
                if target.poll() is None:
                    target.terminate()
                    target.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
