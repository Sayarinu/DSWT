#include "platinumwt/server/metrics_server.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <libpq-fe.h>

#include <condition_variable>
#include <thread>

namespace platinumwt::server {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace {
class Scrape : public std::enable_shared_from_this<Scrape> {
public:
  Scrape(tcp::socket socket, std::shared_ptr<std::atomic<unsigned>> active)
      : stream_(std::move(socket)), active_(std::move(active)) {
    parser_.header_limit(1024); parser_.body_limit(0);
  }
  ~Scrape() { active_->fetch_sub(1); }
  void Run() {
    stream_.expires_after(std::chrono::seconds(2));
    http::async_read(stream_, buffer_, parser_, [self = shared_from_this()](beast::error_code error, std::size_t) {
      if (error) return;
      const auto &request = self->parser_.get();
      self->response_.version(11);
      self->response_.keep_alive(false);
      self->response_.set(http::field::cache_control, "no-store");
      if (request.method() != http::verb::get) self->response_.result(http::status::method_not_allowed);
      else if (request.target() != "/metrics") self->response_.result(http::status::not_found);
      else {
        self->response_.result(http::status::ok);
        self->response_.set(http::field::content_type, "text/plain; version=0.0.4; charset=utf-8");
        self->response_.body() = Telemetry::Global().Render();
      }
      self->response_.prepare_payload();
      self->stream_.expires_after(std::chrono::seconds(2));
      http::async_write(self->stream_, self->response_, [self](beast::error_code, std::size_t) {
        beast::error_code ignored;
        self->stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
      });
    });
  }
private:
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_{2048};
  http::request_parser<http::empty_body> parser_;
  http::response<http::string_body> response_;
  std::shared_ptr<std::atomic<unsigned>> active_;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context &io, unsigned short port)
      : acceptor_(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)) {}
  void Run() {
    acceptor_.async_accept([self = shared_from_this()](beast::error_code error, tcp::socket socket) {
      if (!error) {
        if (self->active_->fetch_add(1) < 16) std::make_shared<Scrape>(std::move(socket), self->active_)->Run();
        else self->active_->fetch_sub(1);
      }
      if (self->acceptor_.is_open()) self->Run();
    });
  }
  void Stop() { beast::error_code ignored; acceptor_.close(ignored); }
private:
  tcp::acceptor acceptor_;
  std::shared_ptr<std::atomic<unsigned>> active_ = std::make_shared<std::atomic<unsigned>>(0);
};
} // namespace

LedgerMetrics SampleLedgerMetrics(const std::string &database) {
  LedgerMetrics metrics;
  const char *keys[] = {"dbname", "connect_timeout", "options", "application_name", nullptr};
  const char *values[] = {database.c_str(), "1", "-c statement_timeout=250 -c lock_timeout=100 -c default_transaction_read_only=on", "platinumwt-metrics", nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdbParams(keys, values, 1), PQfinish);
  if (!connection || PQstatus(connection.get()) != CONNECTION_OK) return metrics;
  PQsetNoticeProcessor(connection.get(), [](void *, const char *) {}, nullptr);
  std::unique_ptr<PGresult, decltype(&PQclear)> row(PQexec(connection.get(),
      "SELECT count(*) FILTER (WHERE o.state=1), count(*) FILTER (WHERE o.state IN (2,3)), "
      "coalesce(max(greatest(extract(epoch FROM clock_timestamp()-a.recorded_at),0)) FILTER (WHERE o.state=1),0), "
      "coalesce(max(greatest(extract(epoch FROM clock_timestamp()-a.recorded_at),0)) FILTER (WHERE o.state IN (2,3)),0), "
      "count(*) FILTER (WHERE o.state=1 AND a.recorded_at IS NULL), "
      "count(*) FILTER (WHERE o.state IN (2,3) AND a.recorded_at IS NULL), "
      "(SELECT count(*) FROM platinumwt.matches), "
      "(SELECT count(*) FROM platinumwt.matches m JOIN platinumwt.offers x ON x.trade_id=m.first_trade_id "
      "JOIN platinumwt.offers y ON y.trade_id=m.second_trade_id WHERE x.state=4 AND y.state=4), "
      "(SELECT count(*) FROM platinumwt.offers WHERE state=4), "
      "(NOT pg_is_in_recovery() AND current_setting('fsync')='on' AND current_setting('full_page_writes')='on') "
      "FROM platinumwt.offers o LEFT JOIN platinumwt.audit_journal a ON a.table_name='offers' "
      "AND a.row_key->>'trade_id'=o.trade_id::text AND (a.after_image->>'last_event')::numeric=o.last_event "
      "WHERE o.state IN (1,2,3)"), PQclear);
  if (!row || PQresultStatus(row.get()) != PGRES_TUPLES_OK || PQntuples(row.get()) != 1) return metrics;
  try {
    metrics.queued = std::stoull(PQgetvalue(row.get(), 0, 0));
    metrics.pending = std::stoull(PQgetvalue(row.get(), 0, 1));
    metrics.oldest_queue_seconds = std::stod(PQgetvalue(row.get(), 0, 2));
    metrics.oldest_delivery_seconds = std::stod(PQgetvalue(row.get(), 0, 3));
    metrics.unknown_queue_ages = std::stoull(PQgetvalue(row.get(), 0, 4));
    metrics.unknown_delivery_ages = std::stoull(PQgetvalue(row.get(), 0, 5));
    metrics.matched = std::stoull(PQgetvalue(row.get(), 0, 6));
    metrics.completed = std::stoull(PQgetvalue(row.get(), 0, 7));
    metrics.acknowledged = std::stoull(PQgetvalue(row.get(), 0, 8));
    metrics.up = std::string_view(PQgetvalue(row.get(), 0, 9)) == "t";
  } catch (const std::exception &) { return {}; }
  return metrics;
}

struct MetricsServer::Impl {
  asio::io_context io;
  std::shared_ptr<Listener> listener;
  std::mutex mutex;
  std::condition_variable changed;
  bool stopped = false;
  std::thread sampler, network;
  Impl(unsigned short port, std::string database) : listener(std::make_shared<Listener>(io, port)) {
    listener->Run();
    try {
      network = std::thread([this] { try { io.run(); } catch (...) {} });
      if (!database.empty()) sampler = std::thread([this, database = std::move(database)] {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopped) {
          lock.unlock();
          Telemetry::Global().SetLedger(SampleLedgerMetrics(database));
          lock.lock();
          changed.wait_for(lock, std::chrono::seconds(5), [this] { return stopped; });
        }
      });
    } catch (...) { Stop(); throw; }
  }
  void Stop() {
    { std::lock_guard<std::mutex> lock(mutex); stopped = true; }
    changed.notify_all();
    io.stop();
    if (network.joinable()) network.join();
    listener->Stop();
    if (sampler.joinable()) sampler.join();
  }
  ~Impl() { Stop(); }
};

MetricsServer::MetricsServer(unsigned short port, std::string database) {
  if (port) impl_ = std::make_unique<Impl>(port, std::move(database));
}
MetricsServer::~MetricsServer() = default;

} // namespace platinumwt::server
