use crate::bench::{self, Args, Latencies, Report, SERVER_READ_BUFFER, check};
use crate::support::{
    OrFail, Phase, Window, join_side, loopback, mib_per_s, positive, positive_seconds, runtime, spawn_side,
};
use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::mpsc::{SyncSender, sync_channel};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::task::JoinSet;

pub async fn listen_on_loopback(port: SyncSender<u16>) -> TcpListener {
    let listener = TcpListener::bind(loopback(0)).await.or_fail("bind");
    let bound = listener.local_addr().or_fail("local address").port();
    port.send(bound).or_fail("hand the port to the client");
    listener
}

pub async fn accept_one(listener: &TcpListener) -> TcpStream {
    let (stream, _) = listener.accept().await.or_fail("accept");
    stream.set_nodelay(true).or_fail("TCP_NODELAY");
    stream
}

pub async fn connect(port: u16) -> TcpStream {
    let stream = TcpStream::connect(loopback(port)).await.or_fail("connect");
    stream.set_nodelay(true).or_fail("TCP_NODELAY");
    stream
}

async fn echo(mut stream: TcpStream) {
    let mut buffer = vec![0u8; SERVER_READ_BUFFER];
    loop {
        let length = match stream.read(&mut buffer).await {
            Ok(0) | Err(_) => return,
            Ok(length) => length,
        };
        if stream.write_all(&buffer[..length]).await.is_err() {
            return;
        }
    }
}

async fn echo_server(connections: i64, port: SyncSender<u16>) {
    let listener = listen_on_loopback(port).await;
    let mut echoes = JoinSet::new();
    for _ in 0..connections {
        echoes.spawn(echo(accept_one(&listener).await));
    }
    drop(listener);
    while echoes.join_next().await.is_some() {}
}

#[derive(Default)]
struct RrCounts {
    requests: i64,
    bytes: i64,
    latencies: Latencies,
}

async fn send_request(stream: &mut TcpStream, request: &[u8], sent_at: &mut VecDeque<i64>) {
    sent_at.push_back(bench::now_ns());
    stream.write_all(request).await.or_fail("request write");
}

async fn rr_connection(mut stream: TcpStream, request: Arc<[u8]>, depth: i64, phase: Phase) -> RrCounts {
    let size = request.len();
    let mut buffer = vec![0u8; SERVER_READ_BUFFER];
    let mut sent_at = VecDeque::with_capacity(depth as usize);
    let mut partial = 0usize;
    let mut counts = RrCounts::default();
    for _ in 0..depth {
        send_request(&mut stream, &request, &mut sent_at).await;
    }
    loop {
        let length = stream.read(&mut buffer).await.or_fail("response read");
        check(length > 0, "the server closed a connection");
        let measuring = phase.measuring();
        if measuring {
            counts.bytes += length as i64;
        }
        partial += length;
        if partial < size {
            continue;
        }
        let now = bench::now_ns();
        while partial >= size {
            partial -= size;
            let sent = sent_at.pop_front().or_fail("response without a request");
            if measuring {
                counts.requests += 1;
                counts.latencies.add(now - sent);
            }
            if !phase.ended() {
                send_request(&mut stream, &request, &mut sent_at).await;
            }
        }
        if phase.ended() {
            return counts;
        }
    }
}

async fn rr_client(port: u16, connections: i64, size: i64, depth: i64, seconds: f64) -> (RrCounts, f64, f64) {
    let mut streams = Vec::with_capacity(connections as usize);
    for _ in 0..connections {
        streams.push(connect(port).await);
    }
    let request: Arc<[u8]> = bench::payload(size as usize).into();
    let window = Window::start(seconds);
    let running: Vec<_> = streams
        .into_iter()
        .map(|stream| tokio::spawn(rr_connection(stream, request.clone(), depth, window.phase.clone())))
        .collect();
    let mut total = RrCounts::default();
    for connection in running {
        let counts = connection.await.or_fail("request/response connection");
        total.requests += counts.requests;
        total.bytes += counts.bytes;
        total.latencies.merge(&counts.latencies);
    }
    let span = window.finished().await;
    (total, span.elapsed_s(), span.cpu_s())
}

pub fn tcp_rr(args: &Args) -> Report {
    let connections = positive(args, "connections");
    let size = positive(args, "size");
    let depth = positive(args, "depth");
    let seconds = positive_seconds(args);

    let (port, port_ready) = sync_channel(1);
    let server = spawn_side("echo-server", move || runtime().block_on(echo_server(connections, port)));
    let port = port_ready.recv().or_fail("the echo server did not start");
    let (counts, elapsed_s, cpu_s) = runtime().block_on(rr_client(port, connections, size, depth, seconds));
    join_side(server, "echo server");

    let mut report = Report::default();
    report.metric("ops_per_s", counts.requests as f64 / elapsed_s);
    report.metric("p50_us", counts.latencies.percentile_us(50.0));
    report.metric("p99_us", counts.latencies.percentile_us(99.0));
    report.metric("cpu_s", cpu_s);
    report.work("requests", counts.requests);
    report.work("bytes", counts.bytes);
    report
}

struct SinkOutcome {
    received: i64,
    end_ns: i64,
    end_cpu: f64,
}

async fn sink_server(port: SyncSender<u16>) -> SinkOutcome {
    let listener = listen_on_loopback(port).await;
    let mut stream = accept_one(&listener).await;
    drop(listener);
    let mut buffer = vec![0u8; SERVER_READ_BUFFER];
    let mut received = 0i64;
    loop {
        match stream.read(&mut buffer).await.or_fail("bulk read") {
            0 => break,
            length => received += length as i64,
        }
    }
    SinkOutcome { received, end_ns: bench::now_ns(), end_cpu: bench::process_cpu_s() }
}

async fn bulk_client(port: u16, bytes: i64, chunk: i64) -> (i64, f64) {
    let mut stream = connect(port).await;
    let data = bench::payload(chunk as usize);
    let start_ns = bench::now_ns();
    let start_cpu = bench::process_cpu_s();
    let mut sent = 0i64;
    while sent < bytes {
        let length = chunk.min(bytes - sent);
        stream.write_all(&data[..length as usize]).await.or_fail("bulk write");
        sent += length;
    }
    stream.shutdown().await.or_fail("shutdown");
    (start_ns, start_cpu)
}

pub fn tcp_bulk(args: &Args) -> Report {
    let bytes = positive(args, "bytes");
    let chunk = positive(args, "chunk");

    let (port, port_ready) = sync_channel(1);
    let server = spawn_side("sink-server", move || runtime().block_on(sink_server(port)));
    let port = port_ready.recv().or_fail("the sink server did not start");
    let (start_ns, start_cpu) = runtime().block_on(bulk_client(port, bytes, chunk));
    let sink = join_side(server, "sink server");

    let mut report = Report::default();
    report.metric("mib_per_s", mib_per_s(sink.received, sink.end_ns - start_ns));
    report.metric("cpu_s", sink.end_cpu - start_cpu);
    report.work("bytes", sink.received);
    report
}
