#include "admission.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace dswt::server::detail {

struct Quota::State {
  struct Bucket {
    double tokens;
    Clock::time_point refreshed, touched;
    unsigned active = 0;
  };
  Limits limits;
  Now now;
  std::mutex mutex;
  unsigned active = 0;
  std::map<std::string, Bucket, std::less<>> buckets;
  Clock::time_point next_sweep{};

  State(Limits value, Now clock) : limits(value), now(std::move(clock)) {}
};

Quota::Quota(Limits limits, Now now) : state_(std::make_shared<State>(limits, std::move(now))) {
  if (!limits.total || !limits.per_key || !limits.burst || !limits.per_second ||
      limits.burst > 60 * static_cast<std::uint64_t>(limits.per_second) || !state_->now)
    throw std::invalid_argument("invalid admission budget");
}

Quota::Permit::Permit(std::shared_ptr<State> state, std::string key) : state_(std::move(state)), key_(std::move(key)) {}
Quota::Permit::~Permit() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  --state_->active;
  if (!key_.empty()) --state_->buckets.at(key_).active;
}

std::unique_ptr<Quota::Permit> Quota::Acquire() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->active >= state_->limits.total) return {};
  auto permit = std::unique_ptr<Permit>(new Permit(state_, {}));
  ++state_->active;
  return permit;
}

std::unique_ptr<Quota::Permit> Quota::Acquire(std::string_view key) {
  auto permit = Acquire();
  if (!permit || !permit->Bind(key)) return {};
  return permit;
}

bool Quota::Permit::Bind(std::string_view key) {
  if (key.empty() || key.size() > 64) return false;
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!key_.empty()) return key_ == key;
  const auto now = state_->now();
  auto found = state_->buckets.find(key);
  if (found == state_->buckets.end()) {
    if (state_->buckets.size() == 4096 && now >= state_->next_sweep) {
      state_->next_sweep = now + std::chrono::seconds(1);
      for (auto it = state_->buckets.begin(); it != state_->buckets.end();) {
        if (it->second.active == 0 && now - it->second.touched >= std::chrono::seconds(60)) it = state_->buckets.erase(it);
        else ++it;
      }
    }
    if (state_->buckets.size() == 4096) return false;
    found = state_->buckets.emplace(std::string(key), State::Bucket{static_cast<double>(state_->limits.burst), now, now}).first;
  }
  auto &bucket = found->second;
  const auto elapsed = std::max(0.0, std::chrono::duration<double>(now - bucket.refreshed).count());
  bucket.tokens = std::min(static_cast<double>(state_->limits.burst), bucket.tokens + elapsed * state_->limits.per_second);
  bucket.refreshed = std::max(now, bucket.refreshed);
  bucket.touched = std::max(now, bucket.touched);
  if (bucket.active >= state_->limits.per_key || bucket.tokens < 1) return false;
  key_ = found->first;
  --bucket.tokens;
  ++bucket.active;
  return true;
}

EnrollmentBudget::EnrollmentBudget(Limits limits, Quota::Now now)
    : limits_(limits), now_(std::move(now)), hour_started_(now_()) {
  if (!limits.per_network_daily || !limits.hourly) throw std::invalid_argument("invalid enrollment budget");
}

bool EnrollmentBudget::Allow(const std::string &network, std::string_view request) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  std::size_t size = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, request.data(), request.size(), digest.data(), &size) != 1 || size != 32)
    throw std::runtime_error("admission hashing unavailable");
  const std::string key(reinterpret_cast<const char *>(digest.data()), 16);
  const auto now = now_();
  std::lock_guard<std::mutex> lock(mutex_);
  if (now - hour_started_ >= std::chrono::hours(1)) { hour_started_ = now; hour_count_ = 0; }
  auto found = networks_.find(network);
  if (found != networks_.end() && now - found->second.started >= std::chrono::hours(24)) {
    networks_.erase(found);
    found = networks_.end();
  }
  if (found == networks_.end()) {
    if (networks_.size() >= 4096) {
      for (auto it = networks_.begin(); it != networks_.end();) {
        if (now - it->second.started >= std::chrono::hours(24)) it = networks_.erase(it);
        else ++it;
      }
      if (networks_.size() >= 4096) return false;
    }
    found = networks_.emplace(network, Network{now, {}}).first;
  }
  auto &seen = found->second.seen;
  if (seen.count(key)) return true;
  if (seen.size() >= limits_.per_network_daily || hour_count_ >= limits_.hourly) return false;
  seen.insert(key);
  ++hour_count_;
  return true;
}

std::unique_ptr<Quota::Permit> ServiceAdmission::Acquire(RequestKind kind) {
  return (kind == RequestKind::Recovery ? recovery_ : submissions_).Acquire();
}

bool ServiceAdmission::Authorize(Quota::Permit &permit, std::string_view identity, std::string_view credential) {
  if (identity.size() != 16 || credential.size() != 32) return false;
  std::array<unsigned char, 49> input{};
  input[0] = 2;
  std::copy(identity.begin(), identity.end(), input.begin() + 1);
  std::copy(credential.begin(), credential.end(), input.begin() + 17);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  std::size_t size = 0;
  const auto result = EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(), input.size(), digest.data(), &size);
  OPENSSL_cleanse(input.data(), input.size());
  if (result != 1 || size != 32) throw std::runtime_error("admission hashing unavailable");
  const std::string key(reinterpret_cast<const char *>(digest.data()), size);
  OPENSSL_cleanse(digest.data(), digest.size());
  return permit.Bind(key);
}

std::string NetworkKey(const boost::asio::ip::address &address) {
  if (address.is_v4()) {
    const auto bytes = address.to_v4().to_bytes();
    return std::string(1, '\4') + std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  const auto bytes = address.to_v6().to_bytes();
  const bool mapped = std::all_of(bytes.begin(), bytes.begin() + 10, [](auto byte) { return byte == 0; }) &&
      bytes[10] == 255 && bytes[11] == 255;
  return std::string(1, mapped ? '\4' : '\6') +
      std::string(reinterpret_cast<const char *>(bytes.data() + (mapped ? 12 : 0)), mapped ? 4 : 8);
}

} // namespace dswt::server::detail
