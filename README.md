# vio-bench

The same I/O work done four ways: with [vio](https://github.com/jorgen/vio),
[libuv](https://libuv.org), [Asio](https://think-async.com/Asio/) and
[Tokio](https://tokio.rs), on the same machine, with the same payloads.

- `METHOD.md` says what each workload does, the rules every implementation follows, and which
  rows compare more than event loops (files: io_uring against thread pools; TLS: BoringSSL,
  OpenSSL and rustls).
- `cpp/vio/NOTES.md`, `cpp/libuv/NOTES.md`, `cpp/asio/NOTES.md` and `rust/NOTES.md` record each
  implementation's choices that an expert might question.
- `results/` holds published runs, one Markdown and one JSON file per machine and date.

## Results

`results/2026-10-20-vm.md` is the first full run: a 4-vCPU cloud VM, Linux 6.18, cubic, five runs
per number, vio at `45f78b1`. Loopback only, so these measure each library and the kernel's local
paths. Differences under 10% are noise here.

Where vio leads:
- TLS bulk: 1,670 MiB/s (epoll) and 1,737 (io_uring), against 1,611 for Tokio with rustls and about
  1,400 for libuv and Asio with OpenSSL.
- UDP: 230k to 263k datagrams a second one at a time, against 150k to 190k for the others; 907k to
  971k with batching, against 203k for libuv's `recvmmsg`. Asio and Tokio have no batch API.
- File reads on io_uring: 528k random 4 KiB reads a second at depth 1 and 806k at depth 32, against
  453k and 456k for Asio on io_uring and 37k and 460k for libuv's io_uring mode.
- Timers: the lowest p99 lateness (415 µs) and the cheapest arm and cancel.

Where vio trails:
- TLS handshakes: 1,190 a second, against 1,610 for Tokio with rustls. Both beat the OpenSSL
  builds (about 600 to 680).
- File work on epoll goes through vio's four-thread pool: 44k random reads a second at depth 32,
  against 79k for libuv and 247k for Tokio. The other file rows on epoll are level with libuv and
  Tokio.
- Posting from another thread: 1.3 million posts a second, against 1.6 to 1.8 million for Asio and
  12 million for libuv, which coalesces every post into one wakeup.

Request and response over TCP is level across the libraries, apart from Asio on io_uring, which is
slower in every row.

## Build

You need GCC 14 or newer, CMake 3.30, Ninja, pkg-config, Go (vio's BoringSSL needs it), the
OpenSSL 3 and liburing development headers, a stable Rust toolchain, and network access: CMake
fetches vio, libuv and Asio with git, and vio fetches its own dependencies.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build
cargo build --release --manifest-path rust/Cargo.toml
```

## Run

The UDP workloads ask for 4 MiB socket buffers, and the kernel quietly caps them unless it is
allowed to give that much. TCP should use cubic: with BBR, and a qdisc other than `fq`, TCP paces
itself with a kernel timer that can cost more CPU than the choice of event loop changes.

```bash
sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=4194304
sudo sysctl -w net.ipv4.tcp_congestion_control=cubic
```

```bash
python3 scripts/run.py                 # every workload, five runs each
python3 scripts/run.py --quick         # tiny parameters, one run, to check it all works
python3 scripts/run.py --only vio,asio --workload tcp_rr,tls_bulk
```

The runner writes `results/<date>-<host>.md` and `.json`, with the machine, the compiler and
build flags, the UDP buffer limits, the TCP congestion control and qdisc, and the load average
during each measurement. Run it on an
idle machine: a busy one moves latency and thread-pool numbers by several times.
