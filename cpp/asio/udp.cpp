#include "asio_bench.h"

#include <asio/ip/udp.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

namespace asio_bench
{
namespace
{
using asio::ip::udp;

constexpr std::size_t receive_buffer = 64 * 1024;
constexpr std::int64_t largest_datagram = 65507;
constexpr std::chrono::milliseconds quiet_check(10);

class sequence_set_t
{
public:
  bool insert(std::uint64_t sequence)
  {
    auto word = std::size_t(sequence >> 6);
    if (word >= _words.size())
      _words.resize(std::max(word + 1, _words.size() * 2));
    auto bit = std::uint64_t(1) << (sequence & 63);
    bool fresh = (_words[word] & bit) == 0;
    _words[word] |= bit;
    return fresh;
  }

  [[nodiscard]] std::int64_t count(std::uint64_t first, std::uint64_t end) const
  {
    std::int64_t total = 0;
    for (auto sequence = first; sequence < end; ++sequence)
    {
      auto word = std::size_t(sequence >> 6);
      if (word >= _words.size())
        break;
      if ((sequence & 63) == 0 && sequence + 64 <= end)
      {
        total += std::popcount(_words[word]);
        sequence += 63;
        continue;
      }
      total += std::int64_t((_words[word] >> (sequence & 63)) & 1);
    }
    return total;
  }

private:
  std::vector<std::uint64_t> _words;
};

void set_buffers(udp::socket &socket)
{
  socket.set_option(asio::socket_base::send_buffer_size(bench::udp_socket_buffer));
  socket.set_option(asio::socket_base::receive_buffer_size(bench::udp_socket_buffer));
}

struct receiver_state_t
{
  std::int64_t size = 0;
  sequence_set_t seen;
  std::int64_t datagrams = 0;
  std::int64_t duplicates = 0;
  bool stopping = false;
};

asio::awaitable<void> receive(udp::socket &socket, receiver_state_t &state)
{
  std::vector<char> buffer(receive_buffer);
  udp::endpoint sender;
  for (;;)
  {
    auto [error, length] = co_await socket.async_receive_from(asio::buffer(buffer), sender, with_error);
    if (error)
    {
      if (error == asio::error::operation_aborted && state.stopping)
        co_return;
      fail("udp receive", error);
    }
    bench::check(std::int64_t(length) == state.size, "datagram of the wrong size");
    std::uint64_t sequence = 0;
    std::memcpy(&sequence, buffer.data(), sizeof(sequence));
    if (!state.seen.insert(sequence))
      ++state.duplicates;
    ++state.datagrams;
  }
}

asio::awaitable<void> stop_when_quiet(udp::socket &socket, receiver_state_t &state, std::int64_t give_up_ns)
{
  asio::steady_timer timer(co_await asio::this_coro::executor);
  std::int64_t last_count = 0;
  auto last_change_ns = bench::now_ns();
  for (;;)
  {
    timer.expires_after(quiet_check);
    co_await timer.async_wait(asio::deferred);
    auto now = bench::now_ns();
    if (state.datagrams != last_count)
    {
      last_count = state.datagrams;
      last_change_ns = now;
      continue;
    }
    bool heard = last_count > 0;
    if (heard && now - last_change_ns >= bench::udp_quiet_ns)
      break;
    if (!heard && now >= give_up_ns)
      break;
  }
  state.stopping = true;
  socket.close();
}

void run_receiver(receiver_state_t &state, double seconds, std::promise<unsigned short> &port)
{
  asio::io_context context(1);
  udp::socket socket(context, udp::endpoint(asio::ip::address_v4::loopback(), 0));
  set_buffers(socket);
  auto give_up_ns = bench::now_ns() + bench::seconds_ns(seconds) + bench::udp_quiet_ns;
  port.set_value(socket.local_endpoint().port());
  spawn(context.get_executor(), receive(socket, state), "udp receiver");
  spawn(context.get_executor(), stop_when_quiet(socket, state, give_up_ns), "udp quiet watch");
  context.run();
}

struct sender_t
{
  sender_t(asio::io_context &context, double seconds)
    : socket(context, udp::endpoint(asio::ip::address_v4::loopback(), 0))
    , window(seconds)
  {
  }

  udp::socket socket;
  window_t window;
  std::uint64_t next_sequence = 0;
  std::uint64_t first_measured = 0;
  std::uint64_t end_sequence = 0;
};

asio::awaitable<void> flood(sender_t &sender, udp::endpoint destination, std::vector<std::byte> datagram)
{
  auto executor = co_await asio::this_coro::executor;
  spawn(executor,
        sender.window.run([&sender] { sender.first_measured = sender.next_sequence; }, [&sender] { sender.end_sequence = sender.next_sequence; }),
        "udp_flood window");
  while (!sender.window.ended())
  {
    auto sequence = sender.next_sequence++;
    std::memcpy(datagram.data(), &sequence, sizeof(sequence));
    co_await sender.socket.async_send_to(asio::buffer(datagram), destination, asio::deferred);
  }
}
}

bench::result_t udp_flood(const bench::args_t &args)
{
  auto size = positive(args, "size");
  auto seconds = positive_seconds(args);
  auto batch = non_negative(args, "batch");
  bench::check(size >= std::int64_t(sizeof(std::uint64_t)) && size <= largest_datagram,
               "size must hold a 64-bit sequence number and fit in one datagram");
  if (batch > 0)
    bench::unsupported("Asio has no batch datagram API (no sendmmsg, recvmmsg, GSO or GRO); "
                       "async_send_to and async_receive_from carry one datagram per call");

  receiver_state_t receiver;
  receiver.size = size;
  std::promise<unsigned short> port;
  auto port_ready = port.get_future();
  std::thread receiver_thread([&] { run_receiver(receiver, seconds, port); });

  asio::io_context context(1);
  sender_t sender(context, seconds);
  set_buffers(sender.socket);
  udp::endpoint destination(asio::ip::address_v4::loopback(), port_ready.get());
  spawn(context.get_executor(), flood(sender, destination, bench::payload(std::size_t(size))), "udp_flood sender");
  context.run();
  receiver_thread.join();

  auto sent = std::int64_t(sender.end_sequence - sender.first_measured);
  auto received = receiver.seen.count(sender.first_measured, sender.end_sequence);
  bench::check(receiver.duplicates == 0, "received a datagram twice");
  auto window = sender.window.elapsed_s();
  bench::result_t result;
  result.metrics["sent_per_s"] = double(sent) / window;
  result.metrics["received_per_s"] = double(received) / window;
  result.metrics["loss_pct"] = sent > 0 ? 100.0 * double(sent - received) / double(sent) : 0.0;
  result.work["sent"] = sent;
  result.work["received"] = received;
  result.work["backend"] = reactor_backend();
  return result;
}
}
