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
