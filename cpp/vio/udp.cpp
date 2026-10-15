#include "support.h"

#include <vio/buffer_pool.h>
#include <vio/operation/udp.h>
#include <vio/operation/udp_datagram.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <future>
#include <span>
#include <thread>
#include <vector>

namespace vio_bench
{
namespace
{
constexpr std::uint64_t sequence_ceiling = std::uint64_t{1} << 40;
constexpr std::int64_t quiet_poll_ns = 10'000'000;
constexpr std::int64_t give_up_ns = 10'000'000'000;
constexpr std::size_t largest_udp_payload = 65507;
constexpr std::int64_t largest_segment_count = 64;
constexpr std::int64_t clock_stride = 16;

struct received_t
{
  std::vector<std::uint64_t> seen;
  std::int64_t total = 0;

  void mark(const void *data, std::size_t length)
  {
    if (length < sizeof(std::uint64_t))
      return;
    std::uint64_t sequence = 0;
    std::memcpy(&sequence, data, sizeof(sequence));
    if (sequence >= sequence_ceiling)
      return;
    const auto word = std::size_t(sequence >> 6);
    if (word >= seen.size())
      seen.resize(std::max(word + 1, seen.size() * 2));
    seen[word] |= std::uint64_t{1} << (sequence & 63);
    ++total;
  }

  [[nodiscard]] std::int64_t count_from(std::uint64_t first) const
  {
    std::int64_t count = 0;
    const auto first_word = std::size_t(first >> 6);
    for (std::size_t word = first_word; word < seen.size(); ++word)
    {
      auto bits = seen[word];
      if (word == first_word)
        bits &= ~std::uint64_t{0} << (first & 63);
      count += std::popcount(bits);
    }
    return count;
  }
};

struct sent_t
{
  std::uint64_t total = 0;
  std::uint64_t window_first = 0;
  std::int64_t window_started_at = 0;
  std::int64_t ended_at = 0;
  bool window_open = false;
};

struct udp_endpoint_t
{
  vio::udp_t socket;
  std::uint16_t port = 0;
};

udp_endpoint_t open_socket(vio::event_loop_t &loop, vio::udp_recvmmsg_t recvmmsg)
{
  auto socket = vio::udp_create(loop, recvmmsg);
  require(socket, "udp_create");
  auto address = loopback(0);
  require(vio::udp_bind(*socket, as_sockaddr(address)), "udp_bind");
  require(vio::udp_set_send_buffer_size(*socket, bench::udp_socket_buffer), "SO_SNDBUF");
  require(vio::udp_set_recv_buffer_size(*socket, bench::udp_socket_buffer), "SO_RCVBUF");
  auto name = vio::udp_sockname(*socket);
  require(name, "udp_sockname");
  return {std::move(*socket), port_of(*name)};
}

struct listening_t
{
  std::int64_t sending_ns = 0;
  std::int64_t give_up_at = 0;
};

template <typename Reader>
vio::task_t<void> stop_when_quiet(vio::event_loop_t &loop, Reader &reader, const received_t &received, listening_t listening)
{
  std::int64_t last_total = 0;
  std::int64_t last_change = 0;
  std::int64_t sender_done_by = 0;
  for (;;)
  {
    require(co_await vio::sleep(loop, std::chrono::nanoseconds(quiet_poll_ns)), "sleep");
    const auto now = bench::now_ns();
    if (received.total != last_total)
    {
      if (last_total == 0)
        sender_done_by = now + listening.sending_ns;
      last_total = received.total;
      last_change = now;
      continue;
    }
    if (received.total > 0 && now >= sender_done_by && now - last_change >= bench::udp_quiet_ns)
      break;
    if (received.total == 0 && now >= listening.give_up_at)
      break;
  }
  reader.cancel();
}

vio::task_t<void> receive_each(vio::event_loop_t &loop, vio::udp_t socket, received_t &received, listening_t listening)
{
  auto reader = vio::udp_create_reader(socket);
  require(reader, "udp_create_reader");
  auto watchdog = stop_when_quiet(loop, *reader, received, listening);
  for (;;)
  {
    auto datagram = co_await *reader;
    if (!datagram.has_value())
    {
      if (vio::is_cancelled(datagram.error()))
        break;
      fail("udp receive", datagram.error());
    }
    received.mark(datagram->data.data(), datagram->data.size());
  }
  co_await std::move(watchdog);
}

vio::task_t<void> receive_batches(vio::event_loop_t &loop, vio::udp_t socket, std::size_t size, received_t &received, listening_t listening)
{
  auto reader = vio::datagram_reader_t::create(loop, socket);
  require(reader, "datagram_reader_t::create");
  require(reader->receive_datagram_size(std::max<std::size_t>(size, 2048)), "receive_datagram_size");
  auto watchdog = stop_when_quiet(loop, *reader, received, listening);
  for (;;)
  {
    auto batch = co_await reader->receive();
    if (!batch.has_value())
    {
      if (vio::is_cancelled(batch.error()))
        break;
      if (batch.error().code == vio::errc::again)
        continue;
      fail("udp batch receive", batch.error());
    }
    for (const auto &datagram : *batch)
    {
      if (!datagram.truncated)
        received.mark(datagram.data.data(), datagram.data.size());
    }
  }
  co_await std::move(watchdog);
}

bool transient(const vio::error_t &error)
{
  return error.code == vio::errc::again || error.code == vio::errc::no_buffer_space;
}

void note_clock(sent_t &sent, const window_t &window, std::int64_t now)
{
  if (!sent.window_open && now >= window.warm)
  {
    sent.window_open = true;
    sent.window_first = sent.total;
    sent.window_started_at = now;
  }
}

vio::task_t<void> send_each(vio::udp_t socket, sockaddr_in destination, std::size_t size, double seconds, sent_t &sent)
{
  auto bytes = bench::payload(size);
  std::vector<std::uint8_t> payload(size);
  std::memcpy(payload.data(), bytes.data(), size);
  const auto window = window_t::starting(bench::now_ns(), seconds);
  for (std::int64_t attempt = 0;; ++attempt)
  {
    if (attempt % clock_stride == 0)
    {
      const auto now = bench::now_ns();
      if (now >= window.end)
      {
        sent.ended_at = now;
        break;
      }
      note_clock(sent, window, now);
    }
    std::memcpy(payload.data(), &sent.total, sizeof(sent.total));
    auto result = co_await vio::send_udp(socket, payload.data(), payload.size(), as_sockaddr(destination));
    if (!result.has_value())
    {
      if (transient(result.error()))
        continue;
      fail("send_udp", result.error());
    }
    ++sent.total;
  }
}

vio::task_t<void> send_segmented(vio::event_loop_t &loop, vio::udp_t socket, sockaddr_in destination, std::size_t size, std::int64_t batch, double seconds, sent_t &sent)
{
  const auto per_call = std::size_t(std::min({batch, largest_segment_count, std::int64_t(largest_udp_payload / size)}));
  if (per_call == 0)
    bench::unsupported("a datagram this large cannot be segmented");
  const auto pattern = bench::payload(size);
  std::vector<std::uint8_t> buffer(per_call * size);
  for (std::size_t segment = 0; segment < per_call; ++segment)
    std::memcpy(buffer.data() + segment * size, pattern.data(), size);
  const auto window = window_t::starting(bench::now_ns(), seconds);
  for (;;)
  {
    const auto now = bench::now_ns();
    if (now >= window.end)
    {
      sent.ended_at = now;
      break;
    }
    note_clock(sent, window, now);
    for (std::size_t segment = 0; segment < per_call; ++segment)
    {
      const std::uint64_t sequence = sent.total + segment;
      std::memcpy(buffer.data() + segment * size, &sequence, sizeof(sequence));
    }
    auto result = vio::send_segments(socket, std::span<const std::uint8_t>(buffer), size, as_sockaddr(destination), vio::udp_ecn_t::not_ect);
    if (!result.has_value())
    {
      if (transient(result.error()))
      {
        require(co_await vio::sleep(loop, std::chrono::nanoseconds(0)), "sleep");
        continue;
      }
      if (sent.total == 0)
        bench::unsupported(std::format("send_segments refused: {}", result.error().msg));
      fail("send_segments", result.error());
    }
    sent.total += per_call;
  }
}
}

bench::result_t udp_flood(const bench::args_t &args)
{
  const auto size = std::size_t(positive(args, "size"));
  const auto seconds = positive_real(args, "seconds");
  const auto batch = args.integer("batch");
  bench::check(batch >= 0, "parameter 'batch' must not be negative");
  bench::check(size >= sizeof(std::uint64_t), "parameter 'size' must hold a 64-bit sequence number");
  bench::check(size <= largest_udp_payload, "parameter 'size' is larger than a UDP datagram");

  std::promise<std::uint16_t> port_promise;
  auto port_future = port_promise.get_future();
  received_t received;
  std::thread receiver_thread([&] {
    vio::buffer_pool_t receive_pool(std::max<std::size_t>(size, vio::default_pool_block_size));
    run_loop([&](vio::event_loop_t &loop) {
      auto endpoint = open_socket(loop, vio::udp_recvmmsg_t::off);
      if (batch == 0)
        require(vio::udp_use_buffer_pool(endpoint.socket, receive_pool), "udp_use_buffer_pool");
      port_promise.set_value(endpoint.port);
      const listening_t listening{bench::seconds_ns(seconds), bench::now_ns() + bench::seconds_ns(seconds) + give_up_ns};
      if (batch == 0)
        return receive_each(loop, std::move(endpoint.socket), received, listening);
      return receive_batches(loop, std::move(endpoint.socket), size, received, listening);
    });
  });

  sent_t sent;
  std::string backend;
  std::thread sender_thread([&] {
    const auto destination = loopback(port_future.get());
    backend = run_loop([&](vio::event_loop_t &loop) {
      auto endpoint = open_socket(loop, vio::udp_recvmmsg_t::off);
      if (batch == 0)
        return send_each(std::move(endpoint.socket), destination, size, seconds, sent);
      return send_segmented(loop, std::move(endpoint.socket), destination, size, batch, seconds, sent);
    });
  });
  sender_thread.join();
  receiver_thread.join();

  bench::check(sent.window_open, "the sender never reached the end of its warm-up");
  bench::check(received.total > 0, "the receiver got nothing");
  const auto sent_in_window = std::int64_t(sent.total - sent.window_first);
  const auto received_in_window = received.count_from(sent.window_first);
  const auto elapsed = sent.ended_at - sent.window_started_at;

  bench::result_t out;
  out.metrics["sent_per_s"] = per_second(sent_in_window, elapsed);
  out.metrics["received_per_s"] = per_second(received_in_window, elapsed);
  out.metrics["loss_pct"] = sent_in_window > 0 ? 100.0 * double(sent_in_window - received_in_window) / double(sent_in_window) : 0.0;
  out.work["sent"] = sent_in_window;
  out.work["received"] = received_in_window;
  out.work["backend"] = backend;
  return out;
}
}
