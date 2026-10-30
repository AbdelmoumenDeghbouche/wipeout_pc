WipEout Omega Collection for PC (wipeout_pc)
============================================

An early version of a native PC port. Menus and races (HD and 2048) play; not every
event type has been tested yet.

No game files are included. You need your own copy of WipEout Omega Collection, dumped from
your own PS4, as a folder containing eboot.bin, sce_module and sce_sys. The local baseline
playtest (v0.2.43, 97766e4, before the identity migration) used CUSA05670 (EU), APP_VER
01.07, merged into one folder: Empire Climb / 2048,
4K, 120 FPS, VSync on, HDR and Pro off. CUSA07671 (US) has a supported profile; that
local playtest does not validate it or every track, mode or graphics setting. The migrated
build has not yet had a manual gameplay test.

Requirements
- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX: Intel Sandy Bridge (2011) or newer, AMD FX or newer.
  Older processors stop at start with a "STOP: this CPU lacks" line in the log.
- A graphics card with Vulkan 1.3 and an up-to-date driver.
  Nothing else: Python and the libraries are inside this folder.

Starting
1. Extract this folder anywhere, for example C:\Games\wipeout-windows (not inside the zip).
2. Open "WipEout.exe". If Windows shows "Windows protected your PC", click More info, then
   Run anyway: the program is not digitally signed.
3. Click "Choose the game folder", then Browse... next to Game folder, and select the folder
   with eboot.bin.
4. Back on Home, adjust the quick settings if you like, then click PLAY. The first start takes
   a little longer while the game image is prepared.

Settings
- Frame rate: the game runs on real time, so it can go above 60 FPS (up to 240). The Frame
  rate limit on Home sets the cap.
- Render resolution: 1920x1080 by default. 2560x1440, 3840x2160 (4K), 2560x1080, 3440x1440
  and 5120x2160 are in the port's in-game menu, under Performance, and apply on the next start.
  The HUD and menus stay 16:9 in the middle of wider screens. 4K and 5120x2160 need a lot of
  video memory.
- Your own music: the launcher's Music page shows the music folder (user\music, or one you
  choose) and the songs in it: MP3, OGG, FLAC, WAV or M4A. They play instead of the game's
  music. In the game, Insert (or R3 + L2) > Music picks a song, skips, switches shuffle and
  your music off, and sets the music volume.
- "Native 4K rendering (no checkerboard)" on the Game page is on by default: from 3840 pixels
  wide the game renders at full resolution instead of checkerboard.

While playing
- Insert on the keyboard, or R3 + L2 on the controller, opens the port's menu: image,
  performance and the performance overlay.
- Saved pipelines load in the background; a small note shows the progress. New areas can
  stutter the first time while their shaders compile.
- Keyboard keys: Game -> Controls in the launcher.

Next time
- "Play WipEout.exe" starts the game straight away with the saved settings. Make a desktop
  shortcut to it (Advanced -> Desktop shortcut), or add it to Steam with "Add a Non-Steam Game".
- When a new version is out, the launcher offers to open its release page in your browser.
  It does not download, install or replace files. Extract the new package separately and
  preserve your data. Advanced -> Check for updates checks by hand.

Data
- Saves, shader cache and the log: user\ next to WipEout.exe (keep it when updating).
- Settings: wipeout_oc.ini next to WipEout.exe (or in WIPEOUT_OC_DATA_DIR). An existing
  gow3.ini is copied byte for byte on first use and kept intact. If both exist, the new file
  wins; no merge or overwrite. Migration/read errors stop startup with an error message.
  WIPEOUT_OC_CONFIG selects an exact file (relative to the caller's working directory);
  a missing override starts with defaults and Save creates that file. GOW3_CONFIG remains
  accepted when the new variable is absent; an empty WIPEOUT_OC_CONFIG selects the default.
  Launcher options remain in %APPDATA%\wipeout-launcher. Both INIs survive package updates.
- Generated files (prepared game image, patches): out\.

Patches
- WipEout event unlocks remain available. Inherited God of War III cheats, mouse aiming,
  WAD prompt/pause-menu injection and LightingTest have been removed.
- Third-party XML must match the actual title ID and APP_VER. Address-only writes require
  the validated EU 01.07 executable; other builds need unique safe signatures. Skipped
  patches are reported in the log. Built-in XML uses metadata tag 01.00, not the dump version.

Known issues
- PS4 Pro mode is experimental: Graphics -> PS4 Pro mode in the launcher.
- Some event types and tracks have not been tested.

Problems
- The game does not open: check Advanced -> Log, and send user\last_run.log with your report.
  With a custom Saves folder, last_run.log is in that folder instead.
- Memory error (error 1450): increase the Windows page file. Win+R ->
  SystemPropertiesAdvanced -> Performance Settings -> Advanced -> Virtual memory Change ->
  set C: to 16384 MB initial, 24576 MB maximum -> Set -> OK, then restart your PC.
- RTSS/Afterburner and ReShade must target bin\wipeout_oc-probe.exe, not the launcher. Select
  Vulkan in the ReShade installer.
- Report problems here, with your GPU, driver, CPU and the log:
  https://github.com/AbdelmoumenDeghbouche/wipeout_pc/issues

This project is based on the God of War III PC port.

This project is not affiliated with Sony Interactive Entertainment, XDev, Clever Beans or EPOS
Game Studios. WipEout is a trademark of Sony Interactive Entertainment.
