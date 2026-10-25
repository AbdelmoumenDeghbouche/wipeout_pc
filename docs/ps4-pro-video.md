# PS4 Pro video output and HDR

Pro mode needs the Pro memory budget, shader translation and surface-layout support. `GOW3_NEO=1` selects the console model; it does not implement missing GPU instructions by itself.

From a native Windows checkout, start a debug session with a virtual 4K display:

```powershell
$env:GOW3_NEO = '1'
$env:GOW3_INTERNAL_WIDTH = '3840'
$env:GOW3_INTERNAL_HEIGHT = '2160'
$env:GOW3_HDR = '1'
python scripts/windows_debug.py run --game 'D:\Games\CUSA05670'
```

Use the extracted game directory containing `eboot.bin`. Contributors editing in WSL can run the same commands in PowerShell from their Windows mirror; see [the WSL workflow](windows-debug-wsl.md).

There are three separate sizes:

- The game's scene render targets, selected by the game and applicable resolution patches.
- The virtual display reported by VideoOut, selected by `GOW3_INTERNAL_WIDTH` and `GOW3_INTERNAL_HEIGHT`; the defaults are 1920x1080. Despite their names, these variables describe the display reported to the game.
- The Windows window or desktop image, which follows the window size and fullscreen mode.

For example, Pro can render a 4K scene, reduce it to a 1080p VideoOut buffer, then enlarge it to a 1440p window. Reporting a 4K display lets the game register a 4K output buffer instead; the presenter then adapts that image directly to the window. This does not turn a 1440p monitor into a 4K monitor. Check the actual buffers rather than assuming the requested size was selected.

`GOW3_HDR=1` permits HDR when Vulkan offers the required BT.2020/PQ format and HDR is enabled for the window's display. Enable HDR in Windows display settings before starting the game. If HDR is disabled or unavailable, the runtime reports SDR capability and the game can select SDR output. Set `GOW3_HDR=0` to disable HDR explicitly. HDR and resolution are independent.

HDR availability is sampled when the window is created. Restart after changing the Windows HDR setting or moving the game to another display. Live switching is not supported yet.

Validation used CUSA05670 01.07 on native Windows with an RTX 3080: the game registered 3840x2160 buffers in both SDR and BT.2020/PQ HDR; the 2560x1440 monitor received a directly adapted image. With HDR permitted in the runtime, separate launches followed the Windows HDR state correctly. The 120 FPS patch was unavailable for that executable; the observed presentation rate was approximately 60 FPS. See Sony's [4K/60 FPS announcement](https://blog.playstation.com/2017/03/30/wipeout-omega-collection-coming-to-ps4-on-june-6th/) for the original Pro target.

These PowerShell variables apply to that process and its children. They do not change the launcher's saved profile. HDR screenshots are converted to SDR and cannot establish the monitor's HDR appearance by themselves.
