#pragma once

#include "bench.h"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/random_access_file.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/version.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace asio_bench
{
inline constexpr std::string_view library = "asio";
inline constexpr auto with_error = asio::as_tuple(asio::deferred);

[[nodiscard]] inline std::string version()
{
  return std::format("{}.{}.{}", ASIO_VERSION / 100000, ASIO_VERSION / 100 % 1000, ASIO_VERSION % 100);
}

[[nodiscard]] inline std::string reactor_backend()
{
#if defined(ASIO_HAS_IO_URING_AS_DEFAULT)
  return "io_uring";
#elif defined(ASIO_HAS_EPOLL)
  return "epoll";
#else
  return "select";
#endif
}

[[nodiscard]] inline std::string file_backend()
{
#if defined(ASIO_HAS_IO_URING_AS_DEFAULT)
  return "io_uring";
#elif defined(ASIO_HAS_IO_URING)
  return "io_uring via epoll";
#else
  return "none";
#endif
}

[[noreturn]] inline void fail(std::string_view what, const std::error_code &error)
{
  bench::fail(std::format("{}: {} ({})", what, error.message(), error.value()));
}

inline void require_io_uring(std::string_view who)
{
  try
  {
    asio::io_context context(1);
    asio::random_access_file probe(context);
  }
  catch (const asio::system_error &error)
  {
    bench::unsupported(std::format("{} needs io_uring, which this host refused: {}", who, error.what()));
  }
}

[[nodiscard]] inline std::int64_t positive(const bench::args_t &args, std::string_view key)
{
  auto value = args.integer(key);
  if (value <= 0)
    bench::fail(std::format("parameter '{}' must be positive", key));
  return value;
}

[[nodiscard]] inline std::int64_t non_negative(const bench::args_t &args, std::string_view key)
{
  auto value = args.integer(key);
  if (value < 0)
    bench::fail(std::format("parameter '{}' must not be negative", key));
  return value;
}

[[nodiscard]] inline double positive_seconds(const bench::args_t &args)
{
  auto value = args.real("seconds");
  if (!(value > 0.0))
    bench::fail("parameter 'seconds' must be positive");
  return value;
}

[[nodiscard]] inline double mib_per_s(std::int64_t bytes, std::int64_t ns)
{
  return double(bytes) / (1024.0 * 1024.0) / (double(ns) / 1e9);
}

[[nodiscard]] inline asio::ip::tcp::endpoint tcp_loopback(unsigned short port)
{
  return {asio::ip::address_v4::loopback(), port};
}

[[nodiscard]] inline asio::ip::tcp::acceptor listen_loopback(asio::io_context &context)
{
  return asio::ip::tcp::acceptor(context, tcp_loopback(0));
}

inline void spawn(const asio::any_io_executor &executor, asio::awaitable<void> task, std::string_view what)
{
  asio::co_spawn(executor, std::move(task), [what](std::exception_ptr error) {
    if (!error)
      return;
    try
    {
      std::rethrow_exception(error);
    }
    catch (const std::exception &exception)
    {
      bench::fail(std::format("{}: {}", what, exception.what()));
    }
  });
}

class window_t
{
public:
  explicit window_t(double seconds)
    : _warmup(bench::warmup_ns(seconds))
    , _measure(bench::seconds_ns(seconds) - bench::warmup_ns(seconds))
  {
  }

  window_t(const window_t &) = delete;
  window_t &operator=(const window_t &) = delete;

  asio::awaitable<void> run(std::function<void()> on_measure, std::function<void()> on_end)
  {
    asio::steady_timer timer(co_await asio::this_coro::executor, _warmup);
    co_await timer.async_wait(asio::deferred);
    _phase = phase_t::measuring;
    _start_ns = bench::now_ns();
    _start_cpu = bench::process_cpu_s();
    if (on_measure)
      on_measure();
    timer.expires_after(_measure);
    co_await timer.async_wait(asio::deferred);
    _phase = phase_t::ended;
    _end_ns = bench::now_ns();
    _end_cpu = bench::process_cpu_s();
    if (on_end)
      on_end();
  }

  [[nodiscard]] bool measuring() const
  {
    return _phase == phase_t::measuring;
  }

  [[nodiscard]] bool ended() const
  {
    return _phase == phase_t::ended;
  }

  [[nodiscard]] double elapsed_s() const
  {
    return double(_end_ns - _start_ns) / 1e9;
  }

  [[nodiscard]] double cpu_s() const
  {
    return _end_cpu - _start_cpu;
  }

private:
  enum class phase_t
  {
    warming_up,
    measuring,
    ended
  };

  std::chrono::nanoseconds _warmup;
  std::chrono::nanoseconds _measure;
  phase_t _phase = phase_t::warming_up;
  std::int64_t _start_ns = 0;
  std::int64_t _end_ns = 0;
  double _start_cpu = 0.0;
  double _end_cpu = 0.0;
};

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
