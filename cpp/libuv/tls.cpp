#include "uv_bench.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace uvb
{
namespace
{
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

void check_ssl(bool ok, std::string_view what)
{
  if (!ok)
    bench::fail(std::format("{}: {}", what, ssl_errors()));
}

using context_ptr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

context_ptr make_context(bool server)
{
  context_ptr context(SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()), SSL_CTX_free);
  check_ssl(context != nullptr, "SSL_CTX_new");
  auto *ctx = context.get();
  check_ssl(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1, "SSL_CTX_set_min_proto_version");
  check_ssl(SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) == 1, "SSL_CTX_set_max_proto_version");
  check_ssl(SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256") == 1, "SSL_CTX_set_ciphersuites");
  check_ssl(SSL_CTX_set1_groups_list(ctx, "X25519") == 1, "SSL_CTX_set1_groups_list");
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
  if (server)
  {
    check_ssl(SSL_CTX_use_certificate_chain_file(ctx, bench::server_cert_path().c_str()) == 1, "server certificate");
    check_ssl(SSL_CTX_use_PrivateKey_file(ctx, bench::server_key_path().c_str(), SSL_FILETYPE_PEM) == 1, "server key");
    check_ssl(SSL_CTX_check_private_key(ctx) == 1, "SSL_CTX_check_private_key");
    check_ssl(SSL_CTX_set_num_tickets(ctx, 0) == 1, "SSL_CTX_set_num_tickets");
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
  }
  else
  {
    check_ssl(SSL_CTX_load_verify_locations(ctx, bench::ca_cert_path().c_str(), nullptr) == 1, "CA certificate");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  }
  return context;
}

class tls_stream_t
{
public:
  explicit tls_stream_t(SSL_CTX *context)
    : _ssl(SSL_new(context))
  {
    check_ssl(_ssl != nullptr, "SSL_new");
    BIO *network_in = BIO_new(BIO_s_mem());
    BIO *network_out = BIO_new(BIO_s_mem());
    check_ssl(network_in != nullptr && network_out != nullptr, "BIO_new");
    SSL_set_bio(_ssl, network_in, network_out);
    _network_in = network_in;
    _network_out = network_out;
    _tcp.data = this;
    _connect.data = this;
  }

  tls_stream_t(const tls_stream_t &) = delete;
  tls_stream_t &operator=(const tls_stream_t &) = delete;

  virtual ~tls_stream_t()
  {
    SSL_free(_ssl);
  }

  void connect(uv_loop_t *loop, const sockaddr_in &server)
  {
    connect_to(loop, &_tcp, &_connect, server, on_connected);
  }

  void accept(uv_stream_t *listener)
  {
    accept_into(listener, &_tcp);
    SSL_set_accept_state(_ssl);
    start_reading();
  }

  [[nodiscard]] bool handshake_done() const
  {
    return _handshake_done;
  }

  [[nodiscard]] std::string cipher_name() const
  {
    const SSL_CIPHER *cipher = SSL_get_current_cipher(_ssl);
    return cipher ? SSL_CIPHER_get_name(cipher) : "unknown";
  }

  [[nodiscard]] std::string group_name() const
  {
    int nid = int(SSL_get_negotiated_group(_ssl));
    const char *name = OBJ_nid2sn(nid);
    return name ? name : std::format("group 0x{:x}", nid);
  }

protected:
  virtual void handshake_completed()
  {
  }

  virtual void plaintext_received(std::size_t)
  {
  }

  virtual void peer_closed()
  {
    close();
  }

  virtual void writes_drained()
  {
  }

  virtual void closed()
  {
  }

  void write_plaintext(const std::byte *data, std::size_t length)
  {
    int written = SSL_write(_ssl, data, int(length));
    if (written <= 0)
      fail_tls("SSL_write", written);
    bench::check(std::size_t(written) == length, "SSL_write wrote part of a chunk");
    flush();
  }

  void finish()
  {
    check_ssl(SSL_shutdown(_ssl) >= 0, "SSL_shutdown");
    flush();
    check_uv(uv_shutdown(&_shutdown, as_stream(&_tcp), on_shut_down), "uv_shutdown");
  }

  void close()
  {
    close_handle(&_tcp, on_closed);
  }

  [[nodiscard]] bool closing() const
  {
    return uv_is_closing(reinterpret_cast<const uv_handle_t *>(&_tcp)) != 0;
  }

private:
  struct cipher_write_t
  {
    uv_write_t request{};
    std::vector<char> data;
    tls_stream_t *owner = nullptr;
  };

  [[noreturn]] void fail_tls(std::string_view what, int rc)
  {
    int error = SSL_get_error(_ssl, rc);
    std::string detail = ssl_errors();
    long verify = SSL_get_verify_result(_ssl);
    if (verify != X509_V_OK)
      detail += std::format("; certificate: {}", X509_verify_cert_error_string(verify));
    bench::fail(std::format("{}: SSL error {}: {}", what, error, detail));
  }

  void start_reading()
  {
    check_uv(uv_read_start(as_stream(&_tcp), on_alloc, on_read), "uv_read_start");
  }

  void start_client()
  {
    SSL_set_connect_state(_ssl);
    check_ssl(SSL_set_tlsext_host_name(_ssl, server_name) == 1, "SSL_set_tlsext_host_name");
    check_ssl(SSL_set1_host(_ssl, server_name) == 1, "SSL_set1_host");
    start_reading();
    drive();
  }

  void drive()
  {
    if (!_handshake_done)
    {
      int rc = SSL_do_handshake(_ssl);
      if (rc != 1)
      {
        if (SSL_get_error(_ssl, rc) != SSL_ERROR_WANT_READ)
          fail_tls("handshake", rc);
        flush();
        return;
      }
      _handshake_done = true;
      handshake_completed();
    }
    while (!closing())
    {
      int n = SSL_read(_ssl, _plaintext.data(), int(_plaintext.size()));
      if (n > 0)
      {
        plaintext_received(std::size_t(n));
        continue;
      }
      int error = SSL_get_error(_ssl, n);
      if (error == SSL_ERROR_WANT_READ)
        break;
      if (error == SSL_ERROR_ZERO_RETURN)
      {
        peer_closed();
        return;
      }
      fail_tls("SSL_read", n);
    }
    flush();
  }

  void flush()
  {
    std::size_t pending = BIO_ctrl_pending(_network_out);
    if (pending == 0 || closing())
      return;
    cipher_write_t *write = nullptr;
    if (_idle_writes.empty())
    {
      write = _writes.emplace_back(std::make_unique<cipher_write_t>()).get();
      write->owner = this;
      write->request.data = write;
    }
    else
    {
      write = _idle_writes.back();
      _idle_writes.pop_back();
    }
    write->data.resize(pending);
    int n = BIO_read(_network_out, write->data.data(), int(pending));
    check_ssl(n == int(pending), "BIO_read");
    auto buf = uv_buf_init(write->data.data(), unsigned(n));
    ++_writes_in_flight;
    check_uv(uv_write(&write->request, as_stream(&_tcp), &buf, 1, on_written), "uv_write");
  }

  static void on_connected(uv_connect_t *request, int status)
  {
    check_uv(status, "connect");
    static_cast<tls_stream_t *>(request->data)->start_client();
  }

  static void on_alloc(uv_handle_t *handle, std::size_t, uv_buf_t *buf)
  {
    auto *self = static_cast<tls_stream_t *>(handle->data);
    *buf = uv_buf_init(self->_ciphertext.data(), unsigned(self->_ciphertext.size()));
  }

  static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
  {
    auto *self = static_cast<tls_stream_t *>(stream->data);
    if (nread == 0)
      return;
    if (nread < 0)
    {
      if (nread != UV_EOF)
        check_uv(int(nread), "TLS read");
      uv_read_stop(stream);
      self->peer_closed();
      return;
    }
    check_ssl(BIO_write(self->_network_in, buf->base, int(nread)) == int(nread), "BIO_write");
    self->drive();
  }

  static void on_written(uv_write_t *request, int status)
  {
    auto *write = static_cast<cipher_write_t *>(request->data);
    auto *self = write->owner;
    self->_idle_writes.push_back(write);
    --self->_writes_in_flight;
    if (self->closing())
      return;
    check_uv(status, "TLS write");
    if (self->_writes_in_flight == 0)
      self->writes_drained();
  }

  static void on_shut_down(uv_shutdown_t *request, int status)
  {
    auto *self = static_cast<tls_stream_t *>(request->handle->data);
    if (!self->closing())
      check_uv(status, "shutdown");
    self->close();
  }

  static void on_closed(uv_handle_t *handle)
  {
    static_cast<tls_stream_t *>(handle->data)->closed();
  }

  SSL *_ssl;
  BIO *_network_in = nullptr;
  BIO *_network_out = nullptr;
  uv_tcp_t _tcp{};
  uv_connect_t _connect{};
  uv_shutdown_t _shutdown{};
  bool _handshake_done = false;
  std::vector<char> _ciphertext = std::vector<char>(bench::server_read_buffer);
  std::vector<char> _plaintext = std::vector<char>(bench::server_read_buffer);
  std::vector<std::unique_ptr<cipher_write_t>> _writes;
  std::vector<cipher_write_t *> _idle_writes;
  std::size_t _writes_in_flight = 0;
};

class bulk_sink_t final : public tls_stream_t
{
public:
  using tls_stream_t::tls_stream_t;

  std::int64_t received = 0;
  std::int64_t end_ns = 0;
  double end_cpu = 0.0;

protected:
  void plaintext_received(std::size_t length) override
  {
    received += std::int64_t(length);
  }

  void peer_closed() override
  {
    end_ns = bench::now_ns();
    end_cpu = bench::process_cpu_s();
    close();
  }
};

class bulk_source_t final : public tls_stream_t
{
public:
  bulk_source_t(SSL_CTX *context, std::int64_t total, std::int64_t chunk)
    : tls_stream_t(context)
    , _chunk(bench::payload(std::size_t(chunk)))
    , _total(total)
  {
  }

  std::int64_t start_ns = 0;
  double start_cpu = 0.0;
  std::string cipher;
  std::string group;

protected:
  void handshake_completed() override
  {
    cipher = cipher_name();
    group = group_name();
    start_ns = bench::now_ns();
    start_cpu = bench::process_cpu_s();
    write_next();
  }

  void writes_drained() override
  {
    if (!handshake_done() || _finishing)
      return;
    if (_sent < _total)
    {
      write_next();
      return;
    }
    _finishing = true;
    finish();
  }

  void peer_closed() override
  {
    if (!_finishing)
      bench::fail("server closed the TLS stream before the client finished");
  }

private:
  void write_next()
  {
    auto length = std::min<std::int64_t>(std::int64_t(_chunk.size()), _total - _sent);
    _sent += length;
    write_plaintext(_chunk.data(), std::size_t(length));
  }

  std::vector<std::byte> _chunk;
  std::int64_t _total;
  std::int64_t _sent = 0;
  bool _finishing = false;
};

struct tls_sink_server_t
{
  uv_tcp_t listener{};
  SSL_CTX *context = nullptr;
  std::unique_ptr<bulk_sink_t> connection;
};

void tls_sink_accept(uv_stream_t *listener, int status)
{
  check_uv(status, "connection");
  auto *server = static_cast<tls_sink_server_t *>(listener->data);
  server->connection = std::make_unique<bulk_sink_t>(server->context);
  server->connection->accept(listener);
  close_handle(&server->listener);
}

void run_tls_sink_server(tls_sink_server_t &server, std::promise<int> &port)
{
  auto context = make_context(true);
  server.context = context.get();
  loop_t loop;
  check_uv(uv_tcp_init(loop.get(), &server.listener), "uv_tcp_init");
  server.listener.data = &server;
  port.set_value(listen_on_loopback(&server.listener, tls_sink_accept));
  loop.run_and_close();
}

class handshake_peer_t final : public tls_stream_t
{
public:
  using tls_stream_t::tls_stream_t;

protected:
  void closed() override
  {
    delete this;
  }
};

struct handshake_server_t
{
  uv_tcp_t listener{};
  uv_async_t stop{};
  SSL_CTX *context = nullptr;
};

void handshake_accept(uv_stream_t *listener, int status)
{
  check_uv(status, "connection");
  auto *server = static_cast<handshake_server_t *>(listener->data);
  auto *peer = new handshake_peer_t(server->context);
  peer->accept(listener);
}

void handshake_server_stop(uv_async_t *stop)
{
  auto *server = static_cast<handshake_server_t *>(stop->data);
  close_handle(&server->listener);
  close_handle(&server->stop);
}

void run_handshake_server(handshake_server_t &server, std::promise<int> &port)
{
  auto context = make_context(true);
  server.context = context.get();
  loop_t loop;
  check_uv(uv_tcp_init(loop.get(), &server.listener), "uv_tcp_init");
  server.listener.data = &server;
  check_uv(uv_async_init(loop.get(), &server.stop, handshake_server_stop), "uv_async_init");
  server.stop.data = &server;
  port.set_value(listen_on_loopback(&server.listener, handshake_accept));
  loop.run_and_close();
}

class handshake_client_t;

class handshake_connection_t final : public tls_stream_t
{
public:
  handshake_connection_t(SSL_CTX *context, handshake_client_t &client)
    : tls_stream_t(context)
    , _client(client)
  {
  }

protected:
  void handshake_completed() override;
  void closed() override;

  void peer_closed() override
  {
    if (!handshake_done())
      bench::fail("server closed the connection during the handshake");
  }

private:
  handshake_client_t &_client;
};

class handshake_client_t
{
public:
  handshake_client_t(uv_loop_t *loop, SSL_CTX *context, const sockaddr_in &server, double seconds)
    : window(loop, seconds)
    , _loop(loop)
    , _context(context)
    , _server(server)
  {
  }

  void start()
  {
    window.start([] {});
    connect_next();
  }

  void completed(handshake_connection_t &connection)
  {
    auto now = bench::now_ns();
    if (window.measuring())
    {
      ++handshakes;
      latencies.add(now - _started_ns);
    }
    if (cipher.empty())
    {
      cipher = connection.cipher_name();
      group = connection.group_name();
    }
  }

  void connection_closed()
  {
    _current.reset();
    if (!window.ended())
      connect_next();
  }

  window_t window;
  std::int64_t handshakes = 0;
  bench::latencies_t latencies;
  std::string cipher;
  std::string group;

private:
  void connect_next()
  {
    _started_ns = bench::now_ns();
    _current = std::make_unique<handshake_connection_t>(_context, *this);
    _current->connect(_loop, _server);
  }

  uv_loop_t *_loop;
  SSL_CTX *_context;
  sockaddr_in _server;
  std::unique_ptr<handshake_connection_t> _current;
  std::int64_t _started_ns = 0;
};

void handshake_connection_t::handshake_completed()
{
  _client.completed(*this);
  finish();
}

void handshake_connection_t::closed()
{
  _client.connection_closed();
}
}

bench::result_t tls_bulk(const bench::args_t &args)
{
  auto bytes = positive(args, "bytes");
  auto chunk = positive(args, "chunk");

  tls_sink_server_t server;
  std::promise<int> port;
  auto port_ready = port.get_future();
  std::thread server_thread([&] { run_tls_sink_server(server, port); });

  auto context = make_context(false);
  loop_t loop;
  bulk_source_t source(context.get(), bytes, chunk);
  source.connect(loop.get(), loopback(port_ready.get()));
  loop.run_and_close();
  server_thread.join();

  bench::check(server.connection != nullptr, "the server never accepted a connection");
  auto &sink = *server.connection;
  bench::result_t result;
  result.metrics["mib_per_s"] = mib_per_s(sink.received, sink.end_ns - source.start_ns);
  result.metrics["cpu_s"] = sink.end_cpu - source.start_cpu;
  result.work["bytes"] = sink.received;
  result.work["cipher"] = source.cipher;
  result.work["group"] = source.group;
  result.work["tls_library"] = std::string(OpenSSL_version(OPENSSL_VERSION));
  return result;
}

bench::result_t tls_handshake(const bench::args_t &args)
{
  auto seconds = positive_seconds(args);

  handshake_server_t server;
  std::promise<int> port;
  auto port_ready = port.get_future();
  std::thread server_thread([&] { run_handshake_server(server, port); });

  auto context = make_context(false);
  loop_t loop;
  handshake_client_t client(loop.get(), context.get(), loopback(port_ready.get()), seconds);
  client.start();
  loop.run_and_close();
  check_uv(uv_async_send(&server.stop), "uv_async_send");
  server_thread.join();

  bench::result_t result;
  result.metrics["handshakes_per_s"] = double(client.handshakes) / client.window.elapsed_s();
  result.metrics["p50_us"] = client.latencies.percentile_us(50);
  result.metrics["p99_us"] = client.latencies.percentile_us(99);
  result.metrics["cpu_s"] = client.window.cpu_s();
  result.work["handshakes"] = client.handshakes;
  result.work["cipher"] = client.cipher;
  result.work["group"] = client.group;
  result.work["tls_library"] = std::string(OpenSSL_version(OPENSSL_VERSION));
  return result;
}
}
