"""Startup order of the guest memory pool and the GPU (#149).

On Windows the guest pool is one SEC_COMMIT section charged in full when it is created. The
blocking shader warm-up runs inside gow3gpu_init and can hold thousands of pipelines in driver
memory, so a pool created after it may no longer fit in the commit limit ("cannot create ...
MiB of guest memory (error 1450)" after a finished warm-up). The pool is therefore charged
before the GPU starts.
"""
from paths import ROOT
import re
import unittest

PROBE = ROOT / 'src/probe.c'


class StartupMemoryOrderTests(unittest.TestCase):
    def setUp(self):
        self.source = PROBE.read_text(encoding='utf-8')

    def test_guest_pool_is_committed_before_the_gpu_starts(self):
        commit = self.source.find('runtime_memory_commit()')
        gpu = self.source.find('gow3gpu_init(&gpu)')
        self.assertNotEqual(gpu, -1, 'gow3gpu_init call not found in probe.c')
        self.assertNotEqual(commit, -1, 'probe.c never commits the guest pool up front')
        self.assertLess(commit, gpu, 'the guest pool must be committed before gow3gpu_init')

    def test_a_failed_commit_stops_before_the_gpu_starts(self):
        # The pool's own message names the error; the probe must not go on to a warm-up
        # that cannot be followed by the game.
        self.assertRegex(self.source, re.compile(r'if \(runtime_memory_commit\(\)\)\s*fail\('))


if __name__ == '__main__':
    unittest.main()
