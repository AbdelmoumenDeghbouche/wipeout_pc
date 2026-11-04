import importlib.util
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

spec = importlib.util.spec_from_file_location('bench_summary', ROOT / 'tools/deck/bench-summary.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

TSC_HZ = 2_000_000_000  # 2 GHz synthetic TSC


def frame_log(frames, start_ms=36_000_000, a_us_per_draw=4.5, b_us_per_draw=4.0, frame_ms=16.667):
    """frames: list of per-frame draw counts. Cumulative counters as the GPU library writes them."""
    lines = ['# ms_of_day steady_us draws a_cpu_us b_cycles tsc']
    draws = a = b = 0
    for i, n in enumerate(frames):
        draws += n
        a += n * a_us_per_draw
        b += n * b_us_per_draw * TSC_HZ / 1e6
        t_us = int(i * frame_ms * 1000)
        ms_of_day = int(start_ms + i * frame_ms) % 86_400_000  # the GPU library's clock wraps at midnight
        lines.append(f'{ms_of_day} {t_us} {draws} {int(a)} {int(b)} {t_us * TSC_HZ // 1_000_000}')
    return '\n'.join(lines) + '\n'


def make_run(root, name, frames, replay_at='10:00:00.000', status='ok', **kw):
    d = Path(root) / name
    d.mkdir()
    (d / 'frames.log').write_text(frame_log(frames, **kw))
    (d / 'game.log').write_text(f'{replay_at} Runtime: pad replay of 20 samples from x.pad\n')
    (d / 'status').write_text(status + '\n')
    return d


MARKS = {'countdown_ms': 10_000, 'photo_ms': 20_000}
# 10:00:00.000 = 36 000 000 ms of day. Frames every 16.667 ms from there.
GOOD = [100] * 600 + [4200] * 300 + [2500] * 300 + [3434] * 1500  # countdown at 10-13 s, photo from 20 s


class SpanMetrics(unittest.TestCase):
    def test_good_run_metrics(self):
        with tempfile.TemporaryDirectory() as root:
            run = bench.analyse_run(make_run(root, '01-A', GOOD), MARKS)
            self.assertIsNone(run['invalid'])
            photo = run['photo']
            self.assertAlmostEqual(photo['draws'], 3434, delta=1)
            self.assertAlmostEqual(photo['a_us_per_draw'], 4.5, delta=0.01)
            self.assertAlmostEqual(photo['b_us_per_draw'], 4.0, delta=0.01)
            self.assertAlmostEqual(photo['fps'], 60.0, delta=0.1)
            self.assertAlmostEqual(run['countdown']['draws'], 4200, delta=1)

    def test_run_that_missed_the_grid_is_invalid(self):
        with tempfile.TemporaryDirectory() as root:
            stuck = [126] * 3000  # stayed in a menu: no countdown, no photo mode
            run = bench.analyse_run(make_run(root, '01-A', stuck), MARKS)
            self.assertIn('countdown', run['invalid'])

    def test_photo_span_must_be_constant(self):
        with tempfile.TemporaryDirectory() as root:
            racing = GOOD[:1200] + [3000 + (i % 7) * 100 for i in range(1500)]
            run = bench.analyse_run(make_run(root, '01-A', racing), MARKS)
            self.assertIn('photo', run['invalid'])

    def test_photo_alternating_by_two_draws_is_steady(self):
        with tempfile.TemporaryDirectory() as root:
            frames = GOOD[:1200] + [3434 + 2 * (i % 2) for i in range(1500)]
            run = bench.analyse_run(make_run(root, '01-A', frames), MARKS)
            self.assertIsNone(run['invalid'])

    def test_log_cut_short_in_photo_span_is_invalid(self):
        with tempfile.TemporaryDirectory() as root:
            run = bench.analyse_run(make_run(root, '01-A', GOOD[:1320]), MARKS)  # ends 2 s into the photo span
            self.assertIn('photo mode cut short', run['invalid'])

    def test_missing_game_log_is_invalid(self):
        with tempfile.TemporaryDirectory() as root:
            d = make_run(root, '01-A', GOOD)
            (d / 'game.log').unlink()
            self.assertIn('no replay start', bench.analyse_run(d, MARKS)['invalid'])

    def test_garbled_replay_stamp_is_invalid(self):
        with tempfile.TemporaryDirectory() as root:
            d = make_run(root, '01-A', GOOD)
            (d / 'game.log').write_text('xx:yy Runtime: pad replay of 20 samples from x.pad\n')
            self.assertIn('no replay start', bench.analyse_run(d, MARKS)['invalid'])

    def test_single_frame_log_is_too_short(self):
        with tempfile.TemporaryDirectory() as root:
            run = bench.analyse_run(make_run(root, '01-A', [100]), MARKS)
            self.assertIn('too short', run['invalid'])

    def test_partial_last_line_is_ignored(self):
        with tempfile.TemporaryDirectory() as root:
            d = make_run(root, '01-A', GOOD)
            with open(d / 'frames.log', 'a') as f:
                f.write('36050000 1234')  # killed mid-write
            run = bench.analyse_run(d, MARKS)
            self.assertIsNone(run['invalid'])

    def test_a_without_frame_stats_is_unavailable(self):
        with tempfile.TemporaryDirectory() as root:
            run = bench.analyse_run(make_run(root, '01-A', GOOD, a_us_per_draw=0), MARKS)
            self.assertIsNone(run['photo']['a_us_per_draw'])

    def test_midnight_rollover_is_invalid(self):
        with tempfile.TemporaryDirectory() as root:
            near_midnight = 86_400_000 - 5_000
            run = bench.analyse_run(make_run(root, '01-A', GOOD, start_ms=near_midnight, replay_at='23:59:55.000'), MARKS)
            self.assertIn('midnight', run['invalid'])

    def test_timeout_status_is_invalid(self):
        with tempfile.TemporaryDirectory() as root:
            run = bench.analyse_run(make_run(root, '01-A', GOOD, status='timeout'), MARKS)
            self.assertIn('timeout', run['invalid'])


class Summary(unittest.TestCase):
    def test_medians_per_build_and_difference(self):
        with tempfile.TemporaryDirectory() as root:
            for i, b in enumerate([4.0, 4.1, 3.9]):
                make_run(root, f'{2 * i + 1:02d}-A', GOOD, b_us_per_draw=b)
                make_run(root, f'{2 * i + 2:02d}-B', GOOD, b_us_per_draw=b * 0.9)
            (Path(root) / 'vinetak-photo.pad.marks').write_text('countdown_ms 10000\nphoto_ms 20000\n')
            result = bench.summarise(Path(root))
            self.assertAlmostEqual(result['A']['photo']['b_us_per_draw']['median'], 4.0, delta=0.01)
            self.assertAlmostEqual(result['B']['photo']['b_us_per_draw']['median'], 3.6, delta=0.01)
            self.assertEqual(result['A']['valid'], 3)
            self.assertEqual(result['B']['valid'], 3)


if __name__ == '__main__':
    unittest.main()
