#include "dswt/server/telemetry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace dswt::server {
namespace {

TEST(TelemetryTest, RecordsCumulativeHistogramAndFixedResultLabels) {
  Telemetry metrics;
  metrics.Record(Operation::Submit, 1, std::chrono::microseconds(5000));
  metrics.Record(Operation::Submit, 8, std::chrono::microseconds(20000));
  metrics.Record(static_cast<Operation>(999), 999, std::chrono::microseconds(-1));
  const auto text = metrics.Render();
  for (const auto *expected : {
      "dswt_requests_total{service=\"backend\",operation=\"submit\",result=\"ok\"} 1\n",
      "dswt_requests_total{service=\"backend\",operation=\"submit\",result=\"illegal_pokemon\"} 1\n",
      "dswt_requests_total{service=\"backend\",operation=\"invalid\",result=\"transport_error\"} 1\n",
      "dswt_request_duration_seconds_bucket{service=\"backend\",operation=\"submit\",le=\"0.005\"} 1\n",
      "dswt_request_duration_seconds_bucket{service=\"backend\",operation=\"submit\",le=\"0.01\"} 1\n",
      "dswt_request_duration_seconds_bucket{service=\"backend\",operation=\"submit\",le=\"0.025\"} 2\n",
      "dswt_request_duration_seconds_bucket{service=\"backend\",operation=\"submit\",le=\"+Inf\"} 2\n",
      "dswt_request_duration_seconds_count{service=\"backend\",operation=\"submit\"} 2\n",
      "dswt_request_duration_seconds_sum{service=\"backend\",operation=\"submit\"} 0.025\n"})
    EXPECT_NE(text.find(expected), std::string::npos) << expected;
}

TEST(TelemetryTest, CountersAreConcurrentAndNeverLabelIdentifiers) {
  Telemetry metrics;
  const auto before = metrics.Render();
  std::vector<std::thread> threads;
  for (unsigned worker = 0; worker < 8; ++worker) threads.emplace_back([&] {
    for (unsigned request = 0; request < 1000; ++request)
      metrics.Record(Operation::Reconcile, 1, std::chrono::microseconds(1000), std::string(16, 'R'), std::string(16, 'M'), std::string(16, 'T'));
  });
  for (auto &thread : threads) thread.join();
  const auto after = metrics.Render();
  EXPECT_NE(after.find("operation=\"reconcile\",result=\"ok\"} 8000\n"), std::string::npos);
  EXPECT_EQ(std::count(before.begin(), before.end(), '\n'), std::count(after.begin(), after.end(), '\n'));
  EXPECT_EQ(after.find("request_id"), std::string::npos);
  EXPECT_EQ(after.find("operation_id"), std::string::npos);
  EXPECT_EQ(after.find("trade_id"), std::string::npos);
  EXPECT_EQ(after.find(std::string(16, 'R')), std::string::npos);
}

TEST(TelemetryTest, LogFieldsAreBoundedHexIdsWithoutPayloadsOrCredentials) {
  const auto line = Telemetry::LogLine(Operation::Collect, 1, 123, std::string(16, 'R'), std::string(16, 'M'), std::string(16, 'T'));
  EXPECT_NE(line.find("\"request_id\":\"52525252525252525252525252525252\""), std::string::npos);
  EXPECT_NE(line.find("\"operation_id\":\"4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d\""), std::string::npos);
  EXPECT_NE(line.find("\"trade_id\":\"54545454545454545454545454545454\""), std::string::npos);
  const std::string credential(32, 'S'), payload(136, 'P');
  const auto rejected = Telemetry::LogLine(Operation::Submit, 3, 0, credential, payload, std::string(16, '\0'));
  EXPECT_EQ(rejected.find("_id"), std::string::npos);
  EXPECT_EQ(rejected.find(credential), std::string::npos);
  EXPECT_EQ(rejected.find(payload), std::string::npos);
  EXPECT_EQ(std::count(rejected.begin(), rejected.end(), '\n'), 1);
}

TEST(TelemetryTest, UnknownAndUnavailableLedgerAgesRemainUnknown) {
  Telemetry metrics;
  LedgerMetrics ledger;
  ledger.up = true; ledger.queued = 2; ledger.unknown_queue_ages = 1;
  ledger.pending = 1; ledger.oldest_delivery_seconds = 15;
  metrics.SetLedger(ledger);
  auto text = metrics.Render();
  EXPECT_NE(text.find("dswt_queued_offers 2\n"), std::string::npos);
  EXPECT_NE(text.find("dswt_oldest_queue_age_seconds NaN\n"), std::string::npos);
  EXPECT_NE(text.find("dswt_oldest_delivery_age_seconds 15\n"), std::string::npos);
  metrics.SetLedger({});
  text = metrics.Render();
  EXPECT_NE(text.find("dswt_database_up 0\n"), std::string::npos);
  EXPECT_NE(text.find("dswt_queued_offers NaN\n"), std::string::npos);
}

TEST(TelemetryTest, GatewayDoesNotPublishBackendLedgerGauges) {
  Telemetry metrics;
  metrics.Initialize(TelemetryRole::Gateway);
  EXPECT_EQ(metrics.Render().find("dswt_database_up"), std::string::npos);
}

} // namespace
} // namespace dswt::server
