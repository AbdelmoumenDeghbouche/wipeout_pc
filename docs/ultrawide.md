# WipEout render resolutions and ultrawide

Choose **Render resolution** in the launcher or the in-game menu (Insert / R3+L2), then
restart the game to apply it. The built-in patches are in
[WipEout_Omega_Collection.xml](../patches/WipEout_Omega_Collection.xml).

| Size | Aspect |
|---|---|
| 1280 × 720 | 16:9 |
| 1600 × 900 | 16:9 |
| 1920 × 1080 (default) | 16:9 |
| 2560 × 1440 | 16:9 |
| 3840 × 2160 | 16:9 |
| 2560 × 1080 | 21:9 |
| 3440 × 1440 | 21:9 |
| 3840 × 1600 | 2.4:1 |
| 5120 × 2160 | 21:9 |
| 5120 × 1440 | 32:9 |

The WipEout patch notes describe matching output and half-resolution buffers. Wider
patches also change the camera aspect ratio for a wider horizontal view; HUD and menus
stay in a centered 16:9 area.
These are patch definitions, not a claim that every size, track or GPU was playtested.
4K and 5120 × 2160 require substantial video memory; see [Progress](../README.md#progress).

The local baseline playtest, v0.2.43 (`97766e4`) before the identity migration, used
3840 × 2160 on Empire Climb / 2048 at 120 FPS with VSync on and HDR/Pro off.
It does not validate ultrawide rendering or the migrated build. Report cropping,
stretching or missing elements with the resolution, track, GPU, driver and log.

Custom resolutions can be supplied by a mod XML; see [Mods](MODS.md). They must pass the
same title/version and signature checks as other patches. The inherited God of War III
Ultrawide mod and its generator were removed; their camera addresses and gameplay
measurements do not apply to WipEout.
