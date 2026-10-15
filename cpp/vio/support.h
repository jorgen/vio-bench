#pragma once

#include "bench.h"

#include <vio/error.h>
#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/task.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace vio_bench
{
inline constexpr std::string_view library = "vio";
inline constexpr std::string_view version = VIO_BENCH_VERSION;
inline constexpr int listen_backlog = 1024;
inline constexpr double mib = 1024.0 * 1024.0;

[[noreturn]] inline void fail(std::string_view what, const vio::error_t &error)
{
  bench::fail(std::format("{}: {} ({})", what, error.msg, error.code));
}

template <typename T>
void require(const std::expected<T, vio::error_t> &result, std::string_view what)
{
  if (!result.has_value())
    fail(what, result.error());
}

inline std::int64_t positive(const bench::args_t &args, std::string_view key)
{
  auto value = args.integer(key);
  if (value <= 0)
    bench::fail(std::format("parameter '{}' must be positive", key));
  return value;
}

inline double positive_real(const bench::args_t &args, std::string_view key)
{
  auto value = args.real(key);
  if (!(value > 0.0))
    bench::fail(std::format("parameter '{}' must be positive", key));
  return value;
}

struct window_t
{
  std::int64_t start = 0;
  std::int64_t warm = 0;
  std::int64_t end = 0;

  static window_t starting(std::int64_t start, double seconds)
  {
    return {start, start + bench::warmup_ns(seconds), start + bench::seconds_ns(seconds)};
  }

  [[nodiscard]] bool contains(std::int64_t at) const
  {
    return at >= warm && at < end;
  }

  [[nodiscard]] double measured_s() const
  {
    return double(end - warm) / 1e9;
  }
};

struct cpu_window_t
{
  double at_warm = 0.0;
  double at_end = 0.0;

  [[nodiscard]] double used() const
  {
    return at_end - at_warm;
  }
};

inline std::chrono::nanoseconds until(std::int64_t deadline)
{
  return std::chrono::nanoseconds(deadline - bench::now_ns());
}

inline vio::task_t<void> meter_cpu(vio::event_loop_t &loop, window_t window, cpu_window_t &cpu)
{
  require(co_await vio::sleep(loop, until(window.warm)), "sleep");
  cpu.at_warm = bench::process_cpu_s();
  require(co_await vio::sleep(loop, until(window.end)), "sleep");
  cpu.at_end = bench::process_cpu_s();
}

inline vio::task_t<void> stop_after(vio::event_loop_t &loop, vio::task_t<void> body)
{
  co_await std::move(body);
  {
    auto finished = std::move(body);
  }
  loop.stop();
}

inline std::string requested_backend(const vio::event_loop_t &loop)
{
  std::string backend = loop.backend_name();
  const char *requested = std::getenv("VIO_BACKEND");
  if (requested != nullptr && *requested != '\0' && backend != requested)
    bench::unsupported(std::format("VIO_BACKEND={} requested but the loop runs on {}", requested, backend));
  return backend;
}

inline std::string run_loop(const std::function<vio::task_t<void>(vio::event_loop_t &)> &start)
{
  vio::event_loop_t loop;
  std::string backend = requested_backend(loop);
  std::optional<vio::task_t<void>> task;
  loop.run_in_loop([&] { task.emplace(stop_after(loop, start(loop))); });
  loop.run();
  return backend;
}

inline sockaddr_in loopback(std::uint16_t port)
{
  auto address = vio::ip4_addr("127.0.0.1", port);
  require(address, "ip4_addr");
  return *address;
}

inline const sockaddr *as_sockaddr(const sockaddr_in &address)
{
  return reinterpret_cast<const sockaddr *>(&address);
}

inline std::uint16_t port_of(const sockaddr_storage &storage)
{
  bench::check(storage.ss_family == AF_INET, "bound address is not IPv4");
  return ntohs(reinterpret_cast<const sockaddr_in *>(&storage)->sin_port);
}

inline void set_nodelay(vio::native_socket_t socket)
{
  int on = 1;
  bench::check(::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0, "setsockopt TCP_NODELAY failed");
}

inline double per_second(std::int64_t count, std::int64_t elapsed_ns)
{
  return elapsed_ns > 0 ? double(count) * 1e9 / double(elapsed_ns) : 0.0;
}

inline double mib_per_second(std::int64_t bytes, std::int64_t elapsed_ns)
{
  return elapsed_ns > 0 ? double(bytes) / mib * 1e9 / double(elapsed_ns) : 0.0;
}

bench::result_t tcp_rr(const bench::args_t &args);
bench::result_t tcp_bulk(const bench::args_t &args);
bench::result_t tls_bulk(const bench::args_t &args);
bench::result_t tls_handshake(const bench::args_t &args);
bench::result_t udp_flood(const bench::args_t &args);
bench::result_t file_seq(const bench::args_t &args);
bench::result_t file_rand(const bench::args_t &args);
bench::result_t timer_accuracy(const bench::args_t &args);
bench::result_t timer_churn(const bench::args_t &args);
bench::result_t wake_pingpong(const bench::args_t &args);
bench::result_t wake_flood(const bench::args_t &args);
}
