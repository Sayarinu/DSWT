from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import load


def report(per_second=10.0, p95=420.0, conservation="pass", window=3600, trading=1000, submit_p95=480.0):
    return {"measured": {"window_seconds": window, "active_clients": {"configured": 1000, "trading": trading},
                         "completed_pairs": {"per_second": per_second},
                         "latency": {"all": {"p95_ms": p95}, "2": {"p95_ms": submit_p95}}},
            "conservation": {"status": conservation}}


class MeasurementTests(unittest.TestCase):
    def test_percentiles_use_the_nearest_rank(self):
        values = list(range(1, 101))
        self.assertEqual(load.percentile(values, 0.95), 95)
        self.assertEqual(load.percentile(values, 0.5), 50)
        self.assertEqual(load.percentile([7], 0.99), 7)
        self.assertIsNone(load.percentile([], 0.95))
        self.assertEqual(load.summarize([1000, 2000, 500000])["max_ms"], 500.0)

    def test_rates_count_only_the_window_and_report_the_slowest_minute(self):
        times = [100 + index * 0.1 for index in range(1200)] + [50, 400]
        measured = load.rates(times, 100, 220)
        self.assertEqual(measured["count"], 1200)
        self.assertEqual(measured["per_second"], 10.0)
        self.assertEqual(measured["slowest_minute_per_second"], 10.0)
        gap = load.rates([100 + index * 0.1 for index in range(600)], 100, 220)
        self.assertEqual(gap["slowest_minute_per_second"], 0.0)

    def test_conservation_compares_exact_payload_multisets(self):
        self.assertTrue(load.conserved(["a", "b", "b"], ["b", "a", "b"]))
        self.assertFalse(load.conserved(["a", "b"], ["b", "b"]))
        self.assertFalse(load.conserved(["a", "b"], ["a"]))

    def test_each_client_gets_a_distinct_loopback_network(self):
        addresses = [load.source(index) for index in range(62500)]
        self.assertEqual(len(set(addresses)), 62500)
        self.assertTrue(all(address.startswith("127.") for address in addresses))
        self.assertTrue(all(0 < int(address.rsplit(".", 1)[1]) < 255 for address in addresses))

    def test_every_fixed_target_must_be_met_without_weakening(self):
        self.assertEqual(load.verdict(report())["status"], "pass")
        for changed, check in ((report(per_second=9.99), "completed_pairs_per_second"),
                               (report(p95=500.0), "api_p95_below_500ms"),
                               (report(p95=None), "api_p95_below_500ms"),
                               (report(submit_p95=510.0), "api_p95_below_500ms"),
                               (report(conservation="fail"), "conservation"),
                               (report(window=300), "duration"),
                               (report(trading=999), "active_clients")):
            with self.subTest(check=check):
                result = load.verdict(changed)
                self.assertEqual(result["status"], "fail")
                self.assertFalse(result["checks"][check])


if __name__ == "__main__":
    unittest.main()
