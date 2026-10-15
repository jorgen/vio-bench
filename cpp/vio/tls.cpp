#include <vio/ssl_backend.h>

#include <openssl/crypto.h>
#include <openssl/ssl.h>

#include "support.h"

#include <vio/buffer_pool.h>
#include <vio/cancellation.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/operation/tls_client.h>
#include <vio/operation/tls_server.h>

#include <algorithm>
#include <fstream>
#include <future>
#include <iterator>
#include <span>
#include <thread>
#include <vector>

namespace vio_bench
{
namespace
{
constexpr std::string_view server_name = "localhost";
constexpr std::string_view server_ip = "127.0.0.1";

std::vector<std::uint8_t> read_file_bytes(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  bench::check(in.good(), std::format("cannot read {}", path));
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void pin_tls13(vio::ssl_config_t &config)
{
  config.min_protocol = vio::tls_protocol_version::tls1_3;
  config.max_protocol = vio::tls_protocol_version::tls1_3;
  config.groups = "X25519";
  config.ciphersuites = "TLS_AES_128_GCM_SHA256";
  config.enable_session_cache = false;
}

vio::ssl_config_t server_config()
{
  vio::ssl_config_t config;
  config.cert_file = bench::server_cert_path();
  config.key_file = bench::server_key_path();
  config.peer_verify = vio::peer_verify_t::disabled;
  pin_tls13(config);
  return config;
}

vio::ssl_config_t client_config()
{
  vio::ssl_config_t config;
  config.ca_mem = read_file_bytes(bench::ca_cert_path());
  config.peer_verify = vio::peer_verify_t::required;
  pin_tls13(config);
  return config;
}

void disable_server_resumption(SSL_CTX *ctx)
{
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
  bench::check(SSL_CTX_set_num_tickets(ctx, 0) == 1, "SSL_CTX_set_num_tickets");
  SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
}

void disable_client_session_cache(SSL_CTX *ctx)
{
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
}

struct negotiated_t
{
  std::string cipher = "unknown";
  std::string group = "unknown";
};

negotiated_t negotiated(const vio::ssl_client_t &client)
{
  negotiated_t out;
  SSL *ssl = client.state->engine.ssl;
  if (ssl == nullptr)
    return out;
  if (const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl))
    out.cipher = SSL_CIPHER_standard_name(cipher);
  if (const char *group = SSL_get_group_name(SSL_get_group_id(ssl)))
    out.group = group;
  return out;
}

std::string tls_library()
{
  return std::format("{} {}", OpenSSL_version(OPENSSL_VERSION), VIO_BENCH_TLS_VERSION);
}

bool end_of_stream(const vio::error_t &error)
{
  return error.code == vio::vio_tls_clean_shutdown || error.code == vio::vio_tls_truncated || error.code == vio::errc::eof;
}

struct tls_listener_t
{
  vio::ssl_server_t server;
  std::uint16_t port = 0;
};

tls_listener_t listen_loopback(vio::event_loop_t &loop, vio::buffer_pool_t &read_pool)
{
  auto tcp_server = vio::tcp_create_server(loop);
  require(tcp_server, "tcp_create_server");
  auto address = loopback(0);
  require(vio::tcp_bind(*tcp_server, as_sockaddr(address)), "tcp_bind");
  auto name = vio::sockname(tcp_server->tcp);
  require(name, "sockname");
  const auto port = port_of(*name);
  auto server = vio::ssl_server_create(loop, std::move(*tcp_server), std::string(server_name), server_config(), &vio::buffer_pool_t::alloc_cb,
                                       &vio::buffer_pool_t::dealloc_cb, &read_pool);
  require(server, "ssl_server_create");
  disable_server_resumption(server->handle->ssl_ctx.ctx);
  return {std::move(*server), port};
}

vio::ssl_server_client_t accepted_client(vio::ssl_server_t &server)
{
  auto client = vio::ssl_server_accept(server);
  require(client, "ssl_server_accept");
  set_nodelay(client->handle->tcp.handle->socket.native());
  return std::move(*client);
}

vio::ssl_client_t create_client(vio::event_loop_t &loop, const vio::ssl_config_t &config)
{
  auto client = vio::ssl_client_create(loop, config);
  require(client, "ssl_client_create");
  disable_client_session_cache(client->state->ssl_ctx.ctx);
  return std::move(*client);
}

vio::ssl_client_connecting_future_t connect_client(vio::ssl_client_t &client, std::uint16_t port)
{
  auto connecting = vio::ssl_client_connect(client, std::string(server_name), port, std::string(server_ip));
  const bool already_failed = !client.state->connecting && !client.state->connected;
  if (already_failed)
    return connecting;
  auto &tcp = client.state->tcp;
  bench::check(tcp.handle && tcp.handle->socket.native() != vio::invalid_native_socket, "TLS client socket not open after ssl_client_connect; cannot set TCP_NODELAY");
  set_nodelay(tcp.handle->socket.native());
  return connecting;
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
  negotiated_t negotiated;
};

vio::task_t<void> bulk_server(vio::ssl_server_t server, std::function<void()> listening, bulk_server_result_t &result)
{
  auto incoming = vio::ssl_server_listen(server, listen_backlog);
  listening();
  require(co_await incoming, "ssl_server_listen");
  auto client = accepted_client(server);
  require(co_await vio::ssl_server_client_handshake(client), "server handshake");
  auto reader = vio::ssl_server_client_create_reader(client);
  require(reader, "ssl_server_client_create_reader");
  for (;;)
  {
    auto chunk = co_await *reader;
    if (!chunk.has_value())
    {
      if (end_of_stream(chunk.error()))
        break;
      fail("tls read", chunk.error());
    }
    result.received += std::int64_t(chunk->size());
  }
  result.eof_at = bench::now_ns();
  result.cpu_at_eof = bench::process_cpu_s();
}

vio::task_t<void> bulk_client(vio::event_loop_t &loop, std::uint16_t port, std::int64_t bytes, std::int64_t chunk, bulk_client_result_t &result)
{
  auto client = create_client(loop, client_config());
  require(co_await connect_client(client, port), "ssl_client_connect");
  result.negotiated = negotiated(client);
  const auto payload = bench::payload(std::size_t(chunk));

  result.cpu_at_first_write = bench::process_cpu_s();
  result.first_write_at = bench::now_ns();
  for (std::int64_t sent = 0; sent < bytes;)
  {
    const auto length = std::min(chunk, bytes - sent);
    require(co_await vio::ssl_client_write(client, std::span<const std::byte>(payload.data(), std::size_t(length))), "ssl_client_write");
    sent += length;
  }
  require(co_await vio::ssl_client_shutdown(client), "ssl_client_shutdown");
}

struct handshake_server_result_t
{
  std::int64_t handshakes = 0;
  std::int64_t failures = 0;
  std::string last_failure;
};

struct handshake_client_result_t
{
  window_t window;
  cpu_window_t cpu;
  bench::latencies_t latencies;
  std::int64_t handshakes = 0;
  negotiated_t negotiated;
};

vio::task_t<void> handshake_server(vio::ssl_server_t server, vio::cancellation_t &stop, std::function<void()> listening, handshake_server_result_t &result)
{
  for (bool announced = false;; announced = true)
  {
    auto incoming = vio::ssl_server_listen(server, listen_backlog, &stop);
    if (!announced)
      listening();
    auto listened = co_await incoming;
    if (!listened.has_value())
    {
      if (vio::is_cancelled(listened.error()))
        break;
      fail("ssl_server_listen", listened.error());
    }
    auto client = accepted_client(server);
    auto shaken = co_await vio::ssl_server_client_handshake(client);
    if (!shaken.has_value())
    {
      ++result.failures;
      result.last_failure = shaken.error().msg;
      continue;
    }
    ++result.handshakes;
    auto reader = vio::ssl_server_client_create_reader(client);
    require(reader, "ssl_server_client_create_reader");
    for (;;)
    {
      auto chunk = co_await *reader;
      if (!chunk.has_value())
        break;
    }
  }
}

vio::task_t<void> handshake_client(vio::event_loop_t &loop, std::uint16_t port, double seconds, handshake_client_result_t &result)
{
  const auto config = client_config();
  result.window = window_t::starting(bench::now_ns(), seconds);
  auto meter = meter_cpu(loop, result.window, result.cpu);
  while (bench::now_ns() < result.window.end)
  {
    {
      auto client = create_client(loop, config);
      const auto started = bench::now_ns();
      require(co_await connect_client(client, port), "ssl_client_connect");
      const auto finished = bench::now_ns();
      if (result.window.contains(finished))
      {
        ++result.handshakes;
        result.latencies.add(finished - started);
      }
      if (result.negotiated.cipher == "unknown")
        result.negotiated = negotiated(client);
    }
    require(co_await vio::sleep(loop, std::chrono::nanoseconds(0)), "sleep");
  }
  co_await std::move(meter);
}

struct tls_server_handle_t
{
  std::uint16_t port = 0;
  vio::event_loop_t *loop = nullptr;
  vio::cancellation_t *stop = nullptr;
};
}

bench::result_t tls_bulk(const bench::args_t &args)
{
  const auto bytes = positive(args, "bytes");
  const auto chunk = positive(args, "chunk");

  std::promise<std::uint16_t> port_promise;
  auto port_future = port_promise.get_future();
  bulk_server_result_t server_result;
  std::thread server_thread([&] {
    vio::buffer_pool_t read_pool(vio::tls_stream_t::read_chunk);
    run_loop([&](vio::event_loop_t &loop) {
      auto listener = listen_loopback(loop, read_pool);
      return bulk_server(std::move(listener.server), [&port_promise, port = listener.port] { port_promise.set_value(port); }, server_result);
    });
  });

  bulk_client_result_t client_result;
  std::string backend;
  std::thread client_thread([&] {
    const auto port = port_future.get();
    backend = run_loop([&](vio::event_loop_t &loop) { return bulk_client(loop, port, bytes, chunk, client_result); });
  });
  client_thread.join();
  server_thread.join();

  const auto elapsed = server_result.eof_at - client_result.first_write_at;
  bench::result_t out;
  out.metrics["mib_per_s"] = mib_per_second(server_result.received, elapsed);
  out.metrics["cpu_s"] = server_result.cpu_at_eof - client_result.cpu_at_first_write;
  out.work["bytes"] = server_result.received;
  out.work["cipher"] = client_result.negotiated.cipher;
  out.work["group"] = client_result.negotiated.group;
  out.work["tls_library"] = tls_library();
  out.work["backend"] = backend;
  return out;
}

bench::result_t tls_handshake(const bench::args_t &args)
{
  const auto seconds = positive_real(args, "seconds");

  std::promise<tls_server_handle_t> server_promise;
  auto server_future = server_promise.get_future();
  handshake_server_result_t server_result;
  std::thread server_thread([&] {
    vio::cancellation_t stop;
    vio::buffer_pool_t read_pool(vio::tls_stream_t::read_chunk);
    run_loop([&](vio::event_loop_t &loop) {
      auto listener = listen_loopback(loop, read_pool);
      tls_server_handle_t handle{listener.port, &loop, &stop};
      return handshake_server(std::move(listener.server), stop, [&server_promise, handle] { server_promise.set_value(handle); }, server_result);
    });
  });

  const auto server = server_future.get();
  handshake_client_result_t client_result;
  std::string backend;
  std::thread client_thread([&] { backend = run_loop([&](vio::event_loop_t &loop) { return handshake_client(loop, server.port, seconds, client_result); }); });
  client_thread.join();
  server.loop->run_in_loop([stop = server.stop] { stop->cancel(); });
  server_thread.join();

  if (server_result.failures > 0)
    bench::fail(std::format("{} server handshakes failed, last: {}", server_result.failures, server_result.last_failure));

  bench::result_t out;
  out.metrics["handshakes_per_s"] = double(client_result.handshakes) / client_result.window.measured_s();
  out.metrics["p50_us"] = client_result.latencies.percentile_us(50);
  out.metrics["p99_us"] = client_result.latencies.percentile_us(99);
  out.metrics["cpu_s"] = client_result.cpu.used();
  out.work["handshakes"] = client_result.handshakes;
  out.work["cipher"] = client_result.negotiated.cipher;
  out.work["group"] = client_result.negotiated.group;
  out.work["tls_library"] = tls_library();
  out.work["backend"] = backend;
  return out;
}
}
