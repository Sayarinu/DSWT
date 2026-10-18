#pragma once

#include <boost/asio/ip/address.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace platinumwt::server::detail {

// A bounded token bucket and concurrency lease. Idle entries expire only after
// their complete burst has replenished; rotating keys cannot reset a budget.
class Quota {
  struct State;
public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  struct Limits { unsigned total, per_key, burst, per_second; };
  class Permit {
  public:
    ~Permit();
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;
  private:
    friend class Quota;
    Permit(std::shared_ptr<State>, std::string key);
    std::shared_ptr<State> state_;
    std::string key_;
  };
  explicit Quota(Limits, Now now = Clock::now);
  std::unique_ptr<Permit> Acquire(std::string_view key);
private:
  std::shared_ptr<State> state_;
};

enum class RequestKind { Enrollment, Submission, Recovery };

// Shared by enrollment and trading within a backend process. Credentials stay
// out of the cache; a forged credential cannot consume the owner's key budget.
class ServiceAdmission {
public:
  std::unique_ptr<Quota::Permit> Acquire(RequestKind, std::string_view identity, std::string_view credential);
private:
  Quota submissions_{{4, 2, 16, 4}};
  Quota recovery_{{8, 2, 64, 16}};
};

// Direct peer only: IPv4 addresses and IPv6 /64s, including IPv4-mapped peers.
std::string NetworkKey(const boost::asio::ip::address &);

} // namespace platinumwt::server::detail
