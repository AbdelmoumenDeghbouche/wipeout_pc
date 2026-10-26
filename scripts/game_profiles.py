"""Per-game settings, by title id in sce_sys/param.sfo (after bc160201983's MediEvil port).

modules: bundled sce_module PRX files linked into the image (link_modules.py), in order.
env: default environment for the run (run.py); a value already set wins.
"""
from pathlib import Path

PROFILES = {
    # WipEout Omega Collection. NpToolkit2 is the game's own PSN toolkit. Red-zone protection
    # always: the startup probe sees no overwrite, but 2048 races run at 0.3 FPS without it (a
    # loop at 0x40ff50 keeps its count below the stack pointer, under the GPU write faults).
    # The engine steps by the time between frames (rdtsc), so it runs at any frame rate: vblank
    # at 240 Hz, and the frame rate limit (launcher, in-game menu) caps it.
    # Vblank stays at the console's 60 Hz: the menu waits one vblank per frame, so a higher
    # rate ran it that many times faster (240 Hz = 4x). The race engine steps by rdtsc, not
    # vblank, so GOW3_ENGINE_FPS=240 + a Frame rate limit above 60 still gets high-FPS races:
    # the frame limit turns on immediate flips, which flip above the vblank rate on demand.
    # GOW3_ALLOWED_RESOLUTIONS: only these built-in sizes have a resolution patch in
    # patches/WipEout_Omega_Collection.xml; the other built-ins are hidden from the in-game menu.
    # GOW3_HUD_16X9: on a render resolution wider than 16:9 the HUD stays 16:9 in the middle
    # (the HUD is laid out for 1920x1080; vk_rasterizer.cpp PrepareRenderState).
    # GOW3_GAME_MOTION / GOW3_UPSCALE_BEFORE_PS (temporal upscalers, still behind the experimental
    # switch): the main scene pass writes the game's own motion vectors, and the upscale runs
    # before 4f6cc41b, the first pass that reads the finished scene color (a downsample, then the
    # exposure chain). The motion blur (f136b842) is too late: it reads a copy made in between
    # (ps 1ba93f5d). Found on CUSA07671 01.07 only. GOW3_OBJECT_MOTION=0: God of War III's object
    # motion (instrumented vertex shaders, extra pipeline variants) is not read with the game's
    # own vectors; it only cost first-start compiles and GPU time.
    'CUSA07671': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'GOW3_RED_ZONE': '1', 'GOW3_ENGINE_FPS': '240', 'GOW3_HUD_16X9': '1',
                          'GOW3_ALLOWED_RESOLUTIONS': 'native;1440p;4K;2560x1080;3440x1440;5120x1440;5120x2160',
                          'GOW3_GAME_MOTION': '1', 'GOW3_UPSCALE_BEFORE_PS': '4f6cc41b',
                          'GOW3_OBJECT_MOTION': '0'}},
    'CUSA05670': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'GOW3_RED_ZONE': '1', 'GOW3_ENGINE_FPS': '240', 'GOW3_HUD_16X9': '1',
                          'GOW3_ALLOWED_RESOLUTIONS': 'native;1440p;4K;2560x1080;3440x1440;5120x1440;5120x2160'}},
    'CUSA01623': {'name': 'God of War III', 'modules': ('libc.prx', 'libSceFios2.prx')},
}
DEFAULT = {'name': None, 'modules': ('libc.prx', 'libSceFios2.prx')}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def profile(game):
    """The profile of a game folder (DEFAULT for unknown games)."""
    found = PROFILES.get(title_id(game) or '', DEFAULT)
    # Only modules the dump actually ships are linked.
    modules = tuple(m for m in found['modules'] if (Path(game) / 'sce_module' / m).is_file())
    return {**found, 'modules': modules}
