#include "uv_bench.h"

#include <bit>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

namespace uvb
{
namespace
{
constexpr std::size_t receive_slot = 64 * 1024;
constexpr std::int64_t largest_datagram = 65507;
constexpr std::uint64_t quiet_check_ms = 10;
constexpr int no_recvmmsg = -1;
constexpr std::size_t libuv_mmsg_width = 20;

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
    for (auto sequence = first; sequence < end;)
    {
      auto word = std::size_t(sequence >> 6);
      if (word >= _words.size())
        break;
      auto low = sequence & 63;
      auto high = std::min<std::uint64_t>(64, low + (end - sequence));
      auto mask = high == 64 ? ~std::uint64_t(0) << low : ((std::uint64_t(1) << high) - 1) & (~std::uint64_t(0) << low);
      total += std::popcount(_words[word] & mask);
      sequence += high - low;
    }
    return total;
  }

private:
  std::vector<std::uint64_t> _words;
};

void set_socket_buffers(uv_udp_t *udp)
{
  int size = bench::udp_socket_buffer;
  check_uv(uv_send_buffer_size(as_handle(udp), &size), "uv_send_buffer_size");
  size = bench::udp_socket_buffer;
  check_uv(uv_recv_buffer_size(as_handle(udp), &size), "uv_recv_buffer_size");
}

void bind_loopback(uv_udp_t *udp)
{
  auto address = loopback(0);
  check_uv(uv_udp_bind(udp, reinterpret_cast<const sockaddr *>(&address), 0), "uv_udp_bind");
  set_socket_buffers(udp);
}

struct receiver_t
{
  uv_udp_t udp{};
  uv_timer_t quiet{};
  std::int64_t size = 0;
  std::vector<char> buffer;
  sequence_set_t seen;
  std::int64_t duplicates = 0;
  bool heard = false;
  std::uint64_t last_heard_ms = 0;
};

void receiver_alloc(uv_handle_t *handle, std::size_t, uv_buf_t *buf)
{
  auto *receiver = static_cast<receiver_t *>(handle->data);
  *buf = uv_buf_init(receiver->buffer.data(), unsigned(receiver->buffer.size()));
}

void receiver_read(uv_udp_t *udp, ssize_t nread, const uv_buf_t *buf, const sockaddr *, unsigned flags)
{
  if (nread == 0)
    return;
  if (nread < 0)
    check_uv(int(nread), "udp receive");
  auto *receiver = static_cast<receiver_t *>(udp->data);
  bench::check(nread == receiver->size && (flags & UV_UDP_PARTIAL) == 0, "datagram of the wrong size");
  std::uint64_t sequence = 0;
  std::memcpy(&sequence, buf->base, sizeof(sequence));
  if (!receiver->seen.insert(sequence))
    ++receiver->duplicates;
  receiver->heard = true;
  receiver->last_heard_ms = uv_now(udp->loop);
}

void receiver_tick(uv_timer_t *timer)
{
  auto *receiver = static_cast<receiver_t *>(timer->data);
  if (!receiver->heard || uv_now(timer->loop) - receiver->last_heard_ms < std::uint64_t(bench::udp_quiet_ns / 1'000'000))
    return;
  check_uv(uv_udp_recv_stop(&receiver->udp), "uv_udp_recv_stop");
  close_handle(&receiver->udp);
  close_handle(timer);
}

void run_receiver(receiver_t &receiver, std::int64_t batch, std::promise<int> &port)
{
  loop_t loop;
  unsigned flags = AF_INET | (batch > 0 ? unsigned(UV_UDP_RECVMMSG) : 0u);
  check_uv(uv_udp_init_ex(loop.get(), &receiver.udp, flags), "uv_udp_init_ex");
  receiver.udp.data = &receiver;
  bind_loopback(&receiver.udp);
  if (batch > 0 && uv_udp_using_recvmmsg(&receiver.udp) == 0)
  {
    close_handle(&receiver.udp);
    port.set_value(no_recvmmsg);
    loop.run_and_close();
    return;
  }
  receiver.buffer.resize(receive_slot * std::size_t(std::max<std::int64_t>(batch, 1)));
  check_uv(uv_udp_recv_start(&receiver.udp, receiver_alloc, receiver_read), "uv_udp_recv_start");
  check_uv(uv_timer_init(loop.get(), &receiver.quiet), "uv_timer_init");
  receiver.quiet.data = &receiver;
  check_uv(uv_timer_start(&receiver.quiet, receiver_tick, quiet_check_ms, quiet_check_ms), "uv_timer_start");
  sockaddr_storage bound{};
  int length = sizeof(bound);
  check_uv(uv_udp_getsockname(&receiver.udp, reinterpret_cast<sockaddr *>(&bound), &length), "uv_udp_getsockname");
  port.set_value(port_of(bound));
  loop.run_and_close();
}

struct sender_t
{
  sender_t(uv_loop_t *loop, double seconds)
    : window(loop, seconds)
  {
  }

  window_t window;
  uv_udp_t udp{};
  uv_udp_send_t send{};
  uv_idle_t idle{};
  sockaddr_in destination{};
  std::int64_t size = 0;
  std::int64_t batch = 0;
  std::vector<std::byte> datagrams;
  std::vector<uv_buf_t> buffers;
  std::vector<uv_buf_t *> buffer_list;
  std::vector<unsigned> buffer_counts;
  std::vector<sockaddr *> addresses;
  std::uint64_t next_sequence = 0;
  std::uint64_t first_measured = 0;
  std::uint64_t end_sequence = 0;
  bool send_in_flight = false;
};

void stamp(sender_t &sender, std::size_t index, std::uint64_t sequence)
{
  std::memcpy(sender.datagrams.data() + index * std::size_t(sender.size), &sequence, sizeof(sequence));
}

void close_sender(sender_t &sender)
{
  close_handle(&sender.udp);
  if (sender.batch > 0)
    close_handle(&sender.idle);
}

void sent_one(uv_udp_send_t *request, int status);

void send_one(sender_t &sender, std::size_t index)
{
  stamp(sender, index, sender.next_sequence++);
  sender.send_in_flight = true;
  check_uv(uv_udp_send(&sender.send, &sender.udp, &sender.buffers[index], 1, reinterpret_cast<const sockaddr *>(&sender.destination),
                       sent_one),
           "uv_udp_send");
}

void send_batch(uv_idle_t *idle)
{
  auto &sender = *static_cast<sender_t *>(idle->data);
  auto batch = std::size_t(sender.batch);
  for (std::size_t i = 0; i < batch; ++i)
    stamp(sender, i, sender.next_sequence + i);
  std::size_t done = 0;
  while (done < batch)
  {
    auto slice = std::min(batch - done, libuv_mmsg_width);
    int sent = uv_udp_try_send2(&sender.udp, unsigned(slice), sender.buffer_list.data() + done, sender.buffer_counts.data() + done,
                                sender.addresses.data() + done, 0);
    if (sent == UV_EAGAIN || sent == 0)
      break;
    check_uv(sent, "uv_udp_try_send2");
    done += std::size_t(sent);
    sender.next_sequence += std::uint64_t(sent);
    if (std::size_t(sent) < slice)
      break;
  }
  if (done == batch)
    return;
  check_uv(uv_idle_stop(idle), "uv_idle_stop");
  send_one(sender, done);
}

void sent_one(uv_udp_send_t *request, int status)
{
  auto &sender = *static_cast<sender_t *>(request->handle->data);
  sender.send_in_flight = false;
  check_uv(status, "udp send");
  if (sender.window.ended())
  {
    close_sender(sender);
    return;
  }
  if (sender.batch == 0)
    send_one(sender, 0);
  else
    check_uv(uv_idle_start(&sender.idle, send_batch), "uv_idle_start");
}

void window_closed(sender_t &sender)
{
  sender.end_sequence = sender.next_sequence;
  if (sender.batch > 0)
    check_uv(uv_idle_stop(&sender.idle), "uv_idle_stop");
  if (!sender.send_in_flight)
    close_sender(sender);
}
}

bench::result_t udp_flood(const bench::args_t &args)
{
  auto size = positive(args, "size");
  auto seconds = positive_seconds(args);
  auto batch = non_negative(args, "batch");
  bench::check(size >= std::int64_t(sizeof(std::uint64_t)) && size <= largest_datagram,
               "size must hold a 64-bit sequence number and fit in one datagram");

  receiver_t receiver;
  receiver.size = size;
  std::promise<int> port;
  auto port_ready = port.get_future();
  std::thread receiver_thread([&] { run_receiver(receiver, batch, port); });
  auto receiver_port = port_ready.get();
  if (receiver_port == no_recvmmsg)
  {
    receiver_thread.join();
    bench::unsupported("this libuv build cannot receive with recvmmsg");
  }

  loop_t loop;
  sender_t sender(loop.get(), seconds);
  sender.size = size;
  sender.batch = batch;
  check_uv(uv_udp_init_ex(loop.get(), &sender.udp, AF_INET), "uv_udp_init_ex");
  sender.udp.data = &sender;
  bind_loopback(&sender.udp);
  sender.destination = loopback(receiver_port);

  auto slots = std::size_t(std::max<std::int64_t>(batch, 1));
  auto pattern = bench::payload(std::size_t(size));
  sender.datagrams.reserve(slots * pattern.size());
  for (std::size_t i = 0; i < slots; ++i)
    sender.datagrams.insert(sender.datagrams.end(), pattern.begin(), pattern.end());
  for (std::size_t i = 0; i < slots; ++i)
    sender.buffers.push_back(uv_buf_init(reinterpret_cast<char *>(sender.datagrams.data() + i * pattern.size()), unsigned(size)));
  for (auto &buffer : sender.buffers)
  {
    sender.buffer_list.push_back(&buffer);
    sender.buffer_counts.push_back(1);
    sender.addresses.push_back(reinterpret_cast<sockaddr *>(&sender.destination));
  }

  if (batch > 0)
  {
    check_uv(uv_idle_init(loop.get(), &sender.idle), "uv_idle_init");
    sender.idle.data = &sender;
  }
  sender.window.start([&sender] { window_closed(sender); }, [&sender] { sender.first_measured = sender.next_sequence; });
  if (batch > 0)
    check_uv(uv_idle_start(&sender.idle, send_batch), "uv_idle_start");
  else
    send_one(sender, 0);
  loop.run_and_close();
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
  return result;
}
}
