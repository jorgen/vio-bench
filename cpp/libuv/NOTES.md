# libuv: notes

What this implementation does that you need to know to read its numbers. It uses libuv v1.53.0,
fetched and linked statically, and the system's OpenSSL (3.0.13 here) for TLS.

There are two targets:

- `libuv`: epoll for sockets, libuv's thread pool for files. Runs every workload.
- `libuv (io_uring)`: `UV_USE_IO_URING=1`. Runs only `file_seq` and `file_rand`.

## All workloads

- Where a workload has two sides, each runs its own `uv_loop_t` on its own thread.
- `seconds` is the whole run. The first `min(1 s, seconds / 5)` is warm-up, so a 5 s run measures
  4 s. A `uv_timer_t` turns measuring on and off. Both lengths round up to whole milliseconds,
  since libuv timers take milliseconds. Rates divide by the steady-clock time between the two
  timer callbacks, and `cpu_s` is `getrusage` read in the same callbacks, so a late timer does not
  skew a rate.
- libuv's loop clock counts whole milliseconds and is cached for each turn of the loop.
- On this kernel libuv batches `epoll_ctl` add and modify calls through a small io_uring (256
  entries, no SQPOLL) in both targets, so `strace` shows `io_uring_enter` where you expect
  `epoll_ctl`. Sockets still wait in `epoll_wait`.
- Any libuv or OpenSSL error fails the run, except errors on a connection the program is closing.

## tcp_rr

- The client issues each request as its own `uv_write`, with a `uv_write_t` from a per-connection
  pool. Latency runs from just before that `uv_write` to the read callback that completes the
  response. Responses match requests in order, per connection.
- All connections are up before warm-up starts.
- The echo server writes with `uv_try_write` from the read callback, as Node.js does. If every
  byte goes, the write is done and the read stays armed. On `UV_EAGAIN` or a short write it calls
  `uv_read_stop`, sends the rest with `uv_write` from the same 64 KiB buffer, and calls
  `uv_read_start` from the write callback. Either way it reads up to 64 KiB, writes it back, waits
  for that write, then reads again, as METHOD asks. Stopping and restarting the read on every echo
  would change the epoll registration on every request.
- `bytes` counts every response byte read inside the window, and `requests` every response
  completed inside it. A response split by a window edge counts in one but not fully in the
  other, so `bytes` can differ from `requests x size`, and the runner then reports a work
  mismatch.
- When the window ends the client closes its sockets with `uv_close`, with responses still in
  flight. The server takes end of stream or any error as a close.

## tcp_bulk

- One `uv_write` per chunk; the write callback issues the next. Then `uv_shutdown`, then
  `uv_close`.
- The server reads into one 64 KiB buffer.
- Time runs from the connect callback, just before the first `uv_write`, to the server's `UV_EOF`.
  `cpu_s` is process CPU over that span.

## TLS: shared by tls_bulk and tls_handshake

libuv has no TLS, so the program drives OpenSSL itself:

- Each connection is an `SSL` over a BIO pair (`BIO_new_bio_pair`): two in-memory rings, the same
  mechanism `asio::ssl` uses. OpenSSL never sees the socket.
- Receive: the alloc callback hands libuv free space in the pair's 64 KiB incoming ring
  (`BIO_nwrite0`), and the read callback commits it (`BIO_nwrite`). Ciphertext lands in the ring
  with no copy of its own. `SSL_read` then decrypts into a 64 KiB plaintext buffer.
- Send: `SSL_write` encrypts into a 128 KiB outgoing ring. A flush copies the records out into a
  pooled buffer and hands them to one `uv_write`. A 64 KiB chunk makes 65,624 bytes of records,
  so it leaves in one `uv_write`. If the ring fills, the program flushes and calls `SSL_write`
  again.
- TLS 1.3 only, `TLS_AES_128_GCM_SHA256` and X25519, set on both sides. The client checks
  `certs/ca.pem` and the name `localhost`, and sends the name as SNI. Tickets and session caches
  are off on both sides.
- In `tls_handshake`, `SSL_do_handshake` takes about 98% of the loop's instructions (callgrind),
  so that row measures OpenSSL 3.0 more than libuv. Compare it with `asio` and `tokio (openssl)`,
  which use the same library. `tls_library` in the output names it.

### tls_bulk

- Timing starts when the client's handshake is done and its Finished has gone to `uv_write`, just
  before the first `SSL_write`. That 64-byte Finished write may still be in flight while the first
  chunk goes out. After that, one chunk is in flight at a time.
- Timing ends when the server sees `close_notify` or EOF.
- The client ends with `SSL_shutdown`, `uv_shutdown` and `uv_close`. The server closes on
  `close_notify` without sending its own.

### tls_handshake

- Latency runs from just before `SSL_new` (which builds the BIO pair) and the TCP connect, to the
  moment the client's handshake is done and its Finished has gone to `uv_write`.
- The client sends `close_notify` and moves on: `SSL_shutdown`, `uv_shutdown`, `uv_close`. It
  does not wait for a reply. The next connect starts from the close callback, so
  `handshakes_per_s` includes the close.
- Each connection allocates, on each side, the two rings (64 KiB and 128 KiB) and a 64 KiB
  plaintext buffer, none zeroed. The client's share is inside the latency.
- Server: accepts, sets `TCP_NODELAY`, completes the handshake and reads until the client's
  `close_notify` or end of stream, then closes the socket without sending a `close_notify` of
  its own. It builds its `SSL_CTX` once, before it listens.
- When the client loop ends, the main thread stops the server with a `uv_async_t`, outside the
  window.

## udp_flood

- Sockets are not connected; every send carries the receiver's address, as in the other
  implementations.
- No GSO or GRO: libuv has no API for them, and METHOD forbids turning them on through the socket.
  vio's batch row sends with GSO, so the two batch rows compare `sendmmsg` with GSO.

**batch=0.** The sender calls `uv_udp_send` and issues the next one from its send callback, so
one datagram is in flight. libuv queues a send made from a send callback instead of sending it at
once, so it goes out on a later pass of the loop. Each datagram still costs one `sendmsg`, and
the loop sends several per `epoll_wait`. The receiver uses `uv_udp_recv_start`: one `recvmsg` per
datagram.

**batch=32.** The sender calls `uv_udp_try_send2` (`sendmmsg`) from a `uv_idle_t` callback, one
batch per turn of the loop. The receiver uses `UV_UDP_RECVMMSG`. libuv caps both sides at 20
datagrams per call: `sendmmsg` fills a 20-entry array, and `recvmmsg` reads into at most 20
slots. So a batch of 32 is two `sendmmsg` calls (20 and 12), and no receive takes more than 20.
The receive buffer is 32 x 64 KiB, because libuv wants a 64 KiB slot per datagram, but it uses
only 20 of them.

**The `uv_udp_try_send2` bug in v1.53.0.** Given more than 20 datagrams, it sends only the first
20 and returns 20. Given more than 40, it sends the wrong ones and reports a prefix as sent. In
`uv__udp_sendmsgv` (`src/unix/udp.c`) the index advances once in the loop that fills each
20-entry array and again by `i += r` after `sendmmsg`. The workaround passes at most 20 datagrams
per call, so a batch of 32 makes the same two `sendmmsg` calls a fixed libuv would.

- On `UV_EAGAIN` or a short count, the sender stops the idle handle, sends the next datagram with
  `uv_udp_send`, and restarts the idle handle from that send's callback. `uv_udp_try_send2`
  refuses to run while a `uv_udp_send` is queued, so this order is needed. Datagrams of a batch
  that did not go out are not counted, and the next batch reuses their sequence numbers.

**Counting.** The sender notes its next sequence number when warm-up ends and when the window
ends; `sent` is the difference. The receiver sets a bit per sequence number, and `received` is
the number of distinct ones in that range, so warm-up datagrams count on neither side. A
duplicate fails the run. Both rates divide by the sender's window.

**Stopping.** A 10 ms timer stops the receiver once 200 ms pass with nothing after its first
datagram. It does not know when the sender finished, so a 200 ms stall mid-run would stop it
early and show as loss. If no datagram arrives within `seconds` + 10 s, the receiver gives up and
the run fails with a message saying so.

## file_seq and file_rand

**Default target.** libuv runs each `uv_fs_*` call on its thread pool, 4 threads by default; the
runner does not set `UV_THREADPOOL_SIZE`. Each block of `file_seq` is a hand-off to a pool thread
and back. In `file_rand` at depth 32 only 4 reads run at once and the rest wait in libuv's queue.
That wait is part of the latency.

**io_uring target.** libuv 1.53 sends file I/O to io_uring only when two things hold:

- the loop is set up with `uv_loop_configure(UV_LOOP_USE_IO_URING_SQPOLL)`, which the binary does
  when `BENCH_VARIANT=io_uring`;
- `UV_USE_IO_URING` is a positive number. The code accepts only positive values, though its
  comment says unset works too. Without it libuv quietly stays on the thread pool, so the binary
  exits 1 if the variable is missing.

libuv's io_uring mode is always SQPOLL: a 64-entry ring that a kernel thread polls, going idle
after 10 ms. Submitting costs no system call. Neither vio's nor Asio's io_uring backend uses
SQPOLL. The file results carry no CPU figure, so the polling thread's cost does not show.
Reads, writes and `fsync` go through the ring; libuv falls back to the pool only when the ring is
full, which depth 32 does not reach.

**Both targets.**

- Opening, closing and deleting the file are blocking `uv_fs_*` calls (no callback), as is filling
  `file_rand`'s file in 1 MiB writes. None of that is timed, and none of it uses the pool or the
  ring.
- Files are named `bench_libuv_<workload>_<pid>.tmp`, in the working directory.
- `file_seq` uses one `O_RDWR | O_CREAT | O_EXCL` descriptor for both passes. Write time runs from
  the first `uv_fs_write` to the `uv_fs_fsync` callback; read time from there to the last
  `uv_fs_read` callback.
- `file_rand` keeps `depth` `uv_fs_read` calls in flight. All slots share one xorshift64 seeded
  with 42, drawn in the order reads are issued. The file is not fsynced before the reads. Latency
  runs from just before `uv_fs_read` to its callback.

## timer_accuracy

- One `uv_timer_t`, re-armed from its own callback. libuv timers take milliseconds, so
  `interval_us` rounds up to whole milliseconds. That is exact for 1000. Lateness is measured
  against `interval_us`, so any rounding would show as lateness.
- `uv_update_time` runs before the first sleep, so a stale loop clock cannot fire it early.

## timer_churn

- Arming a timer means `uv_timer_init`, `uv_update_time`, `uv_timer_start`. Without
  `uv_update_time`, every timer would be due from the loop's cached time, not from the instant it
  was armed; libuv's docs say to call it when the loop has been busy for more than a millisecond.
  `fire_cpu_ns` covers arming as well as firing, so it includes that clock read for each timer.
- `last_late_ms` is the last callback's time minus (the arming instant of the last-armed timer
  with the largest due time, plus that due time). With few timers it can read up to 1 ms early,
  since the loop clock drops the fraction of a millisecond.
- At count=100000 the last timer fires tens of milliseconds late. `uv__run_timers` takes every
  due timer off its heap before it runs any callback, and firing 100,000 timers takes longer than
  their 100 ms spread.
- Phase two uses fresh handles and the cached loop time: `uv_timer_init` and `uv_timer_start`,
  then `uv_timer_stop`, which runs no callback. `arm_cancel_ns` includes the `uv_timer_init`.
  Closing the 2 x count handles is not timed.

## wake_pingpong and wake_flood

- libuv has no call to run a function on another loop. As METHOD says, the program uses a
  mutex-guarded `std::vector<std::function<void()>>` and `uv_async_send`. The async callback swaps
  out the whole queue and runs it. `uv_async_send` does not write again while a wake-up is pending,
  so one wake-up can run many items.
- Each item captures one pointer, which fits in `std::function`'s inline storage in libstdc++. A
  post allocates nothing beyond vector growth, and the vectors keep their capacity.
- `wake_pingpong` starts from inside loop A. Its rate is `count` over the time from the first post
  to the last return.
- `wake_flood` times from the poster thread's first post to the callback that brings the counter
  to `count`. The poster starts as soon as its thread does; unlike the vio, Asio and Tokio
  posters, it does not wait for the loop to run. It ends with `uv_stop`, and joins the poster
  before it closes the async handle.
