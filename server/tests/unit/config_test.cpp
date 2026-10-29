#include "platinumwt/server/config.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace platinumwt::server {
namespace {

class ConfigTest : public testing::Test {
protected:
  void SetUp() override {
    for (std::size_t i = 0; i < names_.size(); ++i) {
      if (const char *value = std::getenv(names_[i])) {
        saved_[i] = value;
      }
    }
    for (const char *name : names_) {
      ASSERT_EQ(unsetenv(name), 0);
    }
  }

  void TearDown() override {
    for (std::size_t i = 0; i < names_.size(); ++i) {
      if (saved_[i]) {
        EXPECT_EQ(setenv(names_[i], saved_[i]->c_str(), 1), 0);
      } else {
        EXPECT_EQ(unsetenv(names_[i]), 0);
      }
    }
  }

  void Set(const char *name, const char *value) {
    ASSERT_EQ(setenv(name, value, 1), 0);
  }

private:
  const std::array<const char *, 12> names_{"WT_HTTP_ADDRESS", "WT_HTTP_PORT", "WT_GRPC_ADDRESS", "WT_GRPC_TARGET",
      "WT_TRADE_TARGET", "WT_TLS_CERTIFICATE", "WT_TLS_PRIVATE_KEY", "WT_BACKEND_ADDRESS", "PLATINUMWT_DATABASE_URL",
      "WT_LEGALITY_TARGET", "WT_LEGALITY_POLICY", "WT_METRICS_PORT"};
  std::array<std::optional<std::string>, 12> saved_;
};

TEST_F(ConfigTest, LoadsDefaults) {
  const auto config = LoadServerConfig();
  EXPECT_EQ(config.http_address, "0.0.0.0");
  EXPECT_EQ(config.http_port, 8080);
  EXPECT_EQ(config.grpc_address, "127.0.0.1:50051");
  EXPECT_FALSE(config.grpc_target.has_value());
  EXPECT_EQ(config.metrics_port, 0);
}

TEST_F(ConfigTest, MetricsAreOptInWithBoundedPorts) {
  Set("WT_METRICS_PORT", "9091");
  EXPECT_EQ(LoadServerConfig().metrics_port, 9091);
  for (const char *port : {"", "-1", "+1", "65536", "12junk"}) {
    Set("WT_METRICS_PORT", port);
    EXPECT_THROW(LoadMetricsPort(), std::invalid_argument);
  }
}

TEST_F(ConfigTest, LoadsEnvironmentOverrides) {
  Set("WT_HTTP_ADDRESS", "127.0.0.1");
  Set("WT_HTTP_PORT", "9090");
  Set("WT_GRPC_ADDRESS", "127.0.0.1:50052");
  Set("WT_GRPC_TARGET", "backend:50051");
  const auto config = LoadServerConfig();
  EXPECT_EQ(config.http_address, "127.0.0.1");
  EXPECT_EQ(config.http_port, 9090);
  EXPECT_EQ(config.grpc_address, "127.0.0.1:50052");
  EXPECT_EQ(config.grpc_target, "backend:50051");
}

TEST_F(ConfigTest, AllowsEphemeralPort) {
  Set("WT_HTTP_PORT", "0");
  EXPECT_EQ(LoadServerConfig().http_port, 0);
}

TEST_F(ConfigTest, AllowsMaximumPort) {
  Set("WT_HTTP_PORT", "65535");
  EXPECT_EQ(LoadServerConfig().http_port, 65535);
}

TEST_F(ConfigTest, RejectsInvalidPorts) {
  for (const char *port : {"", "invalid", "8080junk", "65536", "-1",
                           "999999999999999999999999999999"}) {
    SCOPED_TRACE(port);
    Set("WT_HTTP_PORT", port);
    EXPECT_THROW(LoadServerConfig(), std::logic_error);
  }
}

TEST_F(ConfigTest, PreservesExplicitlyEmptyBackendTarget) {
  Set("WT_GRPC_TARGET", "");
  const auto config = LoadServerConfig();
  ASSERT_TRUE(config.grpc_target.has_value());
  EXPECT_TRUE(config.grpc_target->empty());
}

TEST_F(ConfigTest, RequiresCompleteTlsConfigurationAndPrivateTradeTargets) {
  Set("WT_TLS_CERTIFICATE", "/synthetic/certificate.pem");
  EXPECT_THROW(LoadServerConfig(), std::invalid_argument);
  Set("WT_TLS_PRIVATE_KEY", "/synthetic/private-key.pem");
  Set("WT_TRADE_TARGET", "127.0.0.1:50053");
  const auto config = LoadServerConfig();
  ASSERT_TRUE(config.tls);
  EXPECT_EQ(config.tls->certificate, "/synthetic/certificate.pem");
  EXPECT_EQ(config.trade_target, "127.0.0.1:50053");
  for (const char *target : {"0.0.0.0:50053", "example.invalid:50053", "127.0.0.1:0", "127.0.0.1:-1", "127.0.0.1:70000", ""}) {
    Set("WT_TRADE_TARGET", target);
    EXPECT_THROW(LoadServerConfig(), std::invalid_argument);
  }
}

TEST_F(ConfigTest, KeepsBackendAndValidatorListenersPrivate) {
  EXPECT_THROW(LoadBackendConfig(), std::invalid_argument);
  Set("PLATINUMWT_DATABASE_URL", "dbname=synthetic");
  Set("WT_LEGALITY_POLICY", "synthetic-policy");
  EXPECT_EQ(LoadBackendConfig().address, "127.0.0.1:50053");
  Set("WT_BACKEND_ADDRESS", "0.0.0.0:50053");
  EXPECT_THROW(LoadBackendConfig(), std::invalid_argument);
  Set("WT_BACKEND_ADDRESS", "127.0.0.1:0");
  Set("WT_LEGALITY_TARGET", "public.invalid:50052");
  EXPECT_THROW(LoadBackendConfig(), std::invalid_argument);
  Set("WT_GRPC_ADDRESS", "0.0.0.0:50051");
  EXPECT_THROW(LoadServerConfig(), std::invalid_argument);
}

} // namespace
} // namespace platinumwt::server
