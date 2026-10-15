#include "support.h"

#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>

#include <algorithm>
#include <future>
#include <span>
#include <thread>
#include <vector>

namespace vio_bench
{
namespace
{
struct tcp_listener_t
{
  vio::tcp_server_t server;
  std::uint16_t port = 0;
};

tcp_listener_t listen_loopback(vio::event_loop_t &loop)
{
  auto server = vio::tcp_create_server(loop);
  require(server, "tcp_create_server");
  auto address = loopback(0);
  require(vio::tcp_bind(*server, as_sockaddr(address)), "tcp_bind");
  auto name = vio::sockname(server->tcp);
  require(name, "sockname");
  return {std::move(*server), port_of(*name)};
}

vio::tcp_t accepted_socket(vio::tcp_server_t &server)
{
  auto accepted = vio::tcp_accept(server);
  require(accepted, "tcp_accept");
  set_nodelay(accepted->handle->socket.native());
  return std::move(*accepted);
}

vio::task_t<void> echo(vio::tcp_t socket)
{
  auto reader = vio::tcp_create_reader(socket);
  require(reader, "tcp_create_reader");
  std::vector<std::byte> buffer(bench::server_read_buffer);
  for (;;)
  {
    auto got = co_await reader->read_into(std::span<std::byte>(buffer));
    if (!got.has_value() || *got == 0)
      break;
    auto wrote = co_await vio::write_tcp(socket, std::span<const std::byte>(buffer.data(), *got));
    if (!wrote.has_value())
      break;
  }
}

vio::task_t<void> rr_server(vio::tcp_server_t server, std::int64_t connections, std::function<void()> listening)
{
  std::vector<vio::task_t<void>> echoes;
  echoes.reserve(std::size_t(connections));
  for (std::int64_t i = 0; i < connections; ++i)
  {
    auto incoming = vio::tcp_listen(server, listen_backlog);
    if (i == 0)
      listening();
    require(co_await incoming, "tcp_listen");
    echoes.push_back(echo(accepted_socket(server)));
  }
  for (auto &task : echoes)
    co_await std::move(task);
}

struct rr_shared_t
{
  std::int64_t size = 0;
  std::int64_t depth = 0;
  window_t window;
  std::vector<std::byte> request;
  bench::latencies_t latencies;
  std::int64_t requests = 0;
  std::int64_t bytes = 0;
};

vio::task_t<void> rr_connection(vio::tcp_t socket, rr_shared_t &shared)
{
  auto reader = vio::tcp_create_reader(socket);
  require(reader, "tcp_create_reader");
  std::vector<std::byte> buffer(bench::server_read_buffer);
  const auto depth = std::size_t(shared.depth);
  const auto request = std::span<const std::byte>(shared.request);
  std::vector<std::int64_t> sent_at(depth);
  std::size_t oldest = 0;
  std::size_t outstanding = 0;
  std::int64_t partial = 0;

  for (std::size_t i = 0; i < depth; ++i)
  {
    sent_at[(oldest + outstanding) % depth] = bench::now_ns();
    ++outstanding;
    require(co_await vio::write_tcp(socket, request), "write_tcp");
  }

  while (outstanding > 0)
  {
    auto got = co_await reader->read_into(std::span<std::byte>(buffer));
    require(got, "read_into");
    bench::check(*got > 0, "server closed a connection early");
    const auto arrived = bench::now_ns();
    const bool counted = shared.window.contains(arrived);
    if (counted)
      shared.bytes += std::int64_t(*got);
    partial += std::int64_t(*got);
    while (partial >= shared.size && outstanding > 0)
    {
      partial -= shared.size;
      if (counted)
      {
        ++shared.requests;
        shared.latencies.add(arrived - sent_at[oldest]);
      }
      oldest = (oldest + 1) % depth;
      --outstanding;
      if (arrived < shared.window.end)
      {
        sent_at[(oldest + outstanding) % depth] = bench::now_ns();
        ++outstanding;
        require(co_await vio::write_tcp(socket, request), "write_tcp");
      }
    }
  }
}

struct rr_result_t
{
  window_t window;
  cpu_window_t cpu;
  bench::latencies_t latencies;
  std::int64_t requests = 0;
  std::int64_t bytes = 0;
};

vio::task_t<void> rr_client(vio::event_loop_t &loop, std::uint16_t port, std::int64_t connections, std::int64_t size, std::int64_t depth, double seconds, rr_result_t &result)
{
  const auto address = loopback(port);
  std::vector<vio::tcp_t> sockets;
  sockets.reserve(std::size_t(connections));
  for (std::int64_t i = 0; i < connections; ++i)
  {
    auto socket = vio::tcp_create(loop);
    require(socket, "tcp_create");
    require(co_await vio::tcp_connect(*socket, as_sockaddr(address)), "tcp_connect");
    set_nodelay(socket->handle->socket.native());
    sockets.push_back(std::move(*socket));
  }

  rr_shared_t shared;
  shared.size = size;
  shared.depth = depth;
  shared.window = window_t::starting(bench::now_ns(), seconds);
  shared.request = bench::payload(std::size_t(size));

  auto meter = meter_cpu(loop, shared.window, result.cpu);
  std::vector<vio::task_t<void>> workers;
  workers.reserve(sockets.size());
  for (auto &socket : sockets)
    workers.push_back(rr_connection(std::move(socket), shared));
  sockets.clear();
  for (auto &worker : workers)
    co_await std::move(worker);
  co_await std::move(meter);

  result.window = shared.window;
  result.latencies = std::move(shared.latencies);
  result.requests = shared.requests;
  result.bytes = shared.bytes;
}

struct bulk_server_result_t
{
  std::int64_t received = 0;
  std::int64_t eof_at = 0;
  double cpu_at_eof = 0.0;
};

struct bulk_client_result_t
{
  std::int64_t first_write_at = 0;
  double cpu_at_first_write = 0.0;
};

vio::task_t<void> bulk_server(vio::tcp_server_t server, std::function<void()> listening, bulk_server_result_t &result)
{
  auto incoming = vio::tcp_listen(server, listen_backlog);
  listening();
  require(co_await incoming, "tcp_listen");
  auto socket = accepted_socket(server);
  auto reader = vio::tcp_create_reader(socket);
  require(reader, "tcp_create_reader");
  std::vector<std::byte> buffer(bench::server_read_buffer);
  for (;;)
  {
    auto got = co_await reader->read_into(std::span<std::byte>(buffer));
    require(got, "read_into");
    if (*got == 0)
      break;
    result.received += std::int64_t(*got);
  }
  result.eof_at = bench::now_ns();
  result.cpu_at_eof = bench::process_cpu_s();
}

vio::task_t<void> bulk_client(vio::event_loop_t &loop, std::uint16_t port, std::int64_t bytes, std::int64_t chunk, bulk_client_result_t &result)
{
  const auto address = loopback(port);
  auto socket = vio::tcp_create(loop);
  require(socket, "tcp_create");
  require(co_await vio::tcp_connect(*socket, as_sockaddr(address)), "tcp_connect");
  set_nodelay(socket->handle->socket.native());
  const auto payload = bench::payload(std::size_t(chunk));

  result.cpu_at_first_write = bench::process_cpu_s();
  result.first_write_at = bench::now_ns();
  for (std::int64_t sent = 0; sent < bytes;)
  {
    const auto length = std::min(chunk, bytes - sent);
    require(co_await vio::write_tcp(*socket, std::span<const std::byte>(payload.data(), std::size_t(length))), "write_tcp");
    sent += length;
  }
}
}

bench::result_t tcp_rr(const bench::args_t &args)
{
  const auto connections = positive(args, "connections");
  const auto size = positive(args, "size");
  const auto depth = positive(args, "depth");
  const auto seconds = positive_real(args, "seconds");

  std::promise<std::uint16_t> port_promise;
  auto port_future = port_promise.get_future();
  std::thread server_thread([&] {
    run_loop([&](vio::event_loop_t &loop) {
      auto listener = listen_loopback(loop);
      return rr_server(std::move(listener.server), connections, [&port_promise, port = listener.port] { port_promise.set_value(port); });
    });
  });

  rr_result_t result;
  std::string client_backend;
  std::thread client_thread([&] {
    const auto port = port_future.get();
    client_backend = run_loop([&](vio::event_loop_t &loop) { return rr_client(loop, port, connections, size, depth, seconds, result); });
  });
  client_thread.join();
  server_thread.join();

  bench::result_t out;
  out.metrics["ops_per_s"] = double(result.requests) / result.window.measured_s();
  out.metrics["p50_us"] = result.latencies.percentile_us(50);
  out.metrics["p99_us"] = result.latencies.percentile_us(99);
  out.metrics["cpu_s"] = result.cpu.used();
  out.work["requests"] = result.requests;
  out.work["bytes"] = result.bytes;
  out.work["backend"] = client_backend;
  return out;
}

bench::result_t tcp_bulk(const bench::args_t &args)
{
  const auto bytes = positive(args, "bytes");
  const auto chunk = positive(args, "chunk");

  std::promise<std::uint16_t> port_promise;
  auto port_future = port_promise.get_future();
  bulk_server_result_t server_result;
  std::thread server_thread([&] {
    run_loop([&](vio::event_loop_t &loop) {
      auto listener = listen_loopback(loop);
      return bulk_server(std::move(listener.server), [&port_promise, port = listener.port] { port_promise.set_value(port); }, server_result);
    });
  });

  bulk_client_result_t client_result;
  std::string client_backend;
  std::thread client_thread([&] {
    const auto port = port_future.get();
    client_backend = run_loop([&](vio::event_loop_t &loop) { return bulk_client(loop, port, bytes, chunk, client_result); });
  });
  client_thread.join();
  server_thread.join();

  const auto elapsed = server_result.eof_at - client_result.first_write_at;
  bench::result_t out;
  out.metrics["mib_per_s"] = mib_per_second(server_result.received, elapsed);
  out.metrics["cpu_s"] = server_result.cpu_at_eof - client_result.cpu_at_first_write;
  out.work["bytes"] = server_result.received;
  out.work["backend"] = client_backend;
  return out;
}
}
