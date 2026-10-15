# vio-bench

The same I/O work done four ways: with [vio](https://github.com/jorgen/vio),
[libuv](https://libuv.org), [Asio](https://think-async.com/Asio/) and
[Tokio](https://tokio.rs), on the same machine, with the same payloads.

`METHOD.md` says what each workload does and the rules every implementation follows.
`results/` holds measured runs, one Markdown and one JSON file per machine and date.

## Build

Needs GCC 14 or newer, CMake 3.30, Ninja, Go (for vio's BoringSSL), OpenSSL and liburing
headers, and a stable Rust toolchain.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cargo build --release --manifest-path rust/Cargo.toml
```

## Run

```bash
python3 scripts/run.py                 # every workload, five runs each
python3 scripts/run.py --quick         # tiny parameters, one run, to check it all works
python3 scripts/run.py --only vio,asio --workload tcp_rr,tls_bulk
```

The runner writes `results/<date>-<host>.md` and `.json`. Run it on an idle machine.
