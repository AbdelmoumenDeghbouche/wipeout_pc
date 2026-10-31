#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts wipeout_oc-probe (Windows and Linux; run.sh wraps it on Linux).

Environment variables (WIPEOUT_OC_GAME_DIR, WIPEOUT_OC_DATA_DIR, WIPEOUT_OC_PATCHES, WIPEOUT_OC_MODS_*,
WIPEOUT_OC_USER_DIR, ...). Extra arguments go to wipeout_oc-probe.
The in-game "Apply and restart" runs this script again through WIPEOUT_OC_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
if WINDOWS:
    import ctypes

PORT = Path(__file__).resolve().parent
sys.path.insert(0, str(PORT / "scripts"))
from environment import clear_derived, getenv, normalize
from config_path import absolute, resolve_config


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if not WINDOWS:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            except PermissionError:
                pass
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `wipeout_oc-probe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """WIPEOUT_OC_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = getenv('WIPEOUT_OC_ISA_REWRITE', env=env)
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def run_script(name, *args, capture=False, check=True):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    env = {**os.environ, 'PYTHONIOENCODING': 'utf-8'}
    clear_derived(env)
    normalize(env)
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                encoding='utf-8', env=env, creationflags=no_console())
        if not check:
            return result.returncode, result.stdout
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed launcher child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env=env, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def launch_prefix(env):
    """Linux integrations chosen in the launcher: MangoHud is its Vulkan layer (an environment
    variable), GameMode a command that starts the game."""
    if WINDOWS:
        return []
    env = normalize(env)
    if env.get('WIPEOUT_OC_MANGOHUD') == '1':
        env.setdefault('MANGOHUD', '1')
    if env.get('WIPEOUT_OC_GAMEMODE') == '1':
        if gamemode := shutil.which('gamemoderun'):
            return [gamemode]
        print('GameMode: gamemoderun not found, starting without it', flush=True)
    return []


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    env = os.environ
    clear_derived(env)
    normalize(env)
    # CONFIG is an explicit caller path; keep it stable before entering the port directory.
    if env.get('WIPEOUT_OC_CONFIG'):
        env['WIPEOUT_OC_CONFIG'] = str(absolute(env['WIPEOUT_OC_CONFIG']))
    # Other relative paths (GAME_DIR, DATA_DIR, fsr4_shaders/) remain relative to the port.
    os.chdir(PORT)
    data = Path(env.get('WIPEOUT_OC_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env['WIPEOUT_OC_CONFIG'] = str(resolve_config(data, env))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    derived = []
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    try:
        import discord_presence
    except Exception:
        discord_presence = None
    import mods
    import patches
    # The enabled mods' patch files: their "Resolution Patch - <size>" entries are render resolutions too
    # (the launcher and the in-game menu list them; WIPEOUT_OC_EXTRA_RESOLUTIONS tells the game).
    mod_patch_files = mods.enabled_patch_files(env.get('WIPEOUT_OC_MODS_DIR', data / 'mods'),
                                               env.get('WIPEOUT_OC_MODS_CONFIG', data / 'mods.json'),
                                               env.get('WIPEOUT_OC_MODS_ENABLED', '1') != '0',
                                               env.get('WIPEOUT_OC_GAME_DIR', PORT.parent / 'CUSA05670'))
    patch_game = env.get('WIPEOUT_OC_PATCH_DIR') or env.get('WIPEOUT_OC_GAME_DIR', PORT.parent / 'CUSA05670')
    # Include WipEout profile sizes beyond the shared resolution list (#110).
    import game_profiles
    own = game_profiles.PROFILES.get(patches.game_title_id(patch_game), {})
    own = own.get('env', {}).get('WIPEOUT_OC_ALLOWED_RESOLUTIONS', '').split(';')
    extra_resolutions = list(dict.fromkeys(s for s in [*patches.resolution_names(mod_patch_files, game=patch_game), *own]
                                           if s and s not in graphics_options.RESOLUTIONS))
    env['WIPEOUT_OC_EXTRA_RESOLUTIONS'] = ';'.join(extra_resolutions)
    derived.append('WIPEOUT_OC_EXTRA_RESOLUTIONS')
    graphics, reverted = graphics_options.prepare(env['WIPEOUT_OC_CONFIG'], out / 'graphics_state.json', extra_resolutions)
    if reverted:
        print('Graphics: the last restart never reached the game; previous options restored', flush=True)
    env.update(graphics_options.environment(graphics))
    if not env.get('WIPEOUT_OC_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['WIPEOUT_OC_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['WIPEOUT_OC_PROBE']) if env.get('WIPEOUT_OC_PROBE') else find_executable('wipeout_oc-probe' + EXE_SUFFIX)
    if not probe or not probe.is_file():
        fail(f'wipeout_oc-probe{EXE_SUFFIX} not found: build it with build.sh or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    game = Path(env.get('WIPEOUT_OC_GAME_DIR', PORT.parent / 'CUSA05670'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set WIPEOUT_OC_GAME_DIR).')
    update_view = None
    unlinked = {}  # large files a drive without links kept out of the view: the game reads them in place
    if env.get('WIPEOUT_OC_PATCH_DIR'):
        import game_update
        try:
            update_view = game_update.create_view(game, env['WIPEOUT_OC_PATCH_DIR'], out, unlinked)
        except (ValueError, OSError) as error:
            fail(str(error))
        game = update_view
        print(f'Game and update combined from {env["WIPEOUT_OC_PATCH_DIR"]}', flush=True)
        if unlinked:
            print(f'Game and update: this drive cannot link files; {len(unlinked)} large files are read '
                  'from their folders', flush=True)
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('WIPEOUT_OC_MODS_DIR', data / 'mods'),
                           '--config', env.get('WIPEOUT_OC_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('WIPEOUT_OC_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        import game_profiles
        profile = game_profiles.profile(game)
        if profile['name']:
            print(f'Game profile: {profile["name"]} ({", ".join(profile["modules"])})', flush=True)
        for name, value in profile.get('env', {}).items():
            env.setdefault(name, value)
        run_script('link_modules.py', game, '--out', out, '--modules', *profile['modules'],
                   '--reserve', str(profile.get('reserve', 0)))
        if WINDOWS:  # function starts for the loader's red-zone protection (src/redzone.cpp)
            run_script('function_starts.py', game, '--out', out, '--modules', *profile['modules'])
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            code, output = run_script('isa_rewrite.py', out / 'boot-linked.bin', '--out', out,
                                      capture=True, check=False)
            print(output, end='', flush=True)
            if code == 0:
                isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('WIPEOUT_OC_CONTENT_SKU', 'full'))
        run_script('menu_patches.py', game, '--out', out,  # Eliminator target slider limit (#89)
                   '--limit', env.get('WIPEOUT_OC_ELIMINATOR_LIMIT', '2000'))
        env['WIPEOUT_OC_FILE_PATCHES'] = str(out / 'file_patches.txt')

        from patches import game_profile_for, patch_requirements, patch_status, selected_patches
        profile = game_profile_for(game)
        engine_fps = 60
        # The window shows the game's icon (BMP: the format SDL loads without extra libraries); on
        # Linux the launcher's own icon (the AppImage ships the one chosen when it was built) and
        # an application id the desktop entry matches, so the shell shows the same icon.
        icon_file = game / 'sce_sys' / 'icon0.png'
        if not WINDOWS:
            env.setdefault('SDL_APP_ID', 'wipeout_oc')
            if (PORT / 'launcher' / 'wipeout_oc.png').is_file():
                icon_file = PORT / 'launcher' / 'wipeout_oc.png'
        try:
            from PIL import Image
            with Image.open(icon_file) as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('WIPEOUT_OC_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # The temporal upscalers and motion vectors are not calibrated for this game yet: they stay
        # off unless the launcher's experimental switch is on (then gow3.ini decides); linear
        # image readbacks fix its corrupted textures.
        if env.get('WIPEOUT_OC_EXPERIMENTAL_UPSCALER') != '1':
            env.setdefault('WIPEOUT_OC_UPSCALER', 'none')
        else:
            # FSR 3.1 / DLAA smooth the edges instead of the game's multisampling; with both on, a
            # race showed another image in the upper-left quarter (NVIDIA, where it is on by default).
            env['WIPEOUT_OC_NO_MSAA'] = '1'
        env.setdefault('WIPEOUT_OC_READBACK_LINEAR', '1')
        patch_xml = profile[1] if profile else None
        if profile:
            patch_elf = (out / 'eboot.elf').read_bytes()
            patch_image, patch_segments = patches.eboot_image(patch_elf), patches.eboot_segments(patch_elf)
            # A mod offered by structural UI checks may not match this executable's signatures.
            extra_resolutions = [size for size in patches.resolution_names(mod_patch_files, game=game,
                                 image=patch_image, segments=patch_segments) if size not in graphics_options.RESOLUTIONS]
            env['WIPEOUT_OC_EXTRA_RESOLUTIONS'] = ';'.join(extra_resolutions)
            graphics = graphics_options.startup_options(graphics, extra_resolutions)
            env.update(graphics_options.environment(graphics))
        if profile and mod_patch_files:
            # The built-in patches plus the enabled mods' (the same file for the selection and the notes).
            patch_xml = patches.merge_patch_files(profile[1], mod_patch_files, out / 'patches-merged.xml',
                                                         game=game, image=patch_image, segments=patch_segments)
        if profile:
            names = selected_patches(patch_xml, profile[2], env.get('WIPEOUT_OC_PATCHES', ''), env.get('WIPEOUT_OC_PATCHES_ONLY') == '1')
            # A mod's patches that are on by default (isEnabled) stay on when the launcher chose the list.
            names += [n for n in patches.default_enabled(mod_patch_files, patches.game_app_version(game), game=game,
                                                        image=patch_image, segments=patch_segments) if n not in names]
            # gow3.ini decides the resolution and frame rate patches (in-game menu, launcher).
            names = graphics_options.patch_names(names, graphics)
            names = graphics_options.restrict_to_available(names, patches.available_patches(patch_xml, profile[2]))
            env['WIPEOUT_OC_PATCHES'] = ';'.join(names)
            env['WIPEOUT_OC_PATCHES_ONLY'] = '1'
        (out / 'patch_status.json').unlink(missing_ok=True)
        run_script('patches.py', '--out', out, '--extra', env.get('WIPEOUT_OC_PATCHES', ''), '--game-dir', game,
                   '--patches-dir', env.get('WIPEOUT_OC_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('WIPEOUT_OC_PATCHES_CONFIG', data / 'patches.json'),
                   *[arg for f in mod_patch_files for arg in ('--mod-patches', f)])
        if profile:
            # Only the patches found in this eboot.bin count: a frame rate patch that was skipped
            # leaves the engine at 60 FPS, and the notes' memory and VBlank needs follow the same list.
            applied = (patch_status(out) or {}).get('applied', [])
            engine_fps = graphics_options.applied_engine_fps(applied)
            dmem, vblank = patch_requirements(patch_xml, applied, profile[2])
            for key, value in (('WIPEOUT_OC_DMEM_MB', dmem), ('WIPEOUT_OC_VBLANK_HZ', vblank)):
                if value and key not in env:
                    env[key] = str(value)
                    derived.append(key)
        if 'WIPEOUT_OC_VBLANK_HZ' not in env:
            env['WIPEOUT_OC_VBLANK_HZ'] = '60'
            derived.append('WIPEOUT_OC_VBLANK_HZ')
        # Without the frame rate patch the engine steps 1/60 s per frame: the present thread must
        # not flip faster (wipeout_oc_graphics.h PresentLimit), or the game speeds up. A game whose
        # engine steps by the real time (its profile sets the rate) keeps the profile's.
        if 'WIPEOUT_OC_ENGINE_FPS' not in env:
            env['WIPEOUT_OC_ENGINE_FPS'] = str(engine_fps)
            derived.append('WIPEOUT_OC_ENGINE_FPS')
        engine_fps = int(env['WIPEOUT_OC_ENGINE_FPS'])
        print(f'Engine frame rate: {engine_fps} FPS'
              + ('' if engine_fps > 60 or graphics['engine_fps'] == '60' else
                 ' (the frame rate patch was not found in this eboot.bin)'), flush=True)
        mount_args = []
        for link, target in unlinked.items():
            mount_args += ['--mount', f'/app0/{Path(link).relative_to(update_view).as_posix()}={target}']
        env['WIPEOUT_OC_DERIVED'] = ','.join(derived)
        # The guest memory pool is committed in full at start: a resolution patch above 1080p
        # can need more than 16 GB of RAM, and the game then closes when the page file cannot grow.
        import guest_memory
        if warning := guest_memory.memory_warning(env.get('WIPEOUT_OC_DMEM_MB'), guest_memory.physical_memory_mb(),
                                                  env.get('WIPEOUT_OC_STREAM_MEMORY', 'cached'),
                                                  neo=env.get('WIPEOUT_OC_NEO', '').startswith('1')):
            print(warning, flush=True)

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('WIPEOUT_OC_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        (user_dir / 'music').mkdir(exist_ok=True)  # custom soundtrack (src/runtime_music.c)
        env.setdefault('WIPEOUT_OC_GPU_USER_DIR', str(user_dir))
        from save_backup import backup
        try:
            if made := backup(user_dir, savedata=env.get('WIPEOUT_OC_SAVEDATA_DIR')):
                print(f'Saves backed up to {made}', flush=True)
        except OSError as error:
            print(f'Save backup failed, starting anyway: {error}', flush=True)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        # Windows: the probe starts the new launch and ends, so it waits for this process. Linux:
        # the probe replaces itself with the new launch while this process still waits for it.
        after = ['--after', str(os.getpid())] if WINDOWS else []
        restart = [sys.executable, *this, *after, *args]
        env['WIPEOUT_OC_RESTART_COMMAND'] = subprocess.list2cmdline(restart) if WINDOWS else shlex.join(restart)
        command = [*launch_prefix(env), str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('WIPEOUT_OC_TIMEOUT', '0'), *isa_args, *mount_args, *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, creationflags=no_console())
        presence = None
        try:
            try:
                presence = discord_presence.Presence(pid=process.pid) if discord_presence else None
            except (OSError, RuntimeError):
                print('Discord presence unavailable; the game will continue', flush=True)
            status = process.wait()
            print(f'Game process exited: {status} (0x{status & 0xffffffff:08X})', flush=True)
            return status
        finally:
            if presence:
                presence.close()
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)
        if update_view:
            game_update.remove_view(update_view)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, UnicodeError) as error:
        fail(f'Settings/startup: {error}')
