#include "asio_bench.h"

#include <asio/write.hpp>

#include <algorithm>
#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace asio_bench
{
namespace
{
using asio::ip::tcp;

[[nodiscard]] bool peer_gone(const std::error_code &error)
{
  return error == asio::error::eof || error == asio::error::connection_reset || error == asio::error::broken_pipe;
}

asio::awaitable<void> echo(tcp::socket socket)
{
  std::vector<char> buffer(bench::server_read_buffer);
  for (;;)
  {
    auto [read_error, length] = co_await socket.async_read_some(asio::buffer(buffer), with_error);
    if (read_error)
    {
      if (peer_gone(read_error))
        co_return;
      fail("echo read", read_error);
    }
    auto [write_error, written] = co_await asio::async_write(socket, asio::buffer(buffer.data(), length), with_error);
    if (write_error)
    {
      if (peer_gone(write_error))
        co_return;
      fail("echo write", write_error);
    }
  }
}

asio::awaitable<void> echo_server(tcp::acceptor acceptor, std::int64_t connections)
{
  auto executor = co_await asio::this_coro::executor;
  for (std::int64_t i = 0; i < connections; ++i)
  {
    auto socket = co_await acceptor.async_accept(asio::deferred);
    socket.set_option(tcp::no_delay(true));
    spawn(executor, echo(std::move(socket)), "tcp_rr echo");
  }
}

void run_echo_server(std::int64_t connections, std::promise<unsigned short> &port)
{
  asio::io_context context(1);
  auto acceptor = listen_loopback(context);
  port.set_value(acceptor.local_endpoint().port());
  spawn(context.get_executor(), echo_server(std::move(acceptor), connections), "tcp_rr server");
  context.run();
}

struct rr_connection_t
{
  explicit rr_connection_t(asio::io_context &context)
    : socket(context)
  {
  }

  tcp::socket socket;
  std::deque<std::int64_t> sent_at;
  std::int64_t partial = 0;
};

struct rr_client_t
{
  explicit rr_client_t(double seconds)
    : window(seconds)
  {
  }

  window_t window;
  std::vector<std::byte> request;
  std::int64_t size = 0;
  std::int64_t depth = 0;
  std::vector<std::unique_ptr<rr_connection_t>> connections;
  std::int64_t requests = 0;
  std::int64_t bytes = 0;
  bench::latencies_t latencies;
};

asio::awaitable<void> rr_connection(rr_client_t &client, rr_connection_t &connection)
{
  std::vector<char> buffer(bench::server_read_buffer);
  auto owed = client.depth;
  while (!client.window.ended())
  {
    for (; owed > 0; --owed)
    {
      connection.sent_at.push_back(bench::now_ns());
      auto [error, written] = co_await asio::async_write(connection.socket, asio::buffer(client.request), with_error);
      if (error)
      {
        if (client.window.ended())
          co_return;
        fail("request write", error);
      }
    }
    auto [error, length] = co_await connection.socket.async_read_some(asio::buffer(buffer), with_error);
    if (error)
    {
      if (client.window.ended())
        co_return;
      fail("response read", error);
    }
    bool measuring = client.window.measuring();
    if (measuring)
      client.bytes += std::int64_t(length);
    connection.partial += std::int64_t(length);
    if (connection.partial < client.size)
      continue;
    auto now = bench::now_ns();
    while (connection.partial >= client.size)
    {
      connection.partial -= client.size;
      bench::check(!connection.sent_at.empty(), "response without a request");
      auto sent = connection.sent_at.front();
      connection.sent_at.pop_front();
      if (measuring)
      {
        ++client.requests;
        client.latencies.add(now - sent);
      }
      ++owed;
    }
  }
}

void close_all(rr_client_t &client)
{
  for (auto &connection : client.connections)
  {
    std::error_code error;
    connection->socket.close(error);
    if (error)
      fail("close", error);
  }
}

asio::awaitable<void> rr_client(rr_client_t &client, tcp::endpoint server)
{
  auto executor = co_await asio::this_coro::executor;
  for (auto &connection : client.connections)
  {
    co_await connection->socket.async_connect(server, asio::deferred);
    connection->socket.set_option(tcp::no_delay(true));
  }
  spawn(executor, client.window.run({}, [&client] { close_all(client); }), "tcp_rr window");
  for (auto &connection : client.connections)
    spawn(executor, rr_connection(client, *connection), "tcp_rr connection");
}

struct sink_result_t
{
  std::int64_t received = 0;
  std::int64_t end_ns = 0;
  double end_cpu = 0.0;
};

asio::awaitable<void> sink(tcp::acceptor acceptor, sink_result_t &result)
{
  auto socket = co_await acceptor.async_accept(asio::deferred);
  acceptor.close();
  socket.set_option(tcp::no_delay(true));
  std::vector<char> buffer(bench::server_read_buffer);
  for (;;)
  {
    auto [error, length] = co_await socket.async_read_some(asio::buffer(buffer), with_error);
    if (error)
    {
      if (error != asio::error::eof)
        fail("bulk read", error);
      break;
    }
    result.received += std::int64_t(length);
  }
  result.end_ns = bench::now_ns();
  result.end_cpu = bench::process_cpu_s();
}

void run_sink_server(sink_result_t &result, std::promise<unsigned short> &port)
{
  asio::io_context context(1);
  auto acceptor = listen_loopback(context);
  port.set_value(acceptor.local_endpoint().port());
  spawn(context.get_executor(), sink(std::move(acceptor), result), "tcp_bulk server");
  context.run();
}

struct source_result_t
{
  std::int64_t start_ns = 0;
  double start_cpu = 0.0;
};

asio::awaitable<void> source(tcp::endpoint server, std::int64_t bytes, std::int64_t chunk, source_result_t &result)
{
  tcp::socket socket(co_await asio::this_coro::executor);
  co_await socket.async_connect(server, asio::deferred);
  socket.set_option(tcp::no_delay(true));
  auto payload = bench::payload(std::size_t(chunk));
  result.start_ns = bench::now_ns();
  result.start_cpu = bench::process_cpu_s();
  for (std::int64_t sent = 0; sent < bytes;)
  {
    auto length = std::min(chunk, bytes - sent);
    co_await asio::async_write(socket, asio::buffer(payload.data(), std::size_t(length)), asio::deferred);
    sent += length;
  }
  socket.shutdown(tcp::socket::shutdown_send);
}
}

bench::result_t tcp_rr(const bench::args_t &args)
{
  auto connections = positive(args, "connections");
  auto size = positive(args, "size");
  auto depth = positive(args, "depth");
  auto seconds = positive_seconds(args);

  std::promise<unsigned short> port;
  auto port_ready = port.get_future();
  std::thread server([&] { run_echo_server(connections, port); });

  asio::io_context context(1);
  rr_client_t client(seconds);
  client.request = bench::payload(std::size_t(size));
  client.size = size;
  client.depth = depth;
  for (std::int64_t i = 0; i < connections; ++i)
    client.connections.push_back(std::make_unique<rr_connection_t>(context));
  spawn(context.get_executor(), rr_client(client, tcp_loopback(port_ready.get())), "tcp_rr client");
  context.run();
  server.join();

  bench::result_t result;
  result.metrics["ops_per_s"] = double(client.requests) / client.window.elapsed_s();
  result.metrics["p50_us"] = client.latencies.percentile_us(50);
  result.metrics["p99_us"] = client.latencies.percentile_us(99);
  result.metrics["cpu_s"] = client.window.cpu_s();
  result.work["requests"] = client.requests;
  result.work["bytes"] = client.bytes;
  result.work["backend"] = reactor_backend();
  return result;
}

bench::result_t tcp_bulk(const bench::args_t &args)
{
  auto bytes = positive(args, "bytes");
  auto chunk = positive(args, "chunk");

  sink_result_t server_result;
  std::promise<unsigned short> port;
  auto port_ready = port.get_future();
  std::thread server([&] { run_sink_server(server_result, port); });

  asio::io_context context(1);
  source_result_t client_result;
  spawn(context.get_executor(), source(tcp_loopback(port_ready.get()), bytes, chunk, client_result), "tcp_bulk client");
  context.run();
  server.join();

  bench::result_t result;
  result.metrics["mib_per_s"] = mib_per_s(server_result.received, server_result.end_ns - client_result.start_ns);
  result.metrics["cpu_s"] = server_result.end_cpu - client_result.start_cpu;
  result.work["bytes"] = server_result.received;
  result.work["backend"] = reactor_backend();
  return result;
}
}
