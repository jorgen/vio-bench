# Method

Each library does the same work, in the way its own documentation and examples do it. This file
says exactly what that work is. An implementation that does something else is a bug.

## Libraries

| target | version | sockets | files | TLS |
|--------|---------|---------|-------|-----|
| vio (epoll) | pinned commit, see `cpp/vio/CMakeLists.txt` | epoll | vio's thread pool | BoringSSL, bundled |
| vio (io_uring) | same | io_uring readiness | io_uring | BoringSSL, bundled |
| libuv | v1.53.0 | epoll | libuv's thread pool | OpenSSL through memory BIOs; libuv has no TLS |
| libuv (io_uring) | same, `UV_USE_IO_URING=1` | not run | io_uring with SQPOLL | not run |
| asio | 1.38.2, standalone | epoll | io_uring (`asio::random_access_file` needs it) | OpenSSL through `asio::ssl` |
| asio (io_uring) | same, `ASIO_DISABLE_EPOLL` | io_uring | io_uring | OpenSSL through `asio::ssl` |
| tokio | 1.53 | epoll (mio) | Tokio's blocking pool, 4 threads | rustls |
| tokio (openssl) | same | epoll (mio) | not run | OpenSSL through tokio-openssl |

Two kinds of row compare more than event loops, and should be read that way. The file rows
compare io_uring with thread pools: a page-cache read served inside `io_uring_enter` beats any
thread handoff, whichever library makes it. The TLS rows compare BoringSSL, OpenSSL 3 and
rustls with aws-lc as much as they compare loops; `tls_library` in each result names the one
used.

Each implementation's choices that an expert might question are in its `NOTES.md`
(`cpp/vio`, `cpp/libuv`, `cpp/asio`, `rust`). Read them before reading a number.

C++ is built with GCC 14 at `-O3` (`CMAKE_BUILD_TYPE=Release`), without LTO. Rust is built with
`--release`, `lto = "thin"` and `codegen-units = 1`. OpenSSL is the system's; vio brings its
own BoringSSL.

## Rules

1. **One thread per side.** Every workload that has a client and a server runs each on its own
   thread with its own event loop: a `vio::event_loop_t`, a `uv_loop_t`, an `asio::io_context`,
   or a Tokio `current_thread` runtime. Nothing is shared between the two except the address the
   server listens on and, where a server must be told to stop, a flag.
2. **Idiomatic API only.** I/O goes through the library. Setting a socket option through the
   native handle is allowed; reading or writing the socket around the library is not.
3. **Same socket options.** TCP sockets set `TCP_NODELAY` on both ends. UDP sockets set
   `SO_SNDBUF` and `SO_RCVBUF` to 4 MiB. Nothing else is changed from the library's default.
   The kernel silently caps both buffers at `net.core.wmem_max` and `net.core.rmem_max`, so the
   host must allow 4 MiB (`sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=4194304`); the
   runner records both values with the results. TCP uses the host's congestion control, which the
   runner also records, with the default qdisc. Use cubic. With BBR and a qdisc other than `fq`,
   TCP paces itself with a kernel timer; on a 4-vCPU VM that timer took 10 to 11% of `tls_bulk`'s
   CPU, on both epoll and io_uring, which is more than the backend changed.
4. **Loopback only.** Servers bind `127.0.0.1` port 0 and hand the chosen port to the client.
5. **The runner owns the parameters.** Every parameter comes on the command line; a binary has
   no defaults of its own and refuses to run if one is missing.
6. **Warm-up.** Timed workloads (those with `seconds`) ignore the first `min(1 s, seconds / 5)`.
   Counted workloads (those with `bytes` or `count`) time everything.
7. **Report the work.** Every result carries counters that say how much work was done, so the
   runner can check that every library did the same.
8. **Unsupported is an answer.** A library that has no idiomatic way to do a workload exits with
   status 2 and says why on stderr. The runner records that rather than a number.

## Command line

```
<binary> <workload> key=value ...
```

The runner also sets `BENCH_VARIANT` in the environment; the binary copies it into its output.
On success the binary prints exactly one JSON object on one line to stdout:

```json
{"library":"asio","library_version":"1.38.2","variant":"default","workload":"tcp_rr",
 "params":{"connections":"64","size":"64","depth":"1","seconds":"5"},
 "metrics":{"ops_per_s":412345.0,"p50_us":151.2,"p99_us":402.9,"cpu_s":9.81},
 "work":{"requests":2061725,"bytes":131950400}}
```

`params` echoes the arguments as given. `metrics` are numbers to compare; `work` are counters to
check. `cpu_s` is the process's user plus system CPU time over the measured window, so it covers
both sides.

## Workloads

### `tcp_rr`: request and response

Parameters: `connections`, `size`, `depth`, `seconds`.

The client opens `connections` connections. Each keeps `depth` requests of `size` bytes in
flight: it writes `depth` requests, and each time a full `size` bytes of response have arrived it
counts one request done and writes another. Each request is its own write.

The server echoes: it reads up to 64 KiB, writes those bytes back, waits for that write to
finish, then reads again.

Metrics: `ops_per_s` (requests done per second in the window), `p50_us` and `p99_us` (time from a
request's write to the end of its response, client side), `cpu_s`.
Work: `requests`, `bytes` (response bytes the client received in the window).

### `tcp_bulk`: one-way stream

Parameters: `bytes`, `chunk`.

The client writes `bytes` in writes of `chunk` bytes, one write in flight at a time, then shuts
down its sending side, or closes the socket if the library has no half-close (vio). The server
reads with a 64 KiB buffer and discards, until end of stream.
The time runs from the client's first write to the server seeing end of stream.

Metrics: `mib_per_s`, `cpu_s`. Work: `bytes` (what the server received).

### `tls_bulk`: one-way stream over TLS

Parameters: `bytes`, `chunk`.

`tcp_bulk` over TLS 1.3. The server uses `certs/server.pem` and `certs/server.key` (ECDSA
P-256); the client trusts `certs/ca.pem` and checks the name `localhost`. Key exchange is X25519
and the cipher is `TLS_AES_128_GCM_SHA256`, where the library lets you choose; otherwise the
library's default, and the output says which was used. The handshake is not timed. The client
sends `close_notify` at the end; the server stops at `close_notify` or end of stream.

Metrics: `mib_per_s`, `cpu_s`. Work: `bytes`, plus `cipher` and `group` as strings when the
library can report them.

### `tls_handshake`: new connections

Parameters: `seconds`.

The client, one connection at a time: connects, completes a full TLS 1.3 handshake, closes.
Session tickets and session caches are off on both sides, so no handshake resumes. The server
accepts, completes the handshake and closes when the client does. Certificates, group and cipher
as for `tls_bulk`. How the client closes differs by library (sending `close_notify` and moving
on, or waiting for the server's reply); each `NOTES.md` says which, and the difference is about
one loopback round trip on a handshake of one to two milliseconds.

Metrics: `handshakes_per_s`, `p50_us`, `p99_us` (connect start to handshake done, client side),
`cpu_s`. Work: `handshakes`.

### `udp_flood`: datagrams as fast as they go

Parameters: `size`, `seconds`, `batch`.

The sender sends datagrams of `size` bytes as fast as the API allows, for `seconds`. Each starts
with a 64-bit sequence number. With `batch=0` both sides use the library's ordinary per-datagram
calls, awaiting each send when the call is asynchronous. With `batch=N` both sides use the
library's batch calls (`sendmmsg` or segmentation offload to send, `recvmmsg` to receive) with
up to `N` datagrams per call; a library without them exits with status 2. Rule 3 rules out
turning on offloads through the native handle, so a library that does not offer them in its own
API does not get them. The receiver stops once it has seen nothing for 200 ms after the sender
finished.

Metrics: `sent_per_s`, `received_per_s`, `loss_pct`. Work: `sent`, `received`.

### `file_seq`: sequential file I/O

Parameters: `bytes`, `block`.

In the current directory, create a new file and write `bytes` in positional writes of `block`
bytes, one in flight at a time, then `fsync`. Then read the file back the same way. Then delete
it. The read hits the page cache; that is deliberate, since it measures the library and not the
disk.

Metrics: `write_mib_per_s` (writes plus the `fsync`), `read_mib_per_s`. Work: `written`, `read`.

### `file_rand`: random reads

Parameters: `file_bytes`, `block`, `depth`, `seconds`.

Create a file of `file_bytes` (not timed). Then keep `depth` positional reads of `block` bytes in
flight at block-aligned offsets, for `seconds`. Offsets come from xorshift64 seeded with 42:
`x ^= x << 13; x ^= x >> 7; x ^= x << 17; offset = (x % (file_bytes / block)) * block`.

Metrics: `iops`, `p50_us`, `p99_us`. Work: `reads`.

### `timer_accuracy`: how late a sleep wakes

Parameters: `count`, `interval_us`.

Sleep `interval_us`, `count` times in a row, timing each sleep with the steady clock.

Metrics: `p50_late_us`, `p99_late_us`, `max_late_us` (actual minus requested), `early` (sleeps
that returned before their time; this should be 0). Work: `sleeps`.

### `timer_churn`: many timers

Parameters: `count`.

Phase one: arm `count` one-shot timers, timer `i` due `(i * 7919) % 100` ms from now, and wait
until all have fired. Phase two: arm `count` timers due in 60 s, then cancel every one.

Metrics: `fire_cpu_ns` (process CPU time of phase one divided by `count`), `last_late_ms` (how
late the last timer of phase one fired), `arm_cancel_ns` (wall time of phase two divided by
`count`). Work: `fired`, `cancelled`.

### `wake_pingpong`: cross-thread round trip

Parameters: `count`.

Two loops, A and B, on two threads. A posts work to B; that work posts work back to A; that is
one round trip. Repeat `count` times, one at a time. "Post work" means the library's own way to
run a function on another thread's loop: `run_in_loop` in vio, `asio::post` in Asio,
`Handle::spawn` in Tokio, and a mutex-protected queue plus `uv_async_send` in libuv.

Metrics: `round_trips_per_s`, `p50_us`, `p99_us`. Work: `round_trips`.

### `wake_flood`: many posts from another thread

Parameters: `count`.

A plain thread posts `count` small pieces of work to one loop as fast as it can; each increments
a counter. Time runs from the first post to the moment the counter reaches `count`.

Metrics: `posts_per_s`. Work: `posts`.

## Reading the results

Each number in a results table is the median of the runs the runner made, five by default. A
difference under 10% between two libraries on the same machine is not worth a sentence. Loopback
hides the network; these are measures of the library and the kernel's local paths, not of a
link.
