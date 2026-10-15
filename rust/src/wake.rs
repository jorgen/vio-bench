use crate::bench::{self, Args, Latencies, Report, check};
use crate::support::{OrFail, join_side, per_second, positive, runtime, spawn_side};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::mpsc::sync_channel;
use tokio::runtime::Handle;
use tokio::sync::{Notify, oneshot};

struct PingPong {
    a: Handle,
    b: Handle,
    count: i64,
    done: i64,
    posted_ns: i64,
    started_ns: i64,
    finished_ns: i64,
    latencies: Latencies,
    finished: Option<oneshot::Sender<Box<PingPong>>>,
}

fn round_trip(mut game: Box<PingPong>) {
    game.posted_ns = bench::now_ns();
    let b = game.b.clone();
    b.spawn(async move {
        let a = game.a.clone();
        a.spawn(async move { returned(game) });
    });
}

fn returned(mut game: Box<PingPong>) {
    let now = bench::now_ns();
    game.latencies.add(now - game.posted_ns);
    game.done += 1;
    if game.done < game.count {
        round_trip(game);
        return;
    }
    game.finished_ns = now;
    if let Some(finished) = game.finished.take() {
        let _ = finished.send(game);
    }
}

pub fn wake_pingpong(args: &Args) -> Report {
    let count = positive(args, "count");

    let (b_handle, b_ready) = sync_channel(1);
    let (stop_b, b_stopped) = oneshot::channel::<()>();
    let b_side = spawn_side("loop-b", move || {
        let b = runtime();
        b_handle.send(b.handle().clone()).or_fail("hand loop B to loop A");
        b.block_on(async {
            let _ = b_stopped.await;
        });
    });
    let b = b_ready.recv().or_fail("loop B did not start");

    let game = runtime().block_on(async move {
        let (finished, all_returned) = oneshot::channel();
        let mut latencies = Latencies::default();
        latencies.reserve(count as usize);
        let game = Box::new(PingPong {
            a: Handle::current(),
            b,
            count,
            done: 0,
            posted_ns: 0,
            started_ns: bench::now_ns(),
            finished_ns: 0,
            latencies,
            finished: Some(finished),
        });
        round_trip(game);
        all_returned.await.or_fail("the round trips did not finish")
    });
    let _ = stop_b.send(());
    join_side(b_side, "loop B");

    let mut report = Report::default();
    report.metric("round_trips_per_s", per_second(game.done, game.finished_ns - game.started_ns));
    report.metric("p50_us", game.latencies.percentile_us(50.0));
    report.metric("p99_us", game.latencies.percentile_us(99.0));
    report.work("round_trips", game.done);
    report
}

struct Flood {
    count: AtomicI64,
    done: AtomicI64,
    finished_ns: AtomicI64,
    all_ran: Notify,
}

static FLOOD: Flood = Flood {
    count: AtomicI64::new(0),
    done: AtomicI64::new(0),
    finished_ns: AtomicI64::new(0),
    all_ran: Notify::const_new(),
};

fn increment() {
    if FLOOD.done.fetch_add(1, Ordering::Relaxed) + 1 == FLOOD.count.load(Ordering::Relaxed) {
        FLOOD.finished_ns.store(bench::now_ns(), Ordering::Relaxed);
        FLOOD.all_ran.notify_one();
    }
}

pub fn wake_flood(args: &Args) -> Report {
    let count = positive(args, "count");
    FLOOD.count.store(count, Ordering::Relaxed);

    let (target_handle, target_ready) = sync_channel(1);
    let target = spawn_side("loop", move || {
        runtime().block_on(async move {
            target_handle.send(Handle::current()).or_fail("hand the loop to the poster");
            FLOOD.all_ran.notified().await;
        });
    });
    let handle = target_ready.recv().or_fail("the loop did not start");

    let poster = spawn_side("poster", move || {
        let started_ns = bench::now_ns();
        for _ in 0..count {
            handle.spawn(async { increment() });
        }
        started_ns
    });
    let started_ns = join_side(poster, "poster");
    join_side(target, "loop");

    let posts = FLOOD.done.load(Ordering::Relaxed);
    check(posts == count, "not every post ran");
    let mut report = Report::default();
    report.metric("posts_per_s", per_second(posts, FLOOD.finished_ns.load(Ordering::Relaxed) - started_ns));
    report.work("posts", posts);
    report
}
