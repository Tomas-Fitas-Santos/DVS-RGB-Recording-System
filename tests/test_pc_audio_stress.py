import unittest
from array import array

import numpy as np

from tools.pc_audio_stress import window_metrics


class TimingAnalysisTests(unittest.TestCase):
    def test_periodic_response_is_covered_and_coherent(self):
        events = array("q", [int(1000000 + cycle * (1e6 / 600) + 100)
                             for cycle in range(600 * 3)])
        metrics = window_metrics(events, 600, 2, 1, 1000000)
        self.assertEqual(len(metrics), 2)
        self.assertTrue(all(w["cycle_coverage"] > .99 for w in metrics))
        self.assertTrue(all(w["coherence_f"] > .99 for w in metrics))

    def test_gap_reduces_cycle_coverage(self):
        events = array("q", [int(1000000 + cycle * (1e6 / 600) + 100)
                             for cycle in range(600) if not 200 <= cycle < 260])
        metrics = window_metrics(events, 600, 1, 1, 1000000)
        self.assertLess(metrics[0]["cycle_coverage"], .91)

    def test_random_events_are_not_coherent(self):
        rng = np.random.default_rng(3)
        events = array("q", sorted(int(t) for t in rng.uniform(1e6, 2e6, 1800)))
        metrics = window_metrics(events, 600, 1, 1, 1000000)
        self.assertLess(max(metrics[0]["coherence_f"], metrics[0]["coherence_2f"]), .15)


if __name__ == "__main__":
    unittest.main()
