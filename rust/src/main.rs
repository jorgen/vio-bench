mod bench;
mod file;
mod support;
mod tcp;
mod timer;
mod tls;
mod udp;
mod wake;

use bench::Workload;
use std::collections::BTreeMap;

fn main() {
    let workloads: BTreeMap<&str, Workload> = BTreeMap::from([
        ("tcp_rr", tcp::tcp_rr as Workload),
        ("tcp_bulk", tcp::tcp_bulk),
        ("tls_bulk", tls::tls_bulk),
        ("tls_handshake", tls::tls_handshake),
        ("udp_flood", udp::udp_flood),
        ("file_seq", file::file_seq),
        ("file_rand", file::file_rand),
        ("timer_accuracy", timer::timer_accuracy),
        ("timer_churn", timer::timer_churn),
        ("wake_pingpong", wake::wake_pingpong),
        ("wake_flood", wake::wake_flood),
    ]);
    std::process::exit(bench::run("tokio", env!("BENCH_TOKIO_VERSION"), &workloads));
}
