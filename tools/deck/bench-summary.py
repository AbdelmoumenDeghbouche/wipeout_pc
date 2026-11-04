#!/usr/bin/env python3
"""Summarise a tools/deck/bench-run.sh directory: per build (A, B), median and range over the
valid runs of FPS, draws per frame and stage A / stage B microseconds per draw, over the race
countdown and the photo-mode span the route's marks file names.
usage: python3 -I bench-summary.py <bench directory>"""
import statistics
import sys
from pathlib import Path

COUNTDOWN = (500, 3000)       # ms after countdown_ms
PHOTO = (3000, 23000)         # ms after photo_ms
COUNTDOWN_MIN_DRAWS = 3500    # the grid is 4100-4250 draws a frame
PHOTO_MIN_DRAWS = 3000        # photo mode on the grid is about 3217 with the menu open
PHOTO_CONSTANT_SHARE = 0.9
PHOTO_TOLERANCE = 0.01        # a steady frame is within 1 % of the span's median draws
MIN_SPAN_FRAMES = 30
METRICS = ('fps', 'draws', 'a_us_per_draw', 'b_us_per_draw')


def read_frames(path):
    rows = []
    # the last element is '' after a final newline, else a line cut by the killing signal
    for line in Path(path).read_text().split('\n')[:-1]:
        if line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) != 6 or not all(p.isdigit() for p in parts):
            continue  # a line cut by the signal that ended the game
        rows.append(tuple(int(p) for p in parts))
    return rows


def ms_of_day(stamp):
    h, m, rest = stamp.split(':')
    s, ms = rest.split('.')
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)


def replay_start(game_log):
    try:
        lines = Path(game_log).read_text(errors='replace').splitlines()
    except OSError:
        return None
    for line in lines:
        if 'Runtime: pad replay of' in line:
            try:
                return ms_of_day(line.split()[0])
            except (ValueError, IndexError):
                continue
    return None


def read_marks(path):
    marks = {}
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        try:
            marks[parts[0]] = int(parts[1])
        except ValueError:
            continue
    for key in ('countdown_ms', 'photo_ms'):
        if key not in marks:
            raise SystemExit(f'{path}: marks file lacks {key}')
    return marks


def span(rows, t0, t1, tsc_hz):
    inside = [r for r in rows if t0 <= r[0] <= t1]
    if len(inside) < MIN_SPAN_FRAMES or rows[-1][0] < t1:
        return None
    first, last = inside[0], inside[-1]
    draws = last[2] - first[2]
    seconds = (last[1] - first[1]) / 1e6
    frames = len(inside) - 1
    per_frame = [b[2] - a[2] for a, b in zip(inside, inside[1:])]
    a_us = last[3] - first[3]
    return {
        'fps': frames / seconds if seconds > 0 else 0.0,
        'draws': draws / frames,
        'a_us_per_draw': a_us / draws if draws and a_us else None,
        'b_us_per_draw': (last[4] - first[4]) / tsc_hz / draws * 1e6 if draws and last[4] != first[4] else None,
        'per_frame_draws': per_frame,
    }


def analyse_run(run_dir, marks):
    run_dir = Path(run_dir)
    result = {'name': run_dir.name, 'invalid': None, 'countdown': None, 'photo': None}
    status_file = run_dir / 'status'
    status = status_file.read_text().strip() if status_file.exists() else 'ok'
    if status != 'ok':
        result['invalid'] = f'run status {status}'
        return result
    frames_log = run_dir / 'frames.log'
    if not frames_log.exists():
        result['invalid'] = 'no frames.log'
        return result
    rows = read_frames(frames_log)
    if any(b[0] < a[0] for a, b in zip(rows, rows[1:])):
        result['invalid'] = 'the frame log crosses midnight'
        return result
    if len(rows) < 2 or rows[-1][1] <= rows[0][1]:
        result['invalid'] = 'frame log too short'
        return result
    start = replay_start(run_dir / 'game.log')
    if start is None:
        result['invalid'] = 'no replay start in game.log'
        return result
    tsc_hz = (rows[-1][5] - rows[0][5]) / ((rows[-1][1] - rows[0][1]) / 1e6)
    if tsc_hz <= 0:
        result['invalid'] = 'frame log too short'
        return result
    c0 = start + marks['countdown_ms']
    countdown = span(rows, c0 + COUNTDOWN[0], c0 + COUNTDOWN[1], tsc_hz)
    if not countdown:
        result['invalid'] = 'countdown cut short'
        return result
    if statistics.median(countdown['per_frame_draws']) < COUNTDOWN_MIN_DRAWS:
        result['invalid'] = 'countdown not reached (no grid-sized frames at its mark)'
        return result
    p0 = start + marks['photo_ms']
    photo = span(rows, p0 + PHOTO[0], p0 + PHOTO[1], tsc_hz)
    if not photo:
        result['invalid'] = 'photo mode cut short'
        return result
    per_frame = photo['per_frame_draws']
    centre = statistics.median(per_frame)
    share = sum(1 for n in per_frame if abs(n - centre) <= centre * PHOTO_TOLERANCE) / len(per_frame)
    if share < PHOTO_CONSTANT_SHARE:
        result['invalid'] = f'photo mode not steady ({share:.0%} of frames within 1 % of {centre:.0f} draws)'
        return result
    if centre < PHOTO_MIN_DRAWS:
        result['invalid'] = f'photo mode too light (steady at {centre:.0f} draws, below {PHOTO_MIN_DRAWS})'
        return result
    result['countdown'], result['photo'] = countdown, photo
    return result


def summarise(bench_dir):
    bench_dir = Path(bench_dir)
    marks_file = next(iter(sorted(bench_dir.glob('*.marks'))), None)
    if marks_file is None:
        raise SystemExit(f'{bench_dir}: no *.marks file')
    marks = read_marks(marks_file)
    out = {}
    for build in ('A', 'B'):
        runs = [analyse_run(d, marks) for d in sorted(bench_dir.glob(f'*-{build}')) if d.is_dir()]
        valid = [r for r in runs if not r['invalid']]
        entry = {'valid': len(valid), 'invalid': [(r['name'], r['invalid']) for r in runs if r['invalid']]}
        for part in ('countdown', 'photo'):
            entry[part] = {}
            for metric in METRICS:
                values = [r[part][metric] for r in valid if r[part][metric] is not None]
                entry[part][metric] = ({'median': statistics.median(values), 'min': min(values),
                                        'max': max(values)} if values else None)
        out[build] = entry
    return out


def main():
    result = summarise(sys.argv[1])
    for build in ('A', 'B'):
        print(f"{build}: {result[build]['valid']} valid runs")
        for name, why in result[build]['invalid']:
            print(f'  {name}: invalid, {why}')
    for part in ('countdown', 'photo'):
        print(f'\n{part}:')
        for metric in METRICS:
            a, b = result['A'][part][metric], result['B'][part][metric]
            fmt = lambda s: 'n/a' if not s else f"{s['median']:.2f} ({s['min']:.2f}-{s['max']:.2f})"
            diff = f"{(b['median'] - a['median']) / a['median'] * 100:+.1f} %" if a and b and a['median'] else ''
            print(f'  {metric:14s} A {fmt(a):24s} B {fmt(b):24s} {diff}')


if __name__ == '__main__':
    main()
