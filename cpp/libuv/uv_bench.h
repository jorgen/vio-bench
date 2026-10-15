#pragma once

#include "bench.h"

#include <uv.h>

#include <netinet/in.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace uvb
{
inline void check_uv(int rc, std::string_view what)
{
  if (rc < 0)
    bench::fail(std::format("{}: {}", what, uv_strerror(rc)));
}

[[nodiscard]] inline std::uint64_t ceil_ms(std::int64_t ns)
{
  return std::uint64_t((std::max<std::int64_t>(ns, 0) + 999'999) / 1'000'000);
}

[[nodiscard]] inline double mib_per_s(std::int64_t bytes, std::int64_t ns)
{
  return double(bytes) / (1024.0 * 1024.0) / (double(ns) / 1e9);
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

[[nodiscard]] inline sockaddr_in loopback(int port)
{
  sockaddr_in address{};
  check_uv(uv_ip4_addr("127.0.0.1", port, &address), "uv_ip4_addr");
  return address;
}

[[nodiscard]] inline int port_of(const sockaddr_storage &storage)
{
  return ntohs(reinterpret_cast<const sockaddr_in &>(storage).sin_port);
}

template <typename T>
[[nodiscard]] uv_handle_t *as_handle(T *handle)
{
  return reinterpret_cast<uv_handle_t *>(handle);
}

template <typename T>
[[nodiscard]] uv_stream_t *as_stream(T *handle)
{
  return reinterpret_cast<uv_stream_t *>(handle);
}

template <typename T>
void close_handle(T *handle, uv_close_cb on_closed = nullptr)
{
  if (!uv_is_closing(as_handle(handle)))
    uv_close(as_handle(handle), on_closed);
}

inline int listen_on_loopback(uv_tcp_t *listener, uv_connection_cb on_connection)
{
  auto address = loopback(0);
  check_uv(uv_tcp_bind(listener, reinterpret_cast<const sockaddr *>(&address), 0), "uv_tcp_bind");
  check_uv(uv_listen(as_stream(listener), SOMAXCONN, on_connection), "uv_listen");
  sockaddr_storage bound{};
  int length = sizeof(bound);
  check_uv(uv_tcp_getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &length), "uv_tcp_getsockname");
  return port_of(bound);
}

inline void accept_into(uv_stream_t *listener, uv_tcp_t *connection)
{
  check_uv(uv_tcp_init(listener->loop, connection), "uv_tcp_init");
  check_uv(uv_accept(listener, as_stream(connection)), "uv_accept");
  check_uv(uv_tcp_nodelay(connection, 1), "uv_tcp_nodelay");
}

inline void connect_to(uv_loop_t *loop, uv_tcp_t *tcp, uv_connect_t *request, const sockaddr_in &server, uv_connect_cb on_connected)
{
  check_uv(uv_tcp_init(loop, tcp), "uv_tcp_init");
  check_uv(uv_tcp_nodelay(tcp, 1), "uv_tcp_nodelay");
  check_uv(uv_tcp_connect(request, tcp, reinterpret_cast<const sockaddr *>(&server), on_connected), "uv_tcp_connect");
}

[[nodiscard]] inline bool io_uring_variant()
{
  const char *variant = std::getenv("BENCH_VARIANT");
  if (variant == nullptr || std::string_view(variant) != "io_uring")
    return false;
  const char *enabled = std::getenv("UV_USE_IO_URING");
  if (enabled == nullptr || std::atoi(enabled) <= 0)
    bench::fail("the io_uring variant needs UV_USE_IO_URING=1, or libuv 1.53 quietly keeps file I/O on its threadpool");
  return true;
}

class loop_t
{
public:
  loop_t()
  {
    check_uv(uv_loop_init(&_loop), "uv_loop_init");
    if (io_uring_variant())
      check_uv(uv_loop_configure(&_loop, UV_LOOP_USE_IO_URING_SQPOLL), "uv_loop_configure");
  }

  loop_t(const loop_t &) = delete;
  loop_t &operator=(const loop_t &) = delete;

  [[nodiscard]] uv_loop_t *get()
  {
    return &_loop;
  }

  void run()
  {
    uv_run(&_loop, UV_RUN_DEFAULT);
  }

  void run_and_close()
  {
    run();
    int rc = uv_loop_close(&_loop);
    if (rc == UV_EBUSY)
    {
      uv_walk(&_loop, report_open_handle, nullptr);
      bench::fail("uv_loop_close: handles still open");
    }
    check_uv(rc, "uv_loop_close");
  }

private:
  static void report_open_handle(uv_handle_t *handle, void *)
  {
    std::fprintf(stderr, "open handle: %s active=%d closing=%d\n", uv_handle_type_name(handle->type), uv_is_active(handle),
                 uv_is_closing(handle));
  }

  uv_loop_t _loop{};
};

class window_t
{
public:
  window_t(uv_loop_t *loop, double seconds)
    : _warmup_ms(ceil_ms(bench::warmup_ns(seconds)))
    , _measure_ms(ceil_ms(bench::seconds_ns(seconds) - bench::warmup_ns(seconds)))
  {
    check_uv(uv_timer_init(loop, &_timer), "uv_timer_init");
    _timer.data = this;
  }

  window_t(const window_t &) = delete;
  window_t &operator=(const window_t &) = delete;

  void start(std::function<void()> on_end, std::function<void()> on_measure = {})
  {
    _on_end = std::move(on_end);
    _on_measure = std::move(on_measure);
    _phase = phase_t::warming_up;
    uv_update_time(_timer.loop);
    check_uv(uv_timer_start(&_timer, on_timer, _warmup_ms, 0), "uv_timer_start");
  }

  [[nodiscard]] bool measuring() const
  {
    return _phase == phase_t::measuring;
  }

  [[nodiscard]] bool ended() const
  {
    return _phase == phase_t::ended;
  }

  [[nodiscard]] std::int64_t elapsed_ns() const
  {
    return _end_ns - _start_ns;
  }

  [[nodiscard]] double elapsed_s() const
  {
    return double(elapsed_ns()) / 1e9;
  }

  [[nodiscard]] double cpu_s() const
  {
    return _end_cpu - _start_cpu;
  }

private:
  enum class phase_t
  {
    idle,
    warming_up,
    measuring,
    ended
  };

  static void on_timer(uv_timer_t *timer)
  {
    auto *self = static_cast<window_t *>(timer->data);
    if (self->_phase == phase_t::warming_up)
    {
      self->_phase = phase_t::measuring;
      self->_start_ns = bench::now_ns();
      self->_start_cpu = bench::process_cpu_s();
      check_uv(uv_timer_start(timer, on_timer, self->_measure_ms, 0), "uv_timer_start");
      if (self->_on_measure)
        self->_on_measure();
      return;
    }
    self->_phase = phase_t::ended;
    self->_end_ns = bench::now_ns();
    self->_end_cpu = bench::process_cpu_s();
    close_handle(timer);
    self->_on_end();
  }

  uv_timer_t _timer{};
  std::uint64_t _warmup_ms;
  std::uint64_t _measure_ms;
  phase_t _phase = phase_t::idle;
  std::int64_t _start_ns = 0;
  std::int64_t _end_ns = 0;
  double _start_cpu = 0.0;
  double _end_cpu = 0.0;
  std::function<void()> _on_end;
  std::function<void()> _on_measure;
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
