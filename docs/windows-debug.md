# Debugging on Windows

This workflow runs entirely on Windows. It needs neither WSL nor Docker. Build from a source checkout so the executable, debug symbols and source files match.

## Install the development tools

Follow the [Windows build prerequisites](../packaging/windows/README.md): MSYS2 **CLANG64**, its toolchain and libraries, Windows Python 3.10+ and the repository's submodules. In the CLANG64 terminal, also install GDB:

```bash
pacman -S --needed mingw-w64-clang-x86_64-gdb
```

In PowerShell, use your Windows Python to install the launcher and preparation dependencies:

```powershell
python -m pip install pillow iced-x86==1.21.0 cryptography
```

Keep the checkout on a Windows drive. All commands below run from that checkout in PowerShell:

```powershell
python scripts/windows_debug.py check
python scripts/windows_debug.py build
```

The script discovers MSYS2 through `MSYS2_ROOT`, Clang on `PATH`, `C:\msys64`, or the current user's `%LOCALAPPDATA%\Programs\msys64`. For another location, pass `--msys2-root 'D:\Tools\msys64'`. It uses the Python running the script; no particular Python installation directory or minor version is required.

`build` selects CMake `Debug`, disables LTO, builds the game, and runs the native save-data, JSON, services and debugger-lifetime tests. The normal `bash build.sh` build still defaults to `RelWithDebInfo`. Existing explicit definitions such as the GPU library's `NDEBUG` remain in effect.

Outputs stay in this checkout's `out/`. Do not use the packaged executable from `dist/` for source debugging: packaging strips its debug information.

## Run and attach

Start the game with your own extracted game folder:

```powershell
python scripts/windows_debug.py run --game 'D:\Games\WipEout'
```

The folder must contain `eboot.bin`. Debug saves and caches use `user-debug/` in the checkout; normal saves are not selected by this command. Game files are not copied into the repository.

While the game is running, open another PowerShell terminal in the same checkout:

```powershell
python scripts/windows_debug.py debug
```

The command attaches GDB only to the executable built in this checkout and rejects an absent or ambiguous process selection. Attaching pauses the process. Useful GDB commands:

```text
break save_set_param
continue
bt
thread apply all bt
detach
quit
```

`continue` resumes execution. `detach` resumes the game and disconnects the debugger; use it before `quit` to leave the game running. The runtime handles some memory and instruction exceptions itself, so an initial debugger stop is not sufficient evidence of a fatal crash.

GDB is used because the tested MSYS2 LLDB 23.1.3 terminated targets after reporting a successful detach. `tests/test_windows_debugger.py` compiles a disposable target and checks that GDB leaves it running after detaching. It requires neither a GPU nor game files and also runs in the Windows debug CI workflow.

## Logs and reports

`out/windows-debug/` contains `check.log`, `build.log` and `run.log`. Each command replaces its previous log; preserve it before repeating a failing run. `run.py` records the native exit status in decimal and hexadecimal. The interactive GDB console is not captured automatically.

When reporting a problem, include the commit, GPU/driver, reproduction steps, relevant log and stack trace. If Windows generated a crash dump, keep the matching executable and source checkout for analysis. Logs and dumps may contain local paths or game data; inspect them before sharing and do not commit them.

For editing in WSL2 while executing the Windows build, see the [WSL2 workflow](windows-debug-wsl.md). It uses this same script and adds source synchronization; it is optional.
