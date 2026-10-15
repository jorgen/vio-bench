#include "asio_bench.h"

#include <asio/post.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace asio_bench
{
namespace
{
using asio::ip::tcp;
using tls_stream_t = asio::ssl::stream<tcp::socket>;

constexpr const char *server_name = "localhost";

std::string ssl_errors()
{
  std::string out;
  while (unsigned long code = ERR_get_error())
  {
    char text[256];
    ERR_error_string_n(code, text, sizeof(text));
    if (!out.empty())
      out += "; ";
    out += text;
  }
  return out.empty() ? std::string("no OpenSSL error queued") : out;
}

void require_ssl(bool ok, std::string_view what)
{
  if (!ok)
    bench::fail(std::format("{}: {}", what, ssl_errors()));
}

asio::ssl::context make_context(bool server)
{
  asio::ssl::context context(server ? asio::ssl::context::tlsv13_server : asio::ssl::context::tlsv13_client);
  auto *native = context.native_handle();
  require_ssl(SSL_CTX_set_ciphersuites(native, "TLS_AES_128_GCM_SHA256") == 1, "SSL_CTX_set_ciphersuites");
  require_ssl(SSL_CTX_set1_groups_list(native, "X25519") == 1, "SSL_CTX_set1_groups_list");
  SSL_CTX_set_session_cache_mode(native, SSL_SESS_CACHE_OFF);
  if (server)
  {
    require_ssl(SSL_CTX_set_num_tickets(native, 0) == 1, "SSL_CTX_set_num_tickets");
    SSL_CTX_set_options(native, SSL_OP_NO_TICKET);
    context.use_certificate_chain_file(bench::server_cert_path());
    context.use_private_key_file(bench::server_key_path(), asio::ssl::context::pem);
  }
  else
  {
    context.load_verify_file(bench::ca_cert_path());
    context.set_verify_mode(asio::ssl::verify_peer);
  }
  return context;
}

void prepare_client(tls_stream_t &stream)
{
  require_ssl(SSL_set_tlsext_host_name(stream.native_handle(), server_name) == 1, "SSL_set_tlsext_host_name");
  stream.set_verify_callback(asio::ssl::host_name_verification(server_name));
}

struct negotiated_t
{
  std::string cipher;
  std::string group;
};

negotiated_t negotiated(tls_stream_t &stream)
{
  auto *ssl = stream.native_handle();
  negotiated_t out;
  const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
  out.cipher = cipher ? SSL_CIPHER_get_name(cipher) : "unknown";
  int nid = int(SSL_get_negotiated_group(ssl));
  const char *group = OBJ_nid2sn(nid);
  out.group = group ? group : std::format("group 0x{:x}", nid);
  return out;
}

void add_tls_work(bench::result_t &result, const negotiated_t &negotiated)
{
  result.work["cipher"] = negotiated.cipher;
  result.work["group"] = negotiated.group;
  result.work["tls_library"] = std::string(OpenSSL_version(OPENSSL_VERSION));
  result.work["backend"] = reactor_backend();
}

struct tls_sink_result_t
{
  std::int64_t received = 0;
  std::int64_t end_ns = 0;
  double end_cpu = 0.0;
};

asio::awaitable<void> tls_sink(tcp::acceptor acceptor, asio::ssl::context &context, tls_sink_result_t &result)
{
  auto socket = co_await acceptor.async_accept(asio::deferred);
  acceptor.close();
  socket.set_option(tcp::no_delay(true));
  tls_stream_t stream(std::move(socket), context);
  co_await stream.async_handshake(asio::ssl::stream_base::server, asio::deferred);
  std::vector<char> buffer(bench::server_read_buffer);
  std::error_code ending;
  for (;;)
  {
    auto [error, length] = co_await stream.async_read_some(asio::buffer(buffer), with_error);
    if (error)
    {
      ending = error;
      break;
    }
    result.received += std::int64_t(length);
  }
  result.end_ns = bench::now_ns();
  result.end_cpu = bench::process_cpu_s();
  if (ending == asio::ssl::error::stream_truncated)
    co_return;
  if (ending != asio::error::eof)
    fail("tls read", ending);
  co_await stream.async_shutdown(asio::deferred);
}

void run_tls_sink_server(tls_sink_result_t &result, std::promise<unsigned short> &port)
{
  auto tls = make_context(true);
  asio::io_context context(1);
  auto acceptor = listen_loopback(context);
  port.set_value(acceptor.local_endpoint().port());
  spawn(context.get_executor(), tls_sink(std::move(acceptor), tls, result), "tls_bulk server");
  context.run();
}

struct tls_source_result_t
{
  std::int64_t start_ns = 0;
  double start_cpu = 0.0;
  negotiated_t negotiated;
};

asio::awaitable<void> tls_source(tcp::endpoint server, asio::ssl::context &context, std::int64_t bytes, std::int64_t chunk,
                                 tls_source_result_t &result)
{
  tls_stream_t stream(co_await asio::this_coro::executor, context);
  prepare_client(stream);
  co_await stream.next_layer().async_connect(server, asio::deferred);
  stream.next_layer().set_option(tcp::no_delay(true));
  co_await stream.async_handshake(asio::ssl::stream_base::client, asio::deferred);
  result.negotiated = negotiated(stream);
  auto payload = bench::payload(std::size_t(chunk));
  result.start_ns = bench::now_ns();
  result.start_cpu = bench::process_cpu_s();
  for (std::int64_t sent = 0; sent < bytes;)
  {
    auto length = std::min(chunk, bytes - sent);
    co_await asio::async_write(stream, asio::buffer(payload.data(), std::size_t(length)), asio::deferred);
    sent += length;
  }
  co_await stream.async_shutdown(asio::deferred);
}

struct handshake_server_t
{
  asio::io_context *context = nullptr;
  tcp::acceptor *acceptor = nullptr;
  unsigned short port = 0;
};

asio::awaitable<void> handshake_peer(tcp::socket socket, asio::ssl::context &context)
{
  socket.set_option(tcp::no_delay(true));
  tls_stream_t stream(std::move(socket), context);
  co_await stream.async_handshake(asio::ssl::stream_base::server, asio::deferred);
  std::array<char, 256> buffer{};
  for (;;)
  {
    auto [error, length] = co_await stream.async_read_some(asio::buffer(buffer), with_error);
    if (!error)
      continue;
    if (error == asio::ssl::error::stream_truncated)
      co_return;
    if (error != asio::error::eof)
      fail("tls_handshake server read", error);
    co_await stream.async_shutdown(asio::deferred);
    co_return;
  }
}

asio::awaitable<void> handshake_acceptor(tcp::acceptor &acceptor, asio::ssl::context &context)
{
  auto executor = co_await asio::this_coro::executor;
  for (;;)
  {
    auto [error, socket] = co_await acceptor.async_accept(with_error);
    if (error == asio::error::operation_aborted)
      co_return;
    if (error)
      fail("accept", error);
    spawn(executor, handshake_peer(std::move(socket), context), "tls_handshake server connection");
  }
}

void run_handshake_server(std::promise<handshake_server_t> &ready)
{
  auto tls = make_context(true);
  asio::io_context context(1);
  auto acceptor = listen_loopback(context);
  ready.set_value({&context, &acceptor, acceptor.local_endpoint().port()});
  spawn(context.get_executor(), handshake_acceptor(acceptor, tls), "tls_handshake server");
  context.run();
}

struct handshake_client_t
{
  explicit handshake_client_t(double seconds)
    : window(seconds)
  {
  }

  window_t window;
  std::int64_t handshakes = 0;
  bench::latencies_t latencies;
  negotiated_t negotiated;
};

asio::awaitable<void> close_tls(std::unique_ptr<tls_stream_t> stream)
{
  co_await stream->async_shutdown(asio::deferred);
}

asio::awaitable<void> handshake_client(tcp::endpoint server, asio::ssl::context &context, handshake_client_t &client)
{
  auto executor = co_await asio::this_coro::executor;
  spawn(executor, client.window.run({}, {}), "tls_handshake window");
  while (!client.window.ended())
  {
    auto started = bench::now_ns();
    auto stream = std::make_unique<tls_stream_t>(executor, context);
    prepare_client(*stream);
    co_await stream->next_layer().async_connect(server, asio::deferred);
    stream->next_layer().set_option(tcp::no_delay(true));
    co_await stream->async_handshake(asio::ssl::stream_base::client, asio::deferred);
    auto finished = bench::now_ns();
    if (client.window.measuring())
    {
      ++client.handshakes;
      client.latencies.add(finished - started);
    }
    if (client.negotiated.cipher.empty())
      client.negotiated = negotiated(*stream);
    spawn(executor, close_tls(std::move(stream)), "tls_handshake close");
  }
}
}

bench::result_t tls_bulk(const bench::args_t &args)
{
  auto bytes = positive(args, "bytes");
  auto chunk = positive(args, "chunk");

  tls_sink_result_t server_result;
  std::promise<unsigned short> port;
  auto port_ready = port.get_future();
  std::thread server([&] { run_tls_sink_server(server_result, port); });

  auto tls = make_context(false);
  asio::io_context context(1);
  tls_source_result_t client_result;
  spawn(context.get_executor(), tls_source(tcp_loopback(port_ready.get()), tls, bytes, chunk, client_result), "tls_bulk client");
  context.run();
  server.join();

  bench::result_t result;
  result.metrics["mib_per_s"] = mib_per_s(server_result.received, server_result.end_ns - client_result.start_ns);
  result.metrics["cpu_s"] = server_result.end_cpu - client_result.start_cpu;
  result.work["bytes"] = server_result.received;
  add_tls_work(result, client_result.negotiated);
  return result;
}

bench::result_t tls_handshake(const bench::args_t &args)
{
  auto seconds = positive_seconds(args);

  std::promise<handshake_server_t> server_ready;
  auto server_future = server_ready.get_future();
  std::thread server_thread([&] { run_handshake_server(server_ready); });
  auto server = server_future.get();

  auto tls = make_context(false);
  asio::io_context context(1);
  handshake_client_t client(seconds);
  spawn(context.get_executor(), handshake_client(tcp_loopback(server.port), tls, client), "tls_handshake client");
  context.run();
  asio::post(*server.context, [acceptor = server.acceptor] { acceptor->close(); });
  server_thread.join();

  bench::result_t result;
  result.metrics["handshakes_per_s"] = double(client.handshakes) / client.window.elapsed_s();
  result.metrics["p50_us"] = client.latencies.percentile_us(50);
  result.metrics["p99_us"] = client.latencies.percentile_us(99);
  result.metrics["cpu_s"] = client.window.cpu_s();
  result.work["handshakes"] = client.handshakes;
  add_tls_work(result, client.negotiated);
  return result;
}
}
