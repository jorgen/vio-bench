#include "uv_bench.h"

#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace uvb
{
namespace
{
struct echo_connection_t
{
  uv_tcp_t tcp{};
  uv_write_t write{};
  std::vector<char> buffer = std::vector<char>(bench::server_read_buffer);
};

struct echo_server_t
{
  uv_tcp_t listener{};
  std::int64_t expected = 0;
  std::int64_t accepted = 0;
  std::vector<std::unique_ptr<echo_connection_t>> connections;
};

void echo_alloc(uv_handle_t *handle, std::size_t, uv_buf_t *buf)
{
  auto *connection = static_cast<echo_connection_t *>(handle->data);
  *buf = uv_buf_init(connection->buffer.data(), unsigned(connection->buffer.size()));
}

void echo_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

void echo_written(uv_write_t *request, int status)
{
  auto *stream = request->handle;
  if (status < 0)
  {
    close_handle(stream);
    return;
  }
  if (!uv_is_closing(as_handle(stream)))
    check_uv(uv_read_start(stream, echo_alloc, echo_read), "uv_read_start");
}

void echo_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *)
{
  if (nread == 0)
    return;
  if (nread < 0)
  {
    close_handle(stream);
    return;
  }
  auto *connection = static_cast<echo_connection_t *>(stream->data);
  auto echo = uv_buf_init(connection->buffer.data(), unsigned(nread));
  int written = uv_try_write(stream, &echo, 1);
  if (written == nread)
    return;
  if (written < 0 && written != UV_EAGAIN)
  {
    close_handle(stream);
    return;
  }
  auto echoed = std::max(written, 0);
  uv_read_stop(stream);
  auto rest = uv_buf_init(connection->buffer.data() + echoed, unsigned(nread - echoed));
  if (uv_write(&connection->write, stream, &rest, 1, echo_written) < 0)
    close_handle(stream);
}

void echo_accept(uv_stream_t *listener, int status)
{
  check_uv(status, "connection");
  auto *server = static_cast<echo_server_t *>(listener->data);
  auto &connection = *server->connections.emplace_back(std::make_unique<echo_connection_t>());
  accept_into(listener, &connection.tcp);
  connection.tcp.data = &connection;
  check_uv(uv_read_start(as_stream(&connection.tcp), echo_alloc, echo_read), "uv_read_start");
  if (++server->accepted == server->expected)
    close_handle(&server->listener);
}

void run_echo_server(std::int64_t connections, std::promise<int> &port)
{
  loop_t loop;
  echo_server_t server;
  server.expected = connections;
  check_uv(uv_tcp_init(loop.get(), &server.listener), "uv_tcp_init");
  server.listener.data = &server;
  port.set_value(listen_on_loopback(&server.listener, echo_accept));
  loop.run_and_close();
}

struct rr_connection_t;

struct rr_write_t
{
  uv_write_t request{};
  rr_connection_t *connection = nullptr;
};

struct rr_client_t
{
  rr_client_t(uv_loop_t *loop, double seconds)
    : window(loop, seconds)
  {
  }

  window_t window;
  std::vector<std::byte> request;
  std::int64_t size = 0;
  std::int64_t depth = 0;
  std::vector<std::unique_ptr<rr_connection_t>> connections;
  std::size_t connected = 0;
  std::int64_t requests = 0;
  std::int64_t bytes = 0;
  bench::latencies_t latencies;
};

struct rr_connection_t
{
  uv_tcp_t tcp{};
  uv_connect_t connect{};
  rr_client_t *client = nullptr;
  std::vector<char> buffer = std::vector<char>(bench::server_read_buffer);
  std::deque<std::int64_t> sent_at;
  std::int64_t partial = 0;
  std::vector<std::unique_ptr<rr_write_t>> writes;
  std::vector<rr_write_t *> idle_writes;
};

void rr_written(uv_write_t *request, int status)
{
  auto *write = static_cast<rr_write_t *>(request->data);
  write->connection->idle_writes.push_back(write);
  if (status < 0 && !write->connection->client->window.ended())
    check_uv(status, "request write");
}

void send_request(rr_connection_t &connection)
{
  rr_write_t *write = nullptr;
  if (connection.idle_writes.empty())
  {
    write = connection.writes.emplace_back(std::make_unique<rr_write_t>()).get();
    write->connection = &connection;
    write->request.data = write;
  }
  else
  {
    write = connection.idle_writes.back();
    connection.idle_writes.pop_back();
  }
  auto &request = connection.client->request;
  auto buf = uv_buf_init(reinterpret_cast<char *>(request.data()), unsigned(request.size()));
  connection.sent_at.push_back(bench::now_ns());
  check_uv(uv_write(&write->request, as_stream(&connection.tcp), &buf, 1, rr_written), "uv_write");
}

void rr_alloc(uv_handle_t *handle, std::size_t, uv_buf_t *buf)
{
  auto *connection = static_cast<rr_connection_t *>(handle->data);
  *buf = uv_buf_init(connection->buffer.data(), unsigned(connection->buffer.size()));
}

void rr_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *)
{
  auto &connection = *static_cast<rr_connection_t *>(stream->data);
  auto &client = *connection.client;
  if (nread == 0)
    return;
  if (nread < 0)
  {
    if (!client.window.ended())
      check_uv(int(nread), "response read");
    return;
  }
  bool measuring = client.window.measuring();
  if (measuring)
    client.bytes += nread;
  connection.partial += nread;
  if (connection.partial < client.size)
    return;
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
    if (!client.window.ended())
      send_request(connection);
  }
}

void rr_close_all(rr_client_t &client)
{
  for (auto &connection : client.connections)
    close_handle(&connection->tcp);
}

void rr_connected(uv_connect_t *request, int status)
{
  check_uv(status, "connect");
  auto &connection = *static_cast<rr_connection_t *>(request->data);
  auto &client = *connection.client;
  check_uv(uv_read_start(as_stream(&connection.tcp), rr_alloc, rr_read), "uv_read_start");
  if (++client.connected < client.connections.size())
    return;
  client.window.start([&client] { rr_close_all(client); });
  for (auto &each : client.connections)
    for (std::int64_t i = 0; i < client.depth; ++i)
      send_request(*each);
}

struct sink_server_t
{
  uv_tcp_t listener{};
  uv_tcp_t connection{};
  std::vector<char> buffer = std::vector<char>(bench::server_read_buffer);
  std::int64_t received = 0;
  std::int64_t end_ns = 0;
  double end_cpu = 0.0;
};

void sink_alloc(uv_handle_t *handle, std::size_t, uv_buf_t *buf)
{
  auto *server = static_cast<sink_server_t *>(handle->data);
  *buf = uv_buf_init(server->buffer.data(), unsigned(server->buffer.size()));
}

void sink_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *)
{
  auto *server = static_cast<sink_server_t *>(stream->data);
  if (nread > 0)
  {
    server->received += nread;
    return;
  }
  if (nread == 0)
    return;
  if (nread != UV_EOF)
    check_uv(int(nread), "bulk read");
  server->end_ns = bench::now_ns();
  server->end_cpu = bench::process_cpu_s();
  close_handle(stream);
}

void sink_accept(uv_stream_t *listener, int status)
{
  check_uv(status, "connection");
  auto *server = static_cast<sink_server_t *>(listener->data);
  accept_into(listener, &server->connection);
  server->connection.data = server;
  check_uv(uv_read_start(as_stream(&server->connection), sink_alloc, sink_read), "uv_read_start");
  close_handle(&server->listener);
}

void run_sink_server(sink_server_t &server, std::promise<int> &port)
{
  loop_t loop;
  check_uv(uv_tcp_init(loop.get(), &server.listener), "uv_tcp_init");
  server.listener.data = &server;
  port.set_value(listen_on_loopback(&server.listener, sink_accept));
  loop.run_and_close();
}

struct bulk_client_t
{
  uv_tcp_t tcp{};
  uv_connect_t connect{};
  uv_write_t write{};
  uv_shutdown_t shutdown{};
  std::vector<std::byte> chunk;
  std::int64_t total = 0;
  std::int64_t sent = 0;
  std::int64_t start_ns = 0;
  double start_cpu = 0.0;
};

void bulk_written(uv_write_t *request, int status);

void bulk_write_next(bulk_client_t &client)
{
  auto length = std::min<std::int64_t>(std::int64_t(client.chunk.size()), client.total - client.sent);
  auto buf = uv_buf_init(reinterpret_cast<char *>(client.chunk.data()), unsigned(length));
  client.sent += length;
  check_uv(uv_write(&client.write, as_stream(&client.tcp), &buf, 1, bulk_written), "uv_write");
}

void bulk_shut_down(uv_shutdown_t *request, int status)
{
  check_uv(status, "shutdown");
  close_handle(request->handle);
}

void bulk_written(uv_write_t *request, int status)
{
  check_uv(status, "bulk write");
  auto &client = *static_cast<bulk_client_t *>(request->data);
  if (client.sent < client.total)
    bulk_write_next(client);
  else
    check_uv(uv_shutdown(&client.shutdown, as_stream(&client.tcp), bulk_shut_down), "uv_shutdown");
}

void bulk_connected(uv_connect_t *request, int status)
{
  check_uv(status, "connect");
  auto &client = *static_cast<bulk_client_t *>(request->data);
  client.start_ns = bench::now_ns();
  client.start_cpu = bench::process_cpu_s();
  bulk_write_next(client);
}
}

bench::result_t tcp_rr(const bench::args_t &args)
{
  auto connections = positive(args, "connections");
  auto size = positive(args, "size");
  auto depth = positive(args, "depth");
  auto seconds = positive_seconds(args);

  std::promise<int> port;
  auto port_ready = port.get_future();
  std::thread server([&] { run_echo_server(connections, port); });

  loop_t loop;
  rr_client_t client(loop.get(), seconds);
  client.request = bench::payload(std::size_t(size));
  client.size = size;
  client.depth = depth;
  auto address = loopback(port_ready.get());
  for (std::int64_t i = 0; i < connections; ++i)
  {
    auto &connection = *client.connections.emplace_back(std::make_unique<rr_connection_t>());
    connection.client = &client;
    connection.tcp.data = &connection;
    connection.connect.data = &connection;
    connect_to(loop.get(), &connection.tcp, &connection.connect, address, rr_connected);
  }
  loop.run_and_close();
  server.join();

  bench::result_t result;
  result.metrics["ops_per_s"] = double(client.requests) / client.window.elapsed_s();
  result.metrics["p50_us"] = client.latencies.percentile_us(50);
  result.metrics["p99_us"] = client.latencies.percentile_us(99);
  result.metrics["cpu_s"] = client.window.cpu_s();
  result.work["requests"] = client.requests;
  result.work["bytes"] = client.bytes;
  return result;
}

bench::result_t tcp_bulk(const bench::args_t &args)
{
  auto bytes = positive(args, "bytes");
  auto chunk = positive(args, "chunk");

  sink_server_t server;
  std::promise<int> port;
  auto port_ready = port.get_future();
  std::thread server_thread([&] { run_sink_server(server, port); });

  loop_t loop;
  bulk_client_t client;
  client.chunk = bench::payload(std::size_t(chunk));
  client.total = bytes;
  client.connect.data = &client;
  client.write.data = &client;
  connect_to(loop.get(), &client.tcp, &client.connect, loopback(port_ready.get()), bulk_connected);
  loop.run_and_close();
  server_thread.join();

  bench::result_t result;
  result.metrics["mib_per_s"] = mib_per_s(server.received, server.end_ns - client.start_ns);
  result.metrics["cpu_s"] = server.end_cpu - client.start_cpu;
  result.work["bytes"] = server.received;
  return result;
}
}
