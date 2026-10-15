use crate::bench::{self, Args, Report, UDP_QUIET_NS, UDP_SOCKET_BUFFER, check, fail, unsupported};
use crate::support::{
    OrFail, Window, join_side, loopback, nanos, non_negative, positive, positive_seconds, runtime, spawn_side,
};
use socket2::{Domain, Protocol, Socket, Type};
use std::sync::mpsc::{SyncSender, sync_channel};
use std::time::Duration;
use tokio::net::UdpSocket;
use tokio::time::{Instant, sleep_until};

const RECEIVE_BUFFER: usize = 64 * 1024;
const LARGEST_DATAGRAM: i64 = 65507;
const SEQUENCE_BYTES: usize = std::mem::size_of::<u64>();
const GIVE_UP_AFTER: Duration = Duration::from_secs(10);

#[derive(Default)]
struct SequenceSet {
    words: Vec<u64>,
}

impl SequenceSet {
    fn insert(&mut self, sequence: u64) -> bool {
        let word = (sequence >> 6) as usize;
        if word >= self.words.len() {
            let grown = (word + 1).max(self.words.len() * 2);
            self.words.resize(grown, 0);
        }
        let bit = 1u64 << (sequence & 63);
        let fresh = self.words[word] & bit == 0;
        self.words[word] |= bit;
        fresh
    }

    fn count(&self, first: u64, end: u64) -> i64 {
        let mut total = 0i64;
        let mut sequence = first;
        while sequence < end {
            let word = (sequence >> 6) as usize;
            if word >= self.words.len() {
                break;
            }
            let low = sequence & 63;
            let high = 64.min(low + (end - sequence));
            let mask = if high == 64 { !0u64 << low } else { ((1u64 << high) - 1) & (!0u64 << low) };
            total += (self.words[word] & mask).count_ones() as i64;
            sequence += high - low;
        }
        total
    }
}

fn bound_socket() -> UdpSocket {
    let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP)).or_fail("UDP socket");
    socket.set_send_buffer_size(UDP_SOCKET_BUFFER).or_fail("SO_SNDBUF");
    socket.set_recv_buffer_size(UDP_SOCKET_BUFFER).or_fail("SO_RCVBUF");
    socket.bind(&loopback(0).into()).or_fail("UDP bind");
    socket.set_nonblocking(true).or_fail("O_NONBLOCK");
    UdpSocket::from_std(socket.into()).or_fail("tokio UdpSocket")
}

struct Received {
    seen: SequenceSet,
    duplicates: i64,
    heard: bool,
}

async fn receive(size: usize, seconds: f64, port: SyncSender<u16>) -> Received {
    let socket = bound_socket();
    let give_up_at = Instant::now() + nanos(bench::seconds_ns(seconds)) + GIVE_UP_AFTER;
    port.send(socket.local_addr().or_fail("local address").port()).or_fail("hand the port to the sender");
    let quiet = nanos(UDP_QUIET_NS);
    let mut buffer = vec![0u8; RECEIVE_BUFFER];
    let mut received = Received { seen: SequenceSet::default(), duplicates: 0, heard: false };
    let mut deadline = std::pin::pin!(sleep_until(give_up_at));
    loop {
        let datagram = tokio::select! {
            biased;
            datagram = socket.recv_from(&mut buffer) => datagram,
            () = &mut deadline => return received,
        };
        let (length, _) = datagram.or_fail("udp receive");
        check(length == size, "datagram of the wrong size");
        let sequence = u64::from_ne_bytes(buffer[..SEQUENCE_BYTES].try_into().or_fail("sequence number"));
        if !received.seen.insert(sequence) {
            received.duplicates += 1;
        }
        received.heard = true;
        deadline.as_mut().reset(Instant::now() + quiet);
    }
}

struct Sent {
    first_measured: u64,
    end_sequence: u64,
    elapsed_s: f64,
}

async fn send(port: u16, size: usize, seconds: f64) -> Sent {
    let socket = bound_socket();
    let destination = loopback(port);
    let mut datagram = bench::payload(size);
    let window = Window::start(seconds);
    let mut next_sequence = 0u64;
    let mut first_measured = None;
    loop {
        if window.phase.ended() {
            break;
        }
        if first_measured.is_none() && window.phase.measuring() {
            first_measured = Some(next_sequence);
        }
        datagram[..SEQUENCE_BYTES].copy_from_slice(&next_sequence.to_ne_bytes());
        let length = socket.send_to(&datagram, destination).await.or_fail("udp send");
        check(length == size, "udp send was truncated");
        next_sequence += 1;
    }
    let span = window.finished().await;
    Sent { first_measured: first_measured.unwrap_or(next_sequence), end_sequence: next_sequence, elapsed_s: span.elapsed_s() }
}

pub fn udp_flood(args: &Args) -> Report {
    let size = positive(args, "size");
    let seconds = positive_seconds(args);
    let batch = non_negative(args, "batch");
    check(
        size >= SEQUENCE_BYTES as i64 && size <= LARGEST_DATAGRAM,
        "size must hold a 64-bit sequence number and fit in one datagram",
    );
    if batch > 0 {
        unsupported(
            "tokio::net::UdpSocket has no batch datagram API (no sendmmsg, recvmmsg, GSO or GRO); it moves one datagram per call",
        );
    }
    let size = size as usize;

    let (port, port_ready) = sync_channel(1);
    let receiver = spawn_side("udp-receiver", move || runtime().block_on(receive(size, seconds, port)));
    let port = port_ready.recv().or_fail("the receiver did not start");
    let sent = runtime().block_on(send(port, size, seconds));
    let received = join_side(receiver, "receiver");

    check(
        received.heard,
        &format!(
            "the receiver heard nothing in {} s (seconds + {} s) and gave up",
            seconds + GIVE_UP_AFTER.as_secs_f64(),
            GIVE_UP_AFTER.as_secs()
        ),
    );
    if received.duplicates != 0 {
        fail("received a datagram twice");
    }
    let sent_count = (sent.end_sequence - sent.first_measured) as i64;
    let received_count = received.seen.count(sent.first_measured, sent.end_sequence);
    let mut report = Report::default();
    report.metric("sent_per_s", sent_count as f64 / sent.elapsed_s);
    report.metric("received_per_s", received_count as f64 / sent.elapsed_s);
    report.metric(
        "loss_pct",
        if sent_count > 0 { 100.0 * (sent_count - received_count) as f64 / sent_count as f64 } else { 0.0 },
    );
    report.work("sent", sent_count);
    report.work("received", received_count);
    report
}
