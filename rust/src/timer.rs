use crate::bench::{self, Args, Latencies, Report, check, fail};
use crate::support::{non_negative, positive, runtime};
use std::sync::Arc;
use std::sync::atomic::{AtomicI64, AtomicU64, Ordering};
use std::time::Duration;
use tokio::sync::Notify;
use tokio::time::{Instant, sleep};

const CHURN_SPREAD_MS: i64 = 100;
const CHURN_STRIDE: i64 = 7919;
const CANCELLED_TIMEOUT: Duration = Duration::from_secs(60);

struct Accuracy {
    late: Latencies,
    sleeps: i64,
    early: i64,
}

async fn sleep_repeatedly(count: i64, interval_us: i64) -> Accuracy {
    let interval = Duration::from_micros(interval_us as u64);
    let interval_ns = interval_us * 1000;
    let mut accuracy = Accuracy { late: Latencies::default(), sleeps: 0, early: 0 };
    accuracy.late.reserve(count as usize);
    for _ in 0..count {
        let started = bench::now_ns();
        sleep(interval).await;
        let late = bench::now_ns() - started - interval_ns;
        if late < 0 {
            accuracy.early += 1;
        }
        accuracy.late.add(late);
        accuracy.sleeps += 1;
    }
    accuracy
}

pub fn timer_accuracy(args: &Args) -> Report {
    let count = positive(args, "count");
    let interval_us = non_negative(args, "interval_us");

    let accuracy = runtime().block_on(sleep_repeatedly(count, interval_us));

    let mut report = Report::default();
    report.metric("p50_late_us", accuracy.late.percentile_us(50.0));
    report.metric("p99_late_us", accuracy.late.percentile_us(99.0));
    report.metric("max_late_us", accuracy.late.max_us());
    report.metric("early", accuracy.early as f64);
    report.work("sleeps", accuracy.sleeps);
    report
}

fn churn_due_ms(index: i64) -> i64 {
    (index * CHURN_STRIDE) % CHURN_SPREAD_MS
}

struct Firing {
    count: i64,
    armed_at: Instant,
    fired: AtomicI64,
    last_fired_ns: AtomicI64,
    last_fired_cpu: AtomicU64,
    last_deadline_after_arming_ns: AtomicI64,
    all_fired: Notify,
}

impl Firing {
    fn fire(&self, deadline: Instant) {
        if self.fired.fetch_add(1, Ordering::Relaxed) + 1 < self.count {
            return;
        }
        self.last_fired_ns.store(bench::now_ns(), Ordering::Relaxed);
        self.last_fired_cpu.store(bench::process_cpu_s().to_bits(), Ordering::Relaxed);
        let after_arming = deadline.duration_since(self.armed_at).as_nanos() as i64;
        self.last_deadline_after_arming_ns.store(after_arming, Ordering::Relaxed);
        self.all_fired.notify_one();
    }
}

struct Arming {
    unarmed: AtomicI64,
    all_armed: Notify,
}

impl Arming {
    fn armed_one(&self) {
        if self.unarmed.fetch_sub(1, Ordering::Relaxed) == 1 {
            self.all_armed.notify_one();
        }
    }
}

struct Churn {
    fire_cpu_s: f64,
    last_late_ns: i64,
    arm_cancel_ns: i64,
    fired: i64,
    cancelled: i64,
}

async fn churn(count: i64) -> Churn {
    let firing = Arc::new(Firing {
        count,
        armed_at: Instant::now(),
        fired: AtomicI64::new(0),
        last_fired_ns: AtomicI64::new(0),
        last_fired_cpu: AtomicU64::new(0),
        last_deadline_after_arming_ns: AtomicI64::new(0),
        all_fired: Notify::new(),
    });
    let fire_start_ns = bench::now_ns();
    let fire_start_cpu = bench::process_cpu_s();
    for i in 0..count {
        let timer = sleep(Duration::from_millis(churn_due_ms(i) as u64));
        let firing = Arc::clone(&firing);
        tokio::spawn(async move {
            let deadline = timer.deadline();
            timer.await;
            firing.fire(deadline);
        });
    }
    firing.all_fired.notified().await;
    let fired = firing.fired.load(Ordering::Relaxed);
    check(fired == count, "not every timer fired");
    let fire_cpu_s = f64::from_bits(firing.last_fired_cpu.load(Ordering::Relaxed)) - fire_start_cpu;
    let last_deadline_ns = fire_start_ns + firing.last_deadline_after_arming_ns.load(Ordering::Relaxed);
    let last_late_ns = firing.last_fired_ns.load(Ordering::Relaxed) - last_deadline_ns;

    let arming = Arc::new(Arming { unarmed: AtomicI64::new(count), all_armed: Notify::new() });
    let arm_start_ns = bench::now_ns();
    let timers: Vec<_> = (0..count)
        .map(|_| {
            let arming = Arc::clone(&arming);
            tokio::spawn(async move {
                arming.armed_one();
                sleep(CANCELLED_TIMEOUT).await;
                fail("a cancelled timer fired");
            })
        })
        .collect();
    arming.all_armed.notified().await;
    for timer in &timers {
        timer.abort();
    }
    let mut cancelled = 0i64;
    for timer in timers {
        if timer.await.is_err_and(|error| error.is_cancelled()) {
            cancelled += 1;
        }
    }
    let arm_cancel_ns = bench::now_ns() - arm_start_ns;

    Churn { fire_cpu_s, last_late_ns, arm_cancel_ns, fired, cancelled }
}

pub fn timer_churn(args: &Args) -> Report {
    let count = positive(args, "count");

    let outcome = runtime().block_on(churn(count));

    let mut report = Report::default();
    report.metric("fire_cpu_ns", outcome.fire_cpu_s * 1e9 / count as f64);
    report.metric("last_late_ms", outcome.last_late_ns as f64 / 1e6);
    report.metric("arm_cancel_ns", outcome.arm_cancel_ns as f64 / count as f64);
    report.work("fired", outcome.fired);
    report.work("cancelled", outcome.cancelled);
    report
}
