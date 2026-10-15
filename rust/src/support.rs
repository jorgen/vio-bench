use crate::bench::{self, Args, fail};
use std::fmt::Display;
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};
use std::thread;
use std::time::Duration;
use tokio::runtime::{Builder, Runtime};
use tokio::task::JoinHandle;

pub trait OrFail<T> {
    fn or_fail(self, what: &str) -> T;
}

impl<T, E: Display> OrFail<T> for Result<T, E> {
    fn or_fail(self, what: &str) -> T {
        match self {
            Ok(value) => value,
            Err(error) => fail(&format!("{what}: {error}")),
        }
    }
}

impl<T> OrFail<T> for Option<T> {
    fn or_fail(self, what: &str) -> T {
        match self {
            Some(value) => value,
            None => fail(what),
        }
    }
}

pub fn runtime() -> Runtime {
    Builder::new_current_thread().enable_all().build().or_fail("tokio runtime")
}

pub fn runtime_with_blocking_threads(threads: usize) -> Runtime {
    Builder::new_current_thread().enable_all().max_blocking_threads(threads).build().or_fail("tokio runtime")
}

pub fn spawn_side<T: Send + 'static>(name: &str, side: impl FnOnce() -> T + Send + 'static) -> thread::JoinHandle<T> {
    thread::Builder::new().name(name.to_string()).spawn(side).or_fail("spawn a thread")
}

pub fn join_side<T>(side: thread::JoinHandle<T>, what: &str) -> T {
    side.join().unwrap_or_else(|_| fail(&format!("the {what} thread panicked")))
}

pub fn positive(args: &Args, key: &str) -> i64 {
    let value = args.integer(key);
    if value <= 0 {
        fail(&format!("parameter '{key}' must be positive"));
    }
    value
}

pub fn non_negative(args: &Args, key: &str) -> i64 {
    let value = args.integer(key);
    if value < 0 {
        fail(&format!("parameter '{key}' must not be negative"));
    }
    value
}

pub fn positive_seconds(args: &Args) -> f64 {
    let value = args.real("seconds");
    if value.is_nan() || value <= 0.0 {
        fail("parameter 'seconds' must be positive");
    }
    value
}

pub fn loopback(port: u16) -> SocketAddr {
    SocketAddr::from((Ipv4Addr::LOCALHOST, port))
}

pub fn nanos(ns: i64) -> Duration {
    Duration::from_nanos(ns.max(0) as u64)
}

pub fn mib_per_s(bytes: i64, ns: i64) -> f64 {
    bytes as f64 / (1024.0 * 1024.0) / (ns as f64 / 1e9)
}

pub fn per_second(count: i64, ns: i64) -> f64 {
    count as f64 / (ns as f64 / 1e9)
}

const WARMING_UP: u8 = 0;
const MEASURING: u8 = 1;
const ENDED: u8 = 2;

#[derive(Clone)]
pub struct Phase(Arc<AtomicU8>);

impl Phase {
    pub fn measuring(&self) -> bool {
        self.0.load(Ordering::Relaxed) == MEASURING
    }

    pub fn ended(&self) -> bool {
        self.0.load(Ordering::Relaxed) == ENDED
    }

    fn enter(&self, phase: u8) {
        self.0.store(phase, Ordering::Relaxed);
    }
}

#[derive(Clone, Copy)]
pub struct Span {
    pub start_ns: i64,
    pub end_ns: i64,
    pub start_cpu: f64,
    pub end_cpu: f64,
}

impl Span {
    pub fn elapsed_ns(&self) -> i64 {
        self.end_ns - self.start_ns
    }

    pub fn elapsed_s(&self) -> f64 {
        self.elapsed_ns() as f64 / 1e9
    }

    pub fn cpu_s(&self) -> f64 {
        self.end_cpu - self.start_cpu
    }
}

pub struct Window {
    pub phase: Phase,
    timer: JoinHandle<Span>,
}

impl Window {
    pub fn start(seconds: f64) -> Window {
        let phase = Phase(Arc::new(AtomicU8::new(WARMING_UP)));
        let warmup = bench::warmup_ns(seconds);
        let measured = bench::seconds_ns(seconds) - warmup;
        let flips = phase.clone();
        let timer = tokio::spawn(async move {
            tokio::time::sleep(nanos(warmup)).await;
            let start_ns = bench::now_ns();
            let start_cpu = bench::process_cpu_s();
            flips.enter(MEASURING);
            tokio::time::sleep(nanos(measured)).await;
            let end_ns = bench::now_ns();
            let end_cpu = bench::process_cpu_s();
            flips.enter(ENDED);
            Span { start_ns, end_ns, start_cpu, end_cpu }
        });
        Window { phase, timer }
    }

    pub async fn finished(self) -> Span {
        self.timer.await.or_fail("measurement window")
    }
}
