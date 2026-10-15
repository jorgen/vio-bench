use crate::bench::{self, Args, Latencies, Report, Xorshift64, check};
use crate::support::{OrFail, Window, mib_per_s, positive, positive_seconds, runtime_with_blocking_threads};
use std::io;
use std::os::unix::fs::FileExt;
use std::sync::Arc;
use tokio::fs::{File, OpenOptions};
use tokio::io::AsyncWriteExt;
use tokio::task::{JoinSet, spawn_blocking};

const FILL_BLOCK: i64 = 1024 * 1024;
const LIBUV_DEFAULT_POOL_THREADS: usize = 4;

type SharedFile = Arc<std::fs::File>;

fn scratch_path(workload: &str) -> String {
    format!("bench_tokio_{workload}_{}.tmp", std::process::id())
}

async fn create(path: &str) -> File {
    OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(path)
        .await
        .or_fail(&format!("create {path}"))
}

async fn remove(path: &str) {
    tokio::fs::remove_file(path).await.or_fail(&format!("remove {path}"));
}

async fn write_at(file: &SharedFile, data: &Arc<[u8]>, length: usize, offset: i64) -> i64 {
    let file = Arc::clone(file);
    let data = Arc::clone(data);
    let written = spawn_blocking(move || file.write_at(&data[..length], offset as u64))
        .await
        .or_fail("write task")
        .or_fail("pwrite");
    check(written > 0, "a write made no progress");
    written as i64
}

async fn read_at(file: &SharedFile, mut buffer: Vec<u8>, length: usize, offset: i64) -> (Vec<u8>, i64) {
    let file = Arc::clone(file);
    let (buffer, result) = spawn_blocking(move || {
        let result = file.read_at(&mut buffer[..length], offset as u64);
        (buffer, result)
    })
    .await
    .or_fail("read task");
    let got = result.or_fail("pread");
    check(got > 0, "the file ended before everything written was read back");
    (buffer, got as i64)
}

async fn sync_all(file: &SharedFile) {
    let file = Arc::clone(file);
    spawn_blocking(move || file.sync_all()).await.or_fail("fsync task").or_fail("fsync");
}

struct Sequential {
    written: i64,
    read: i64,
    write_ns: i64,
    read_ns: i64,
}

async fn sequential(bytes: i64, block: i64) -> Sequential {
    let path = scratch_path("file_seq");
    let file: SharedFile = Arc::new(create(&path).await.into_std().await);
    let data: Arc<[u8]> = bench::payload(block as usize).into();

    let write_start = bench::now_ns();
    let mut written = 0i64;
    while written < bytes {
        let length = block.min(bytes - written) as usize;
        written += write_at(&file, &data, length, written).await;
    }
    sync_all(&file).await;
    let write_ns = bench::now_ns() - write_start;

    let mut buffer = vec![0u8; block as usize];
    let read_start = bench::now_ns();
    let mut read = 0i64;
    while read < bytes {
        let length = block.min(bytes - read) as usize;
        let (returned, got) = read_at(&file, buffer, length, read).await;
        buffer = returned;
        read += got;
    }
    let read_ns = bench::now_ns() - read_start;

    drop(file);
    remove(&path).await;
    Sequential { written, read, write_ns, read_ns }
}

pub fn file_seq(args: &Args) -> Report {
    let bytes = positive(args, "bytes");
    let block = positive(args, "block");

    let outcome = runtime_with_blocking_threads(LIBUV_DEFAULT_POOL_THREADS).block_on(sequential(bytes, block));

    let mut report = Report::default();
    report.metric("write_mib_per_s", mib_per_s(outcome.written, outcome.write_ns));
    report.metric("read_mib_per_s", mib_per_s(outcome.read, outcome.read_ns));
    report.work("written", outcome.written);
    report.work("read", outcome.read);
    report.work("blocking_threads", LIBUV_DEFAULT_POOL_THREADS as i64);
    report
}

async fn fill(file: &mut File, total: i64) {
    let pattern = bench::payload(FILL_BLOCK.min(total) as usize);
    let mut written = 0i64;
    while written < total {
        let length = (pattern.len() as i64).min(total - written) as usize;
        file.write_all(&pattern[..length]).await.or_fail("fill the file");
        written += length as i64;
    }
    file.flush().await.or_fail("fill the file");
}

type ReadDone = (Vec<u8>, i64, io::Result<usize>);

fn start_read(reads: &mut JoinSet<ReadDone>, file: &SharedFile, mut buffer: Vec<u8>, offset: u64) {
    let file = Arc::clone(file);
    let started = bench::now_ns();
    reads.spawn_blocking(move || {
        let result = file.read_at(&mut buffer, offset);
        (buffer, started, result)
    });
}

struct Random {
    reads: i64,
    latencies: Latencies,
    elapsed_s: f64,
}

async fn random(file_bytes: i64, block: i64, depth: i64, seconds: f64) -> Random {
    let path = scratch_path("file_rand");
    let mut file = create(&path).await;
    fill(&mut file, file_bytes).await;
    let file: SharedFile = Arc::new(file.into_std().await);

    let blocks = (file_bytes / block) as u64;
    let mut offsets = Xorshift64::default();
    let mut next_offset = || (offsets.next() % blocks) * block as u64;
    let window = Window::start(seconds);
    let mut in_flight = JoinSet::new();
    for _ in 0..depth {
        start_read(&mut in_flight, &file, vec![0u8; block as usize], next_offset());
    }
    let mut reads = 0i64;
    let mut latencies = Latencies::default();
    while let Some(done) = in_flight.join_next().await {
        let now = bench::now_ns();
        let (buffer, started, result) = done.or_fail("read task");
        let length = result.or_fail("pread");
        check(length == block as usize, "a random read came back short");
        if window.phase.measuring() {
            reads += 1;
            latencies.add(now - started);
        }
        if !window.phase.ended() {
            start_read(&mut in_flight, &file, buffer, next_offset());
        }
    }
    let span = window.finished().await;

    drop(file);
    remove(&path).await;
    Random { reads, latencies, elapsed_s: span.elapsed_s() }
}

pub fn file_rand(args: &Args) -> Report {
    let file_bytes = positive(args, "file_bytes");
    let block = positive(args, "block");
    let depth = positive(args, "depth");
    let seconds = positive_seconds(args);
    check(file_bytes >= block, "file_bytes must hold at least one block");

    let outcome =
        runtime_with_blocking_threads(LIBUV_DEFAULT_POOL_THREADS).block_on(random(file_bytes, block, depth, seconds));

    let mut report = Report::default();
    report.metric("iops", outcome.reads as f64 / outcome.elapsed_s);
    report.metric("p50_us", outcome.latencies.percentile_us(50.0));
    report.metric("p99_us", outcome.latencies.percentile_us(99.0));
    report.work("reads", outcome.reads);
    report.work("blocking_threads", LIBUV_DEFAULT_POOL_THREADS as i64);
    report
}
