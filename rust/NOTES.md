# Tokio: notes

What this implementation does that you need to know to read its numbers. It uses Tokio 1.53.3 on
mio 1.2.4. TLS goes through rustls 0.23.46 on aws-lc-rs 1.18.2 (tokio-rustls 0.26.6), or through
the system's OpenSSL (3.0.13 here) with tokio-openssl 0.6.5. rustc 1.94.1 builds it with
`--release`, thin LTO and `codegen-units = 1`, and no `target-cpu` flag; aws-lc and OpenSSL pick
their AES-GCM code at run time.

One binary serves two targets:

- `tokio`: every workload, with TLS through rustls (`BENCH_TLS=rustls`).
- `tokio (openssl)`: only `tls_bulk` and `tls_handshake`, through OpenSSL (`BENCH_TLS=openssl`).

## All workloads

- Every loop is a `current_thread` runtime from `Builder::new_current_thread().enable_all()`,
  which is what `#[tokio::main(flavor = "current_thread")]` builds. No multi-thread runtime is
  used. The I/O and time drivers run even where a workload does no socket I/O, so every loop
  parks in `epoll_wait`, in the timer, file and wake rows too.
- Where a workload has two sides, the client runs on the main thread and the server on a thread
  of its own, each with its own runtime. The server hands over its port through a
  `std::sync::mpsc` channel.
- A task of its own opens and closes the window with `tokio::time::sleep`. Tokio's timers count
  whole milliseconds (see `timer_accuracy`), so an edge can land about 1 ms late. Rates divide by
  the steady-clock time read at the two edges, and `cpu_s` is `getrusage` read there too, so a
  late edge does not skew a rate.
- Tokio makes a task yield once it has done 128 operations in one poll (its cooperative
  budget). The loops here run under that budget, as any Tokio code does. Only phase two of
  `timer_churn` turns it off, and says why.
- TCP sockets set `TCP_NODELAY` with `TcpStream::set_nodelay`. Listeners keep mio's defaults:
  `SO_REUSEADDR` and a backlog of 128.
- Any error fails the run, except on a connection the peer is closing.

## tcp_rr

- The client connects one socket at a time, then starts the window and spawns one task per
  connection on its runtime.
- Each request is its own `write_all`, awaited where it is issued: `depth` of them up front, then
  one in the read loop each time a response completes. The asio and vio clients have the same
  inline shape. Latency runs from just before the write to the end of the read that completes
  the response; responses completed by one read share its end time.
- Since writes are inline, client and server can both block in writes once `depth x size`
  outgrows the socket buffers. The asio and vio clients share this limit; libuv's queued writes
  do not. On this machine 4 MiB in flight on one connection runs and 16 MiB hangs. The
  configured sets keep at most 16 KiB in flight per connection.
- The echo server spawns one task per connection: `read` up to 64 KiB, `write_all` it back,
  read again. It takes end of stream or any error as a close.
- When the window ends, each connection task returns after its next completed response and drops
  its socket.
- `bytes` counts every response byte read inside the window, and `requests` every response
  completed inside it. A response split by a window edge can make `bytes` differ from
  `requests x size`, and the runner then reports a work mismatch.

## tcp_bulk

- One `write_all` per chunk, each awaited before the next, then `shutdown()`, which shuts down
  the send side, then close.
- The server reads into one 64 KiB buffer until EOF.
- Time runs from just before the first write to the server's EOF. `cpu_s` is process CPU over
  that span.

## TLS: shared by tls_bulk and tls_handshake

- `BENCH_TLS` picks the library. A TLS workload refuses to run without it, or when
  `BENCH_VARIANT` names the other library. Nothing falls back to a default.
- rustls: `aws_lc_rs::default_provider()` cut down to `TLS13_AES_128_GCM_SHA256` and `X25519`,
  passed in with `builder_with_provider`, TLS 1.3 only. Left alone, rustls would offer
  X25519MLKEM768 first; this provider offers only X25519. The server sets
  `send_tls13_tickets = 0` and `NoServerSessionStorage`; the client sets
  `Resumption::disabled()`. The client checks the chain against `certs/ca.pem` and the name
  `localhost` with rustls's own WebPKI verifier. All else is rustls's default, including 16 KiB
  records.
- OpenSSL: the `SSL_CTX` is built through the `openssl` crate with the same calls as the libuv
  program: TLS 1.3 only, `TLS_AES_128_GCM_SHA256`, `X25519`, session cache off. The server sets
  `num_tickets` to 0 and `SSL_OP_NO_TICKET`. The client verifies the chain against
  `certs/ca.pem`, checks the host `localhost` and sends it as SNI.
- Both OpenSSL contexts also set `read_ahead`, which the libuv and asio programs do not.
  tokio-openssl hands OpenSSL the socket through a BIO, and without `read_ahead` OpenSSL reads
  each record in two `recv` calls: the 5-byte header, then the body. With it OpenSSL fills its
  16.7 KiB buffer in one call, about one `recv` per record (1,028 for 16 MiB here). libuv and asio
  feed OpenSSL through BIO pairs from their own large socket reads, so they never split a record.
- Writes differ by library. OpenSSL sends each 16 KiB record with its own `send`, four per
  64 KiB chunk. rustls sends the four records of a chunk in one `writev`. Both read about one
  record per `recv`.
- The client reads the cipher and group after the handshake. rustls calls the suite
  `TLS13_AES_128_GCM_SHA256`; the output uses the IANA name `TLS_AES_128_GCM_SHA256`, as
  OpenSSL does. The `openssl` crate has no call for the group, so it comes from
  `SSL_ctrl(SSL_CTRL_GET_NEGOTIATED_GROUP)`. `tls_library` in `work` names the library and its
  version.

### tls_bulk

- The handshake ends before the clock starts. Then one `write_all` per chunk, then `shutdown()`,
  which sends `close_notify` and then shuts down the TCP send side.
- The server reads into a 64 KiB buffer and stops at `close_notify` (a read of 0), or at EOF
  without one (`UnexpectedEof`), as METHOD allows. That ends the clock. It then drops the
  connection without a `close_notify` of its own.

### tls_handshake

- Each round: TCP connect, full handshake, `shutdown()`, close. `shutdown()` sends
  `close_notify` and half-closes the TCP socket; the client does not wait for the server.
  Latency runs from just before the TCP connect to the end of the client's handshake.
- The server spawns a task per connection. It completes the handshake, reads until the client's
  `close_notify` or EOF, and drops the connection without sending a `close_notify` of its own,
  as libuv does.

## udp_flood

- The sockets are made with `socket2`, which sets `SO_SNDBUF` and `SO_RCVBUF` to 4 MiB before
  bind, and then go to `tokio::net::UdpSocket::from_std`. Tokio's `UdpSocket` cannot set buffer
  sizes itself.
- `batch=0`: one awaited `send_to` per datagram; one `recv_from` per datagram into a 64 KiB
  buffer.
- `batch=32`: exits 2. Tokio has no `sendmmsg`, `recvmmsg`, GSO or GRO API.
- The receiver races `recv_from` against one `Sleep` with `select!`, biased to the receive. The
  `Sleep` starts as a give-up deadline at `seconds` + 10 s, as in vio and libuv. After each
  datagram it moves to 200 ms from now. Moving a deadline later is a compare-and-swap on the
  timer's state; Tokio does not touch the timer wheel for each datagram.
- If nothing arrives by the give-up deadline, the run fails with "the receiver heard nothing in
  N s (seconds + 10 s) and gave up".
- The receiver does not know when the sender stops. It stops after 200 ms with no datagram, as
  the libuv and asio receivers do. A sender stalled for 200 ms mid-run would end the receive
  early, and the rest would count as loss. vio's receiver never stops before its first datagram
  plus `seconds`, so such a stall does not end it.
- `sent` and `received` count only sequence numbers sent inside the window, and both rates
  divide by the sender's window. A duplicate or a datagram of the wrong size fails the run.

## file_seq and file_rand

- The runtime is `current_thread` with `max_blocking_threads(4)`: four pool threads, the size of
  libuv's default thread pool (`UV_THREADPOOL_SIZE`). Both rows report it in `work` as
  `blocking_threads=4`.
- `METHOD.md` fixes the pool at four threads. Tokio's default cap is 512: a pool thread that has
  just finished a read is not yet idle when the loop queues the next one, so the pool starts a
  new thread. At `depth=32` on a 4-CPU machine the default pool grew to all 512 threads and gave
  about a fifth of the IOPS that four threads gave. At `depth=1` the cap makes no difference.
- Stable Tokio has no async file I/O on Linux. Each read or write is a `spawn_blocking` job: a
  handoff to a pool thread, then a wake back to the loop through its eventfd.
- `tokio::fs::File` has no positional read or write, and its `write` copies the buffer and
  returns before the write is done. So the timed I/O runs std's `FileExt::write_at` and
  `read_at` on an `Arc<std::fs::File>` inside `spawn_blocking`, the "std::fs inside
  spawn_blocking" form that the `tokio::fs` docs give. Creating the file, filling it for
  `file_rand` in 1 MiB writes with no `fsync`, and removing it go through `tokio::fs`, outside
  the clock. The file is `bench_tokio_<workload>_<pid>.tmp` in the working directory.
- Tokio 1.53 has an io_uring path for some `tokio::fs` calls, behind `--cfg tokio_unstable` and
  the `io-uring` feature. This build does not use it.

### file_seq

- One `spawn_blocking` per block, awaited before the next. The offset is the bytes done so far,
  so a short write or read carries on from where it stopped.
- The `fsync` is `sync_all` in `spawn_blocking`, inside the write clock.

### file_rand

- One task keeps `depth` reads in flight with `JoinSet::spawn_blocking`. It takes offsets from
  the one xorshift64 sequence in the order it issues reads. Each buffer moves into its job and
  back, and is reused.
- Latency runs from just before `spawn_blocking` to when the loop takes the result from the
  `JoinSet`. At `depth=32` up to 28 reads wait in the pool's queue for one of the four threads,
  and that wait counts.

## timer_accuracy

- `tokio::time::sleep(interval)` in a loop, timed with the steady clock.
- Tokio's timer wheel counts whole milliseconds and rounds each deadline up to the next tick.
  A sleep that starts just after a tick ends at the tick after next, and mio rounds the
  `epoll_wait` timeout up to whole milliseconds as well. So a 1 ms sleep takes about 2 ms, and
  `p50_late_us` sits near 1,100. That is the floor of Tokio's timer, not noise.

## timer_churn

- Phase one spawns one task per timer, each awaiting its own `tokio::time::sleep`. It does not
  use `tokio_util::time::DelayQueue`: each timer has its own action, as in the other libraries,
  and this form uses the runtime's own timer wheel. tokio-util is not a dependency.
- Each `Sleep` is made in the arming loop, so its deadline counts from when it was armed. Tokio
  puts it in the wheel at its task's first poll, after the loop ends.
- `fire_cpu_ns` includes spawning, polling and finishing each task, as libuv's includes
  `uv_timer_init` and `uv_timer_start`. At count=100000 that is about 1.5 us of CPU per timer.
  Each deadline counts from its own arming, so time spent in the arming loop pushes later
  deadlines out too. The last timer fires a few milliseconds late on a quiet machine, and later
  under load.
- `last_late_ms` is the time the last timer fired minus that timer's own deadline, as in asio.
  libuv and vio subtract the latest deadline armed. Both deadlines fall in the last 1 ms tick.
- Phase two arms a timer by making `Box::pin(sleep(60 s))` and polling it once, since Tokio
  puts a `Sleep` in the wheel only at its first poll. The polls run inside
  `tokio::task::coop::unconstrained`: once the budget of 128 is spent, a `Sleep` returns
  `Pending` without going into the wheel.
- A timer is cancelled by dropping its `Sleep`, Tokio's documented way. The drop takes it out of
  the wheel at once.
- The clock covers making, arming and dropping every timer, one `Box` each included. The `Vec`
  that holds them is reserved before the clock starts. `cancelled` counts the sleeps that had
  not fired when dropped.

## wake_pingpong and wake_flood

- The posting primitive is `Handle::spawn`, as METHOD names it. From another thread, each post
  allocates a task, adds it to the runtime's task list and injection queue, each under a lock,
  and writes the runtime's eventfd to wake its `epoll_wait`. Every post writes the eventfd;
  nothing coalesces. `strace` counts 20,001 eventfd writes for 20,000 posts.
- The vio, libuv and asio loops wake once for many posts, so the Tokio rows price a different
  mechanism.
- `wake_pingpong`: a round trip is two posts, so two tasks and two eventfd writes. The state is
  a `Box` moved through the posted futures. Each round trip clones both `Handle`s, an `Arc`
  each, since `Handle::spawn` takes `&self`. The rate is `count` over the time from the first
  post to the last return.
- `wake_flood`: a plain `std::thread` calls `Handle::spawn` `count` times and drops each
  `JoinHandle`. It starts once the loop runs. Time runs from its first post to the task that
  brings the counter to `count`. The counter is a static atomic, so the posted future captures
  nothing. Each post costs the poster at least one syscall, the eventfd write.
