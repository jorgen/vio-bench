use crate::bench::{self, Args, Latencies, Report, SERVER_READ_BUFFER, fail};
use crate::support::{OrFail, Window, join_side, mib_per_s, positive, positive_seconds, runtime, spawn_side};
use crate::tcp::{accept_one, connect, listen_on_loopback};
use foreign_types::ForeignTypeRef;
use openssl::nid::Nid;
use openssl::ssl::{
    Ssl, SslContext, SslFiletype, SslMethod, SslOptions, SslSessionCacheMode, SslVerifyMode, SslVersion,
};
use rustls::client::Resumption;
use rustls::crypto::CryptoProvider;
use rustls::crypto::aws_lc_rs;
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, ServerName};
use rustls::server::NoServerSessionStorage;
use rustls::{ClientConfig, RootCertStore, ServerConfig};
use std::ffi::{c_int, c_long};
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::mpsc::{SyncSender, sync_channel};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::oneshot;
use tokio_openssl::SslStream;
use tokio_rustls::{TlsAcceptor, TlsConnector};

const SERVER_NAME: &str = "localhost";
const SSL_CTRL_GET_NEGOTIATED_GROUP: c_int = 134;

struct Negotiated {
    cipher: String,
    group: String,
}

trait Tls: 'static {
    type Acceptor: Clone + Send + Sync + 'static;
    type Connector: Send + Sync + 'static;
    type ServerStream: AsyncRead + AsyncWrite + Unpin + Send + 'static;
    type ClientStream: AsyncRead + AsyncWrite + Unpin + Send + 'static;

    fn library() -> String;
    fn acceptor() -> Self::Acceptor;
    fn connector() -> Self::Connector;
    fn accept(acceptor: &Self::Acceptor, tcp: TcpStream) -> impl Future<Output = io::Result<Self::ServerStream>> + Send;
    fn connect(connector: &Self::Connector, tcp: TcpStream) -> impl Future<Output = io::Result<Self::ClientStream>> + Send;
    fn negotiated(stream: &Self::ClientStream) -> Negotiated;
}

struct Rustls;

fn rustls_provider() -> Arc<CryptoProvider> {
    let mut provider = aws_lc_rs::default_provider();
    provider.cipher_suites = vec![aws_lc_rs::cipher_suite::TLS13_AES_128_GCM_SHA256];
    provider.kx_groups = vec![aws_lc_rs::kx_group::X25519];
    Arc::new(provider)
}

fn iana_cipher_name(rustls_name: &str) -> String {
    match rustls_name.strip_prefix("TLS13_") {
        Some(rest) => format!("TLS_{rest}"),
        None => rustls_name.to_string(),
    }
}

impl Tls for Rustls {
    type Acceptor = TlsAcceptor;
    type Connector = TlsConnector;
    type ServerStream = tokio_rustls::server::TlsStream<TcpStream>;
    type ClientStream = tokio_rustls::client::TlsStream<TcpStream>;

    fn library() -> String {
        format!("rustls {} (aws-lc-rs)", env!("BENCH_RUSTLS_VERSION"))
    }

    fn acceptor() -> TlsAcceptor {
        let chain: Vec<CertificateDer<'static>> = CertificateDer::pem_file_iter(bench::server_cert_path())
            .or_fail("server certificate")
            .collect::<Result<_, _>>()
            .or_fail("server certificate");
        let key = PrivateKeyDer::from_pem_file(bench::server_key_path()).or_fail("server key");
        let mut config = ServerConfig::builder_with_provider(rustls_provider())
            .with_protocol_versions(&[&rustls::version::TLS13])
            .or_fail("rustls server protocol versions")
            .with_no_client_auth()
            .with_single_cert(chain, key)
            .or_fail("server certificate and key");
        config.send_tls13_tickets = 0;
        config.session_storage = Arc::new(NoServerSessionStorage {});
        TlsAcceptor::from(Arc::new(config))
    }

    fn connector() -> TlsConnector {
        let mut roots = RootCertStore::empty();
        for certificate in CertificateDer::pem_file_iter(bench::ca_cert_path()).or_fail("CA certificate") {
            roots.add(certificate.or_fail("CA certificate")).or_fail("CA certificate");
        }
        let mut config = ClientConfig::builder_with_provider(rustls_provider())
            .with_protocol_versions(&[&rustls::version::TLS13])
            .or_fail("rustls client protocol versions")
            .with_root_certificates(roots)
            .with_no_client_auth();
        config.resumption = Resumption::disabled();
        TlsConnector::from(Arc::new(config))
    }

    fn accept(acceptor: &TlsAcceptor, tcp: TcpStream) -> impl Future<Output = io::Result<Self::ServerStream>> + Send {
        acceptor.accept(tcp)
    }

    fn connect(connector: &TlsConnector, tcp: TcpStream) -> impl Future<Output = io::Result<Self::ClientStream>> + Send {
        connector.connect(ServerName::try_from(SERVER_NAME).or_fail("server name"), tcp)
    }

    fn negotiated(stream: &Self::ClientStream) -> Negotiated {
        let (_, connection) = stream.get_ref();
        let cipher = connection
            .negotiated_cipher_suite()
            .and_then(|suite| suite.suite().as_str())
            .map_or_else(|| "unknown".to_string(), iana_cipher_name);
        let group = connection
            .negotiated_key_exchange_group()
            .and_then(|group| group.name().as_str())
            .unwrap_or("unknown")
            .to_string();
        Negotiated { cipher, group }
    }
}

struct OpenSsl;

enum Side {
    Server,
    Client,
}

fn openssl_context(side: Side) -> SslContext {
    let method = match side {
        Side::Server => SslMethod::tls_server(),
        Side::Client => SslMethod::tls_client(),
    };
    let mut builder = SslContext::builder(method).or_fail("SSL_CTX_new");
    builder.set_min_proto_version(Some(SslVersion::TLS1_3)).or_fail("SSL_CTX_set_min_proto_version");
    builder.set_max_proto_version(Some(SslVersion::TLS1_3)).or_fail("SSL_CTX_set_max_proto_version");
    builder.set_ciphersuites("TLS_AES_128_GCM_SHA256").or_fail("SSL_CTX_set_ciphersuites");
    builder.set_groups_list("X25519").or_fail("SSL_CTX_set1_groups_list");
    builder.set_session_cache_mode(SslSessionCacheMode::OFF);
    builder.set_read_ahead(true);
    match side {
        Side::Server => {
            builder.set_certificate_chain_file(bench::server_cert_path()).or_fail("server certificate");
            builder.set_private_key_file(bench::server_key_path(), SslFiletype::PEM).or_fail("server key");
            builder.check_private_key().or_fail("SSL_CTX_check_private_key");
            builder.set_num_tickets(0).or_fail("SSL_CTX_set_num_tickets");
            builder.set_options(SslOptions::NO_TICKET);
        }
        Side::Client => {
            builder.set_ca_file(bench::ca_cert_path()).or_fail("CA certificate");
            builder.set_verify(SslVerifyMode::PEER);
        }
    }
    builder.build()
}

fn client_ssl(context: &SslContext) -> Result<Ssl, openssl::error::ErrorStack> {
    let mut ssl = Ssl::new(context)?;
    ssl.set_hostname(SERVER_NAME)?;
    ssl.param_mut().set_host(SERVER_NAME)?;
    Ok(ssl)
}

impl Tls for OpenSsl {
    type Acceptor = SslContext;
    type Connector = SslContext;
    type ServerStream = SslStream<TcpStream>;
    type ClientStream = SslStream<TcpStream>;

    fn library() -> String {
        openssl::version::version().to_string()
    }

    fn acceptor() -> SslContext {
        openssl_context(Side::Server)
    }

    fn connector() -> SslContext {
        openssl_context(Side::Client)
    }

    fn accept(context: &SslContext, tcp: TcpStream) -> impl Future<Output = io::Result<Self::ServerStream>> + Send {
        let stream = Ssl::new(context).and_then(|ssl| SslStream::new(ssl, tcp));
        async move {
            let mut stream = stream.map_err(io::Error::other)?;
            Pin::new(&mut stream).accept().await.map_err(io::Error::other)?;
            Ok(stream)
        }
    }

    fn connect(context: &SslContext, tcp: TcpStream) -> impl Future<Output = io::Result<Self::ClientStream>> + Send {
        let stream = client_ssl(context).and_then(|ssl| SslStream::new(ssl, tcp));
        async move {
            let mut stream = stream.map_err(io::Error::other)?;
            Pin::new(&mut stream).connect().await.map_err(io::Error::other)?;
            Ok(stream)
        }
    }

    fn negotiated(stream: &Self::ClientStream) -> Negotiated {
        let ssl = stream.ssl();
        let cipher = ssl.current_cipher().map_or("unknown", |cipher| cipher.name()).to_string();
        let nid = unsafe {
            openssl_sys::SSL_ctrl(ssl.as_ptr(), SSL_CTRL_GET_NEGOTIATED_GROUP, 0 as c_long, std::ptr::null_mut())
        };
        let group = Nid::from_raw(nid as c_int)
            .short_name()
            .map_or_else(|_| format!("group 0x{nid:x}"), str::to_string);
        Negotiated { cipher, group }
    }
}

enum Backend {
    Rustls,
    OpenSsl,
}

const BACKEND_NAMES: [&str; 2] = ["rustls", "openssl"];

fn backend() -> Backend {
    let Some(chosen) = std::env::var_os("BENCH_TLS") else {
        fail("BENCH_TLS is not set: it must name the TLS library, rustls or openssl");
    };
    let backend = match chosen.to_str() {
        Some("rustls") => Backend::Rustls,
        Some("openssl") => Backend::OpenSsl,
        _ => fail(&format!("BENCH_TLS must be rustls or openssl, not '{}'", chosen.to_string_lossy())),
    };
    if let Some(variant) = std::env::var_os("BENCH_VARIANT")
        && BACKEND_NAMES.iter().any(|name| variant == *name)
        && variant != chosen
    {
        fail(&format!(
            "BENCH_VARIANT '{}' names a different TLS library than BENCH_TLS '{}'",
            variant.to_string_lossy(),
            chosen.to_string_lossy()
        ));
    }
    backend
}

struct SinkOutcome {
    received: i64,
    end_ns: i64,
    end_cpu: f64,
}

async fn bulk_sink<T: Tls>(port: SyncSender<u16>) -> SinkOutcome {
    let acceptor = T::acceptor();
    let listener = listen_on_loopback(port).await;
    let tcp = accept_one(&listener).await;
    drop(listener);
    let mut stream = T::accept(&acceptor, tcp).await.or_fail("server handshake");
    let mut buffer = vec![0u8; SERVER_READ_BUFFER];
    let mut received = 0i64;
    loop {
        match stream.read(&mut buffer).await {
            Ok(0) => break,
            Ok(length) => received += length as i64,
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(error) => fail(&format!("TLS read: {error}")),
        }
    }
    SinkOutcome { received, end_ns: bench::now_ns(), end_cpu: bench::process_cpu_s() }
}

struct SourceOutcome {
    start_ns: i64,
    start_cpu: f64,
    negotiated: Negotiated,
}

async fn bulk_source<T: Tls>(port: u16, bytes: i64, chunk: i64) -> SourceOutcome {
    let connector = T::connector();
    let tcp = connect(port).await;
    let mut stream = T::connect(&connector, tcp).await.or_fail("client handshake");
    let negotiated = T::negotiated(&stream);
    let data = bench::payload(chunk as usize);
    let start_ns = bench::now_ns();
    let start_cpu = bench::process_cpu_s();
    let mut sent = 0i64;
    while sent < bytes {
        let length = chunk.min(bytes - sent);
        stream.write_all(&data[..length as usize]).await.or_fail("TLS write");
        sent += length;
    }
    stream.shutdown().await.or_fail("close_notify");
    SourceOutcome { start_ns, start_cpu, negotiated }
}

fn bulk<T: Tls>(bytes: i64, chunk: i64) -> Report {
    let (port, port_ready) = sync_channel(1);
    let server = spawn_side("tls-sink", move || runtime().block_on(bulk_sink::<T>(port)));
    let port = port_ready.recv().or_fail("the TLS sink did not start");
    let source = runtime().block_on(bulk_source::<T>(port, bytes, chunk));
    let sink = join_side(server, "TLS sink");

    let mut report = Report::default();
    report.metric("mib_per_s", mib_per_s(sink.received, sink.end_ns - source.start_ns));
    report.metric("cpu_s", sink.end_cpu - source.start_cpu);
    report.work("bytes", sink.received);
    report.work("cipher", source.negotiated.cipher);
    report.work("group", source.negotiated.group);
    report.work("tls_library", T::library());
    report
}

pub fn tls_bulk(args: &Args) -> Report {
    let bytes = positive(args, "bytes");
    let chunk = positive(args, "chunk");
    match backend() {
        Backend::Rustls => bulk::<Rustls>(bytes, chunk),
        Backend::OpenSsl => bulk::<OpenSsl>(bytes, chunk),
    }
}

async fn serve_one_handshake<T: Tls>(acceptor: T::Acceptor, tcp: TcpStream) {
    let mut stream = T::accept(&acceptor, tcp).await.or_fail("server handshake");
    let mut until_closed = [0u8; 1];
    let _ = stream.read(&mut until_closed).await;
}

async fn handshake_server<T: Tls>(port: SyncSender<u16>, mut stop: oneshot::Receiver<()>) {
    let acceptor = T::acceptor();
    let listener = listen_on_loopback(port).await;
    loop {
        tokio::select! {
            tcp = accept_one(&listener) => {
                tokio::spawn(serve_one_handshake::<T>(acceptor.clone(), tcp));
            }
            _ = &mut stop => return,
        }
    }
}

struct HandshakeOutcome {
    handshakes: i64,
    latencies: Latencies,
    negotiated: Option<Negotiated>,
    elapsed_s: f64,
    cpu_s: f64,
}

async fn handshake_client<T: Tls>(port: u16, seconds: f64) -> HandshakeOutcome {
    let connector = T::connector();
    let window = Window::start(seconds);
    let mut handshakes = 0i64;
    let mut latencies = Latencies::default();
    let mut negotiated = None;
    while !window.phase.ended() {
        let started = bench::now_ns();
        let tcp = connect(port).await;
        let mut stream = T::connect(&connector, tcp).await.or_fail("client handshake");
        let done = bench::now_ns();
        if window.phase.measuring() {
            handshakes += 1;
            latencies.add(done - started);
        }
        if negotiated.is_none() {
            negotiated = Some(T::negotiated(&stream));
        }
        stream.shutdown().await.or_fail("close");
    }
    let span = window.finished().await;
    HandshakeOutcome { handshakes, latencies, negotiated, elapsed_s: span.elapsed_s(), cpu_s: span.cpu_s() }
}

fn handshake<T: Tls>(seconds: f64) -> Report {
    let (port, port_ready) = sync_channel(1);
    let (stop, stopped) = oneshot::channel();
    let server = spawn_side("tls-server", move || runtime().block_on(handshake_server::<T>(port, stopped)));
    let port = port_ready.recv().or_fail("the TLS server did not start");
    let client = runtime().block_on(handshake_client::<T>(port, seconds));
    let _ = stop.send(());
    join_side(server, "TLS server");

    let negotiated = client.negotiated.or_fail("no handshake completed");
    let mut report = Report::default();
    report.metric("handshakes_per_s", client.handshakes as f64 / client.elapsed_s);
    report.metric("p50_us", client.latencies.percentile_us(50.0));
    report.metric("p99_us", client.latencies.percentile_us(99.0));
    report.metric("cpu_s", client.cpu_s);
    report.work("handshakes", client.handshakes);
    report.work("cipher", negotiated.cipher);
    report.work("group", negotiated.group);
    report.work("tls_library", T::library());
    report
}

pub fn tls_handshake(args: &Args) -> Report {
    let seconds = positive_seconds(args);
    match backend() {
        Backend::Rustls => handshake::<Rustls>(seconds),
        Backend::OpenSsl => handshake::<OpenSsl>(seconds),
    }
}
