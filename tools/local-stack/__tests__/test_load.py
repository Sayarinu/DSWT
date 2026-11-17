from pathlib import Path
import sys
import unittest
from unittest import mock

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

    def test_loopback_address_boundaries(self):
        self.assertEqual(load.source(0), "127.10.0.1")
        self.assertEqual(load.source(249), "127.10.0.250")
        self.assertEqual(load.source(250), "127.10.1.1")
        self.assertEqual(load.source(62499), "127.10.249.250")

    def test_idle_client_is_not_settled_until_pending_packet_is_retired(self):
        self.assertFalse(load.settled(None))
        self.assertFalse(load.settled({}))
        self.assertFalse(load.settled({"phase": "queued", "pending_sha256": ""}))
        self.assertFalse(load.settled({"phase": "idle", "pending_sha256": "packet"}))
        self.assertTrue(load.settled({"phase": "idle", "pending_sha256": ""}))

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


class ProductionMeasurementTests(unittest.TestCase):
    def test_fake_clients_never_share_a_network_with_real_ones(self):
        real = {load.source(index) for index in range(62500)}
        fake = {load.flood_source(index) for index in range(62500)}
        self.assertEqual(len(fake), 62500)
        self.assertFalse(real & fake)
        self.assertEqual(load.flood_source(0), "127.20.0.1")

    def test_vps_cores_parse_lists_and_ranges(self):
        self.assertEqual(load.cpu_list("2,3"), [2, 3])
        self.assertEqual(load.cpu_list("4-5"), [4, 5])
        self.assertEqual(load.cpu_list("0,6-7"), [0, 6, 7])

    def test_resource_peaks_parse_docker_stats(self):
        samples = [{"containers": [{"Name": "a", "CPUPerc": "12.5%", "MemUsage": "100MiB / 1GiB", "PIDs": "7"}]},
                   {"containers": [{"Name": "a", "CPUPerc": "80.25%", "MemUsage": "1.5GiB / 2GiB", "PIDs": "5"}]}]
        self.assertEqual(load.peaks(samples), {"a": {"cpu_percent": 80.25, "memory_bytes": int(1.5 * 2 ** 30), "pids": 7}})

    def test_storage_growth_scales_to_a_day(self):
        first = {"wal_bytes": 0, "archive_bytes": None, "database_bytes": 100}
        last = {"wal_bytes": 3_600_000_000, "archive_bytes": 5, "database_bytes": 100}
        measured = load.growth(first, last, 3600)
        self.assertEqual(measured["wal_bytes"], {"added": 3_600_000_000, "per_hour": 3_600_000_000, "gb_per_day": 86.4})
        self.assertNotIn("archive_bytes", measured)
        self.assertEqual(measured["database_bytes"]["added"], 0)
        self.assertIsNone(load.growth(None, last, 3600))

    def test_error_rate_counts_transport_failures_and_timeouts(self):
        rate = load.error_rate({"1": [1, 2, 3], "2": [4]}, {"transport:send": 1, "probe_timeout": 1, "retryable_run": 9})
        self.assertEqual(rate, {"attempts": 4, "failed": 2, "rate": 0.5})
        self.assertIsNone(load.error_rate({}, {})["rate"])

    def test_fake_traffic_must_run_and_stay_within_the_wal_budget(self):
        passing = dict(report(), fake_traffic={"enrollments": 10, "submissions": 0,
                                               "calibration": {"production_cap_bytes_per_day": 10 ** 9}})
        self.assertEqual(load.verdict(passing)["status"], "pass")
        over = dict(report(), fake_traffic={"enrollments": 10, "submissions": 0,
                                            "calibration": {"production_cap_bytes_per_day": 6 * 10 ** 9}})
        self.assertFalse(load.verdict(over)["checks"]["fake_enrollment_wal_within_budget"])
        idle = dict(report(), fake_traffic={"enrollments": 0, "submissions": 0, "calibration": None})
        self.assertFalse(load.verdict(idle)["checks"]["fake_traffic_ran"])


class ProbeAccountingTests(unittest.IsolatedAsyncioTestCase):
    async def run_probe(self, driver, client):
        output = b'{"wire_operation": 2, "wire_result": 0, "elapsed_us": 1000}\n{"summary": true}\n'
        process = mock.Mock(communicate=mock.AsyncMock(return_value=(output, b"")), returncode=0)
        with mock.patch.object(load.asyncio, "create_subprocess_exec", new=mock.AsyncMock(return_value=process)):
            return await driver.probe(client, "submit")

    async def test_fake_and_overload_samples_stay_out_of_the_targets(self):
        driver = load.Driver({"concurrency": 1, "offer_rate": 1, "overload_rate": 5, "probe": "probe", "pins": ["c", "n"]})
        driver.measuring = True
        await self.run_probe(driver, {"path": "fake", "legal": False, "source": "127.20.0.1"})
        self.assertEqual(driver.latency, {})
        self.assertEqual(driver.flood_latency, {"2": [1000]})
        self.assertEqual(driver.active, set())
        driver.overloaded = True
        await self.run_probe(driver, {"path": "real", "legal": True, "source": "127.10.0.1"})
        self.assertEqual(driver.latency, {})
        self.assertEqual(driver.overload_latency, {"2": [1000]})
        driver.overloaded = False
        await self.run_probe(driver, {"path": "real", "legal": True, "source": "127.10.0.1"})
        self.assertEqual(driver.latency, {"2": [1000]})
        self.assertEqual(driver.results["2:0"], 2)
        self.assertEqual(driver.tokens.maxsize, 10)


class ProbeTimeoutTests(unittest.IsolatedAsyncioTestCase):
    async def test_deadline_and_latency_accounting_use_the_same_budget(self):
        process = mock.Mock(communicate=mock.AsyncMock(return_value=(b"", b"")),
                            wait=mock.AsyncMock())

        async def timeout(awaitable, seconds):
            await awaitable
            self.assertEqual(seconds, 0.5)
            raise TimeoutError

        driver = load.Driver({"concurrency": 1, "offer_rate": 1,
                              "probe": "probe", "pins": ["current", "next"]})
        driver.measuring = True
        with mock.patch.object(load, "PROBE_TIMEOUT", 0.5), \
             mock.patch.object(load.asyncio, "create_subprocess_exec", new=mock.AsyncMock(return_value=process)), \
             mock.patch.object(load.asyncio, "wait_for", new=timeout):
            self.assertIsNone(await driver.probe({"path": "client", "source": "127.10.0.1"}, "collect"))
        process.kill.assert_called_once_with()
        process.wait.assert_awaited_once_with()
        self.assertEqual(driver.failures["probe_timeout"], 1)
        self.assertEqual(driver.latency["timeout"], [500000])


if __name__ == "__main__":
    unittest.main()
