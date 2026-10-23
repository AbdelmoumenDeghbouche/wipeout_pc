#!/usr/bin/env python3
"""Build and debug on Windows from WSL, keeping source and logs in WSL."""
import argparse
import base64
import filecmp
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
MARKER = '.wipeout-windows-debug.json'


def ps_quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def powershell(command, log=None):
    encoded = base64.b64encode(command.encode('utf-16-le')).decode('ascii')
    args = ['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded]
    if log is None:
        return subprocess.check_output(args, text=True, encoding='utf-8', errors='replace').strip()
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8') as output:
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding='utf-8', errors='replace')
        for line in process.stdout:
            print(line, end='', flush=True)
            output.write(line)
            output.flush()
        status = process.wait()
    if status:
        raise subprocess.CalledProcessError(status, ['Windows command; log:', str(log)])


def sync(root, target):
    root, target = root.resolve(), target.resolve()
    if root == target or root in target.parents or target in root.parents:
        raise ValueError('The Windows build directory must be outside the source tree')
    marker = target / MARKER
    if target.exists() and any(target.iterdir()) and not marker.is_file():
        raise ValueError('Refusing a nonempty directory not owned by windows_debug.py')
    previous = json.loads(marker.read_text()) if marker.exists() else {'source': str(root), 'files': []}
    if previous['source'] != str(root):
        raise ValueError('This Windows directory belongs to a different source checkout')
    names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '--recurse-submodules', '-z'])
    files = []
    for raw in names.split(b'\0'):
        if not raw:
            continue
        name = os.fsdecode(raw)
        source = root / name
        if not source.exists():
            continue
        if root not in source.resolve().parents:
            raise ValueError(f'Source symlink leaves the repository: {name}')
        if not source.is_file():
            raise ValueError(f'Initialize submodules before syncing: {name}')
        files.append(name)
    for name in previous['files']:
        if Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('Invalid source path in Windows mirror manifest')
    target.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        marker.write_text(json.dumps(previous))
    stamps = {}
    for name in files:
        source, dest = root / name, target / name
        info = source.stat()
        stamps[name] = [info.st_size, info.st_mtime_ns]
        # Windows mount timestamps may be rounded; retain the source's precision.
        if previous.get('stamps', {}).get(name) == stamps[name] and dest.is_file():
            continue
        if 'stamps' not in previous and dest.is_file() and filecmp.cmp(source, dest, shallow=False):
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
    for name in set(previous['files']) - set(files):
        (target / name).unlink(missing_ok=True)
    pending = target / (MARKER + '.part')
    pending.write_text(json.dumps({'source': str(root), 'files': files, 'stamps': stamps}, indent=2))
    pending.replace(marker)
    print(f'Windows mirror: {len(files)} tracked files; builds and saves preserved')


def winpath(path):
    return subprocess.check_output(['wslpath', '-w', str(Path(path).resolve())], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'sync', 'build', 'run', 'debug'))
    parser.add_argument('--windows-dir', help='Dedicated Windows mirror directory, e.g. C:\\dev\\wipeout-debug')
    parser.add_argument('--msys2-root', help='MSYS2 Windows installation directory')
    parser.add_argument('--windows-python', help='Windows python.exe path')
    parser.add_argument('--game', type=Path, help='Extracted game directory in WSL or /mnt/c')
    args = parser.parse_args()
    local = powershell('[Console]::OutputEncoding=[Text.Encoding]::UTF8; $env:LOCALAPPDATA')
    windows_dir = args.windows_dir or local + '\\wipeout-debug'
    target = Path(subprocess.check_output(['wslpath', '-u', windows_dir], text=True).strip())
    msys = args.msys2_root or local + '\\Programs\\msys64'
    python = args.windows_python or local + '\\Programs\\Python\\Python312\\python.exe'
    bash = msys + '\\usr\\bin\\bash.exe'
    setup = ('[Console]::OutputEncoding=[Text.Encoding]::UTF8; $ErrorActionPreference="Stop"; $ProgressPreference="SilentlyContinue"; '
             f'$env:MSYS2_ROOT={ps_quote(msys)}; $env:MSYSTEM="CLANG64"; '
             f'$env:PATH={ps_quote(msys + "\\clang64\\bin;" + msys + "\\usr\\bin;")}+$env:PATH; '
             '$env:PYTHONUTF8="1"; ')
    logs = ROOT / 'out' / 'windows-debug'
    if args.action == 'check':
        command = (f'& {ps_quote(bash)} -lc "set -e; clang --version; cmake --version; ninja --version; lldb --version"; '
                   'if ($LASTEXITCODE) { exit $LASTEXITCODE }; '
                   f'& {ps_quote(python)} --version; exit $LASTEXITCODE')
        powershell(setup + command, logs / 'check.log')
        return
    if args.action in ('sync', 'build'):
        sync(ROOT, target)
        print('Source commit:', subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip())
    if args.action == 'sync':
        return
    if args.action == 'build':
        shell = (f'set -e; cd "$(cygpath -u {shlex.quote(windows_dir)})"; bash build.sh; '
                 'ninja -C out/gpu savedata-test savedata-mount-test json-test services-test; '
                 'out/gpu/json-test.exe; out/gpu/services-test.exe')
        driver = target / 'out' / 'windows-debug-build.sh'
        driver.parent.mkdir(parents=True, exist_ok=True)
        driver.write_text(shell + '\n')
        driver_windows = windows_dir.replace('\\', '/') + '/out/windows-debug-build.sh'
        command = ('$env:GOW3_BUILD_TYPE="Debug"; $env:GOW3_LTO="OFF"; '
                   f'& {ps_quote(bash)} --login {ps_quote(driver_windows)}; '
                   'if ($LASTEXITCODE) { exit $LASTEXITCODE }; '
                   f'Set-Location -LiteralPath {ps_quote(windows_dir)}; '
                   f'& {ps_quote(python)} -B -m unittest discover -s tests -p "test_savedata*.py"; exit $LASTEXITCODE')
        powershell(setup + command, logs / 'build.log')
    elif args.action == 'run':
        if not args.game or not (args.game / 'eboot.bin').is_file():
            parser.error('run requires --game pointing to a directory containing eboot.bin')
        command = (f'Set-Location -LiteralPath {ps_quote(windows_dir)}; '
                   f'$env:GOW3_GAME_DIR={ps_quote(winpath(args.game))}; '
                   f'$env:GOW3_USER_DIR={ps_quote(windows_dir + "\\user-debug")}; '
                   f'& {ps_quote(python)} -u run.py; exit $LASTEXITCODE')
        powershell(setup + command, logs / 'run.log')
    else:
        executable = windows_dir + '\\out\\gow3-probe.exe'
        command = setup + f'& {ps_quote(msys + "\\clang64\\bin\\lldb.exe")} {ps_quote(executable)} -o "process attach --name gow3-probe.exe --waitfor"; exit $LASTEXITCODE'
        encoded = base64.b64encode(command.encode('utf-16-le')).decode('ascii')
        subprocess.run(['powershell.exe', '-NoProfile', '-EncodedCommand', encoded], check=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Windows debug: {error}', file=sys.stderr)
        sys.exit(1)
