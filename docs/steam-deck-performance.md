# Steam Deck performance

Where a WipEout race's frame time goes on a Steam Deck (LCD, 1280x800 panel, 4 cores / 8 threads,
CPU and GPU sharing one power budget), and the changes that moved it. Measured 2026-11-02/03.

## Method

Vineta K, full races, `GOW3_FRAME_STATS=1`: a `Frame stats` line every 5 s with FPS, draws per frame
and the GPU command thread's time. Builds and settings are compared window by window at matched
draws (within ±10 %), never as lap averages: the race start, with all eight ships on the grid, is
the heaviest part of a race at 3000-4300 draws a frame, and later in a lap most windows sit at the
60 FPS vsync cap. Two races of the same build differ by 3.5-5.8 FPS at matched draws, so smaller
differences are noise. `tools/deck/exp-run.sh` records a race this way; `tools/deck/perf-start.sh`
records a call-graph profile of both draw pipeline threads at the race start.

## GPU side

At the 1920x1080 output a race kept the GPU 99-100 % busy at 36-53 FPS.

- **Output resolution.** The presenter letterboxes a 16:9 output into the Deck's 16:10 window, so
  1280x720 is all the panel shows. A 1280x720 output took races from 47-53 to 58-60 FPS. The
  launcher now defaults to it on a Deck (and shows the Output resolution row again).
- **Anti-aliasing.** FSR 3.1 costs about 4-6 ms a frame on the Deck; the port's TAA about 1 ms:

  | setting | 1400-2400 draws | race start |
  |---|---|---|
  | TAA, 1080p output | 57-60 FPS | 37-47 |
  | TAA, 1280x720 output | 60 FPS | 54 at 3300 draws |
  | FSR 3.1 Quality, 1080p output | 50-55 FPS | 45 at 3200 draws |

- **The race drawn at the render resolution.** The reduced-size scene the upscaler presets use
  started only at God of War III's G-buffer pass (five or more colour targets). WipEout's scene pass
  has three, so a race was drawn at 1920x1080 and the presets only downscaled it for the upscaler.
  With WipEout's pass recognised, a race frame draws 36 passes at the preset's size; with FSR 3.1
  that is worth 1-2 FPS on the Deck, since the upscale itself is the larger cost there.

## CPU side

With the GPU side settled, the race start is CPU-bound.

- **No draw-preparation workers on 8 hardware threads.** The four speculative workers and their
  scanner took about a core of the shared power budget. Without them the race start ran 50-51 FPS
  instead of 38-40 at 3950-4000 draws,
  later laps 56 instead of 48 at 3600-3700, nothing slower elsewhere, and the GPU clock ran lower.
  A same-session race of main against this branch: 41 against 53 FPS at the heaviest start.
- **What sets the pace now.** The draw pipeline has two threads: the GPU command thread decodes
  the command stream and builds one packet per draw, the draw recording thread (`gow3:DrawRec`)
  binds and records it. At the race start the recording thread is busy 84-95 % of the time and the
  command thread waits for it 14-26 %, so a frame is as long as the recording thread's work. That
  work has no single hot spot: texture binding is about 20 % of it, vertex buffers 12 %, the
  render-pass memo 10 %, then a long tail. Those are the next things to look at.
- **Per-draw packet changes.** The dirty-register mask cleared as it is scanned, the packet's ring
  vectors as a member instead of `thread_local`, 16-word register blocks instead of 32, and no
  register checksum without workers. These take work off the command thread (and some off the
  recording thread) but measured no change at the race start, within the noise above, as expected
  with the recording thread setting the pace. They are a separate commit.

## Measuring

`GOW3_SCREENSHOT_EVERY` encodes its PNGs on the GPU scheduler thread, about 70 ms each: a race with
a screenshot every second has a 70 ms hitch every second. `exp-run.sh` takes one every 5 s.
`GOW3_GPU_PROFILE` reports almost nothing while the GPU is saturated (it skips frames whose results
are not ready), so it is no guide on the Deck.

`perf` works on the Deck as the deck user (user-space samples):
`perf record -e cpu-clock:u -F 999 -g -p $(pgrep -x gow3-probe) -- sleep 20`.
