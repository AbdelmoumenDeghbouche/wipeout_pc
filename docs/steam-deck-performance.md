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

### The CPU bench

Hands-off, same route every run. Record the pad route once: on the Deck run
`route-record.sh start <build dir>`, play the route, then `route-record.sh stop`, which writes
`~/diag/route-shifted.pad`. Copy it to `tools/deck/routes/<name>.pad` and write
`<name>.pad.marks` by hand: `countdown_ms` is the X press at START RACE, `photo_ms` the press that
opens photo mode, both read from the shifted route. A press the bench should not replay
(accelerating at "go") is removed from the route, as for `vinetak-photo`. The repo copy lives in
`tools/deck/routes/`; `bench-run.sh` reads `~/diag/routes/vinetak-photo.pad` on the Deck by default.
`tools/deck/bench-run.sh <N> <A> <B>` then launches N alternating A/B pairs (A and B are extracted
AppImage dirs), replays the route from gamepad connect, holds photo mode 30 s and takes a
screenshot. Run one warm-up pair first so the shader cache is filled. The Deck needs
`tools/deck/{bench-run.sh,bench-summary.py,route-record.sh,stamp.py}` in `~/diag/` and the routes in
`~/diag/routes/` (copied with scp).
`python3 -I ~/diag/bench-summary.py <dir>` reads each run's `GOW3_FRAME_LOG` (`frames.log`),
drops broken or short runs, and prints median and range per side for two spans:

- countdown: `countdown_ms` +0.5-3 s, about 4150 draws per frame;
- photo mode: `photo_ms` +3-23 s, about 3217 draws per frame (a static grid scene, the steadier span).

Metrics: FPS, draws per frame, stage A and stage B CPU time per draw (µs).

A/A noise floor, same build on both sides, 4 + 4 runs (2026-11-04, upstream 0.2.59 plus the frame log):

| span | metric | A median (range) | B median (range) | spread (max − min over all 8 runs) |
|---|---|---|---|---|
| photo | stage A µs/draw | 4.76 (4.71-4.77) | 4.76 (4.74-4.78) | 0.07 (1.5 %) |
| photo | stage B µs/draw | 4.34 (4.33-4.36) | 4.36 (4.32-4.36) | 0.04 (0.9 %) |
| photo | FPS | 59.73 (59.45-59.79) | 59.77 (59.51-59.97) | 0.52 (0.9 %) |
| countdown | stage A µs/draw | 4.75 (4.69-4.83) | 4.81 (4.71-4.84) | 0.15 (3.2 %) |
| countdown | stage B µs/draw | 4.50 (4.42-4.57) | 4.54 (4.53-4.60) | 0.18 (4.0 %) |
| countdown | FPS | 49.49 (48.71-50.27) | 48.88 (48.76-49.18) | 1.56 (3.2 %) |

The spread is the smallest difference a 4 + 4 bench can show: a change smaller than it is noise.
Medians of the two sides differed by at most 1.3 % (A/A). Photo mode is the quieter span for
µs/draw; FPS there is 59.5-60, near the 60 FPS cap, so it probably cannot show a gain; the per-draw
costs and the countdown FPS are the metrics.

`perf` works on the Deck as the deck user (user-space samples):
`perf record -e cpu-clock:u -F 999 -g -p $(pgrep -x gow3-probe) -- sleep 20`.
