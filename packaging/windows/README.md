# WipEout Omega Collection on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 with the same sources as
on Linux. Players: see `README-Windows.txt` (it ships in the package).

## Build

For a source-debugging workflow entirely on Windows, see [Debugging on Windows](../../docs/windows-debug.md). The [WSL2 workflow](../../docs/windows-debug-wsl.md) is optional. `WIPEOUT_OC_BUILD_TYPE=Debug bash build.sh` selects a debug build; the default remains `RelWithDebInfo`.

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`):

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

magic_enum, miniz and xbyak (not packaged by MSYS2) are submodules under `gpu/third_party/`.
Before packaging, also install Windows Python 3.10+ from python.org with Tk support; the
packaging script uses it to create its private Python environment.
Then, in the repository, build with MSYS2 CLANG64:

```bash
git submodule update --init --recursive
bash build.sh                      # out/wipeout_oc-probe.exe, out/wipeout_oc-gpu-capabilities.exe
bash packaging/windows/package.sh  # dist/wipeout-windows/ and .zip: WipEout.exe + DLLs
```

For a source launcher, install Windows Python 3.10+ from python.org with Tk support. In
PowerShell at the repository root:

```powershell
python -m pip install pillow iced-x86==1.21.0 cryptography
python launcher/wipeout_oc_launcher_win.py
```

Choose the game folder in the launcher. For platform-specific tests, see
[Tests in the root README](../../README.md#tests): Windows uses Ninja
test targets; `bash build.sh --test` runs additional runtime tests only on Linux.

`run.py` is the Windows `run.sh` (same variables; works from a plain Windows Python too).
The package freezes the launcher with PyInstaller (a private venv in `out/pyenv`, from a
python.org Python) into `WipEout.exe`, which also runs `run.py` (`--run`), the preparation
scripts (`--script`) and the game without the window (`--play`): players need no Python.
The venv also receives `iced-x86`, which `scripts/isa_rewrite.py` uses for CPUs without MOVBE,
BMI1 or LZCNT.
`Play WipEout.exe` (`packaging/windows/play.c`) is a small native program that runs
`WipEout.exe --play` from its folder and waits for it, for players who want to start the
game directly with the saved settings.
`scripts/patches.py` selects the WipEout profile. Address-only patches require the exact
validated EU CUSA05670 / APP_VER 01.07 executable hash; other builds need unique safe
signatures and valid relative dependencies. Mod and external XML are checked against the
actual title ID and version before merging. A skipped patch is reported; this does not
guarantee that every game build works. See [patch compatibility](../../README.md#game-patches-and-compatibility).
From a source tree, `run.py` adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to `PATH` for DLLs.

In v0.2.49, **Graphics > Temporal anti-aliasing** offers FSR 3.1 and NVIDIA RTX DLAA/DLSS.
**Upscaling** Off uses the render resolution for native AA (DLAA with NVIDIA); Quality,
Balanced and Performance render smaller by factors 1.5, 1.7 and 2 and reconstruct the output.
HUD and menus stay at the output size. FSR 4 and the inherited TAA mode are not player options
on this path. Texture quality offers Full, maximum 4096 or maximum 2048 and applies on restart.
The 144/165 FPS choices are limits, not a promise of measured frame rates.

With supported pipeline binaries and background loading enabled, pipelines are restored on
demand. Otherwise the background path can show its loading note. Turning **Load shader cache
in the background** off waits on the loading screen for compatible saved pipelines while
retaining the **Use pipeline binary cache** setting. New areas can still need compilation.

The package ships `bin/wipeout_oc_dlss.dll` (the bridge to NVIDIA's DLSS SDK, built by
`build_dlss.sh` with MSVC) but never NVIDIA's `nvngx_dlss.dll`: the launcher's **Download NVIDIA
DLSS** button fetches it from NVIDIA's GitHub (the SDK release in `DLSS_SDK_VERSION`) and keeps it
only when its SHA-256 matches. RCAS sharpening remains available. The FSR asset tools in the tree
are development code, not player setup steps.
See [the historical research notes](../../docs/upscaler.md) for context.

The updater only notifies and opens the release page in a browser; it does not install updates.
A local Windows playtest of v0.2.49 was successful; no new FPS or HDR measurements were recorded.
See [Progress](../../README.md#progress) for scope and validation limits.

The launcher texts are written in English with the Russian beside them; the other languages
are in `launcher/wipeout_oc_lang.py` (one list per language in the order of `KEYS`; a missing or
empty text falls back to English). The icon (`launcher/wipeout_oc.ico`/`.png`) is drawn by
`packaging/windows/make_icon.py` and built into `WipEout.exe`, `wipeout_oc-probe.exe` (the game
window) and `wipeout_oc-gpu-capabilities.exe`.

## What is different from Linux

| Linux | Windows |
|---|---|
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base set with `arch_prctl` | GS is the TEB and cannot move. The loader rewrites the displacement to `gs:[0x1480 + 8*slot]` (the TEB's `TlsSlots[slot]`, same 9-byte instruction) and each guest thread keeps its TCB in that TLS slot. The slot is claimed first thing in `main` (Vulkan drivers take many). |
| One memfd for direct + flexible memory, `mmap(MAP_FIXED)` views | A pagefile-backed section; the PS4 range 64 GiB–1 TiB is reserved as one placeholder and views replace placeholders (`VirtualAlloc2`/`MapViewOfFile3`, as in shadPS4). Partial unmaps split views and restore page protections (GPU tracking). |
| `-no-pie`, heap below 1 TiB | `--disable-high-entropy-va`: heaps and thread stacks stay low and out of the PS4 range. |
| SIGSEGV handler, `sigsetjmp` recovery | Vectored exception handler; `wipeout_oc_setjmp`/`wipeout_oc_longjmp` (`src/compat_win.c`) restore all Win64 callee-saved registers without SEH unwinding (guest frames have no unwind data; clang's `__builtin_longjmp` restores a wrong frame pointer on Win64). |
| Guest thread stacks from `runtime_low_map` | Windows allocates them, fully committed (`pthread_attr_setstacksize`): guest code has no stack probes and would skip the guard page. The main thread stack is 16 MiB committed. |
| glibc `nanosleep`, monotonic condition variables | High-resolution waitable timers (`compat_sleep_ns`; winpthreads' sleep rounds to 15.6 ms); semaphore deadlines use `CLOCK_REALTIME` (winpthreads rejects monotonic condition variables). |
| `libwipeout_oc_gpu.so` | `libwipeout_oc_gpu.a`, linked into `wipeout_oc-probe.exe` by CMake (`gpu/CMakeLists.txt`), with a manifest for UTF-8 paths and long paths. |
| `execlp bash run.sh` (in-game restart) | `run.py` puts its command line in `WIPEOUT_OC_RESTART_COMMAND`; the new launch waits for the old process (`--after PID`). The launcher keeps the game in a job object. |
| Mod overlay with symlinks | Symlinks when allowed (Developer Mode), else junctions for folders and hard links for files. On a network share (SMB), which refuses junctions too, folders are mirrored with hard links so the game still reads real files through the view. |

Diagnostics added on the way: `WIPEOUT_OC_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer
and the presented image of the next frame to `WIPEOUT_OC_DUMP_DIR` (raw 32-bit pixels), which also
works for menus and movies.

Linux-specific features include watchdog/`--timeout` thread dumps (Linux signals), userfaultfd
tracking (`WIPEOUT_OC_UFFD`), MangoHud and PGO (GCC flags). Many Python and C/C++ tests run on Windows;
some tests still require Linux APIs. See the root README and `gpu/CMakeLists.txt` for test commands.
