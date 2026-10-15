# Asio: notes

What this implementation does that you need to know to read its numbers. It uses Asio 1.38.2,
standalone, fetched by its git tag, and the system's OpenSSL (3.0.13 here) through `asio::ssl`.
The code uses C++20 coroutines: `co_spawn`, `asio::awaitable` and the `asio::deferred` token.

## Two targets

Both are built from the same source.

- `asio` (`bench_asio`): sockets, timers and wake-ups run on Asio's epoll reactor. Files run on
  io_uring, because on Linux Asio's file classes exist only with io_uring (`ASIO_HAS_FILE` needs
  `ASIO_HAS_IO_URING`). That is the only reason this target defines `ASIO_HAS_IO_URING` and links
  liburing.
- `asio (io_uring)` (`bench_asio_uring`): built with `ASIO_DISABLE_EPOLL`, Asio's switch that makes
  io_uring its only backend. Compared with the default target:
  - socket reads, writes, accepts and connects are ring entries, not a wait for readiness followed
    by a system call; `strace` of `tcp_rr` shows only `io_uring_enter`;
  - timers are ring timeouts;
  - a post from another thread wakes the loop with a no-op ring entry;
  - file completions come straight from the ring, with no epoll in between.
- Neither target uses SQPOLL. Each `io_context` that needs io_uring gets one ring of 16,384
  entries, Asio's default.
- `work.backend` names what ran: `epoll`, `io_uring`, or `io_uring via epoll` for files on the
  default target.

**Without io_uring.** If `io_uring_setup` fails, the default target still runs every socket,
timer and wake workload, and `file_seq` and `file_rand` exit 2. The io_uring target exits 2 for
every workload. Both print the reason, such as `io_uring_queue_init: Function not implemented`.
The check builds a throwaway `io_context` and an unopened `asio::random_access_file` before
anything is timed. Running both binaries under a seccomp filter that makes `io_uring_setup` fail
with `ENOSYS` gives exactly this.

## All workloads

- Each thread runs its own `io_context`, built with concurrency hint 1. The code sets nothing
  else.
- On the epoll target Asio tries the system call first and waits in epoll only on `EAGAIN`. Even
  when the call succeeds at once, Asio queues the completion, so every await suspends the
  coroutine and the loop resumes it.
- In `tcp_rr` with one connection, about 1.7 `recv` calls per request return `EAGAIN`
  (`strace`). Asio's `reactor.reset_edge_on_partial_read` option skips many of them, but with it
  the `tcp_bulk` server sometimes never saw end of stream, so it is off. `reactor.io_locking=0`
  made no difference that could be measured, so it is off too.
- Where an error is an expected end (end of stream, a socket closed when the window ends, a
  cancelled accept), the code awaits with `as_tuple(asio::deferred)` and checks it. Any other error
  fails the run.
- A `steady_timer` ends warm-up and then the window. Rates divide by the steady-clock time between
  those two timer completions, and `cpu_s` is `getrusage` read in the same places.
- Results carry two extra `work` keys: `backend`, and `tls_library` on the TLS rows.

## tcp_rr

- Server: one coroutine per connection sets `TCP_NODELAY`, then loops: `async_read_some` into a
  64 KiB buffer, `async_write` of those bytes, read again.
- Client: connects every socket and sets `TCP_NODELAY`, then starts warm-up. One coroutine per
  connection writes the requests it owes, each its own awaited `async_write`, then makes one
  `async_read_some` into 64 KiB. When a read completes k responses, the coroutine writes k new
  requests back to back before it reads again. libuv writes them from inside its read callback.
- Latency runs from just before a request's `async_write` to the end of the read that completes
  its response.
- `requests` and latencies count responses completed in the window. `bytes` counts the bytes of
  reads that end in the window. A response split by a window edge can make `bytes` differ from
  `requests x size`, and the runner then reports a work mismatch.
- When the window ends the client closes every socket. The server takes end of stream, reset or
  broken pipe as a normal close.

## tcp_bulk

- Client: one awaited `async_write` per chunk, then `shutdown(send)`.
- Server: `async_read_some` into one 64 KiB buffer until end of stream.
- Time and `cpu_s` run from just before the first write to the server seeing end of stream.

## TLS: shared by tls_bulk and tls_handshake

- `asio::ssl::stream<tcp::socket>`. Asio runs OpenSSL over its own BIO pair; OpenSSL never sees
  the socket.
- TLS 1.3 only (`tlsv13_server` and `tlsv13_client` contexts). Through `native_handle()`:
  `TLS_AES_128_GCM_SHA256`, X25519 and `SSL_SESS_CACHE_OFF` on both sides,
  `SSL_CTX_set_num_tickets(0)` and `SSL_OP_NO_TICKET` on the server.
- The client trusts `certs/ca.pem` (`load_verify_file`, `verify_peer`), sends SNI with
  `SSL_set_tlsext_host_name`, and checks the name `localhost` with `host_name_verification`.
- Each side builds one context per run, before anything is timed. Each connection makes a new
  `SSL` from it.
- Asio's stream turns on `SSL_MODE_RELEASE_BUFFERS`, so OpenSSL frees its record buffers whenever
  they empty and allocates them again for the next record. libuv and `tokio (openssl)` leave that
  mode off.
- OpenSSL 3.0 does most of the work in these rows. Compare them with `libuv` and
  `tokio (openssl)`, which use the same library.

### tls_bulk

- Buffers: Asio 1.38.2 added stream constructor arguments that size a stream's input and output
  buffers, 17 KiB by default. This workload changes two of them.
  - Server input buffer: 64 KiB, so each socket read asks for up to 64 KiB of ciphertext, as
    METHOD's 64 KiB read does.
  - Client output buffer: 128 KiB. With the default, Asio turns on partial writes and its buffer
    holds one record, so a 64 KiB chunk goes out in four socket writes of 16,406 bytes. With
    128 KiB, `SSL_write` encrypts the whole chunk into four records (65,624 bytes), and they go
    out in one `sendto` (`strace`). libuv's 128 KiB ring and vio's BIO do the same.
- Each `async_read_some` on the server returns one TLS record, 16 KiB, because one `SSL_read`
  returns one record. The server awaits four times per 64 KiB.
- The client ends with `async_shutdown`. It sends `close_notify` and waits for the server's. The
  server stops the clock when it reads `close_notify`, then sends its own outside the window, so
  the client's shutdown ends cleanly. If the server sees end of stream with no `close_notify`, it
  stops the clock and closes without a reply.
- Time and `cpu_s` run from just before the client's first write after the handshake to the server
  reading `close_notify`.

### tls_handshake

- Latency runs from before the client builds the stream to the end of `async_handshake`. That
  covers `SSL_new`, the stream's two zero-filled 17 KiB buffers (Asio's default), the TCP connect
  and `TCP_NODELAY`.
- The close is two-way. Asio has no one-way TLS close: `async_shutdown` sends `close_notify`, then
  waits for the peer's `close_notify` or end of stream. The server reads `close_notify` and closes
  the socket without sending its own, as Asio's SSL example server does. The client then sees end
  of stream, which Asio reports as `stream_truncated` and the code accepts. Per connection,
  `strace` shows ClientHello, Finished, `close_notify`, a `recv` that returns 0, then `close`.
- So each connection waits about one loopback round trip for the server's FIN before the next
  connect. That wait is outside p50 and p99 but inside `handshakes_per_s` and `cpu_s`. The vio,
  libuv and Tokio clients send `close_notify` and move on.
- Server: accepts in a loop and spawns a coroutine per connection, which sets `TCP_NODELAY`,
  completes the handshake and reads until the client closes. When the client loop ends, the main
  thread posts a close of the acceptor to the server's loop.

## udp_flood

- Both sockets bind `127.0.0.1:0` and set 4 MiB `SO_SNDBUF` and `SO_RCVBUF` through Asio's socket
  options. The sender is not connected; each send names the receiver.
- `batch=0`: the sender awaits `async_send_to` per datagram. The receiver awaits
  `async_receive_from` per datagram into a 64 KiB buffer.
- `batch=32` exits 2. Asio has no call that sends or receives more than one datagram: no
  `sendmmsg`, no `recvmmsg`, and no GSO or GRO option. A buffer sequence passed to `async_send_to`
  is gathered into one datagram. METHOD forbids turning on GSO or GRO through the native handle.
- Counting: the sender notes its next sequence number when warm-up ends and when the window ends;
  `sent` is the difference. The receiver sets a bit per sequence number, and `received` counts the
  distinct ones in that range. A duplicate fails the run. Both rates divide by the sender's window.
- Stopping: every 10 ms the receiver checks its count, and it stops once 200 ms pass with no new
  datagram after its first. A 200 ms stall mid-run would stop it early and show as loss. If nothing
  arrives within `seconds` plus 200 ms of its start, it stops and the run reports 100% loss. The
  vio, libuv and Tokio receivers wait `seconds` plus 10 s instead, and then fail the run.

## file_seq and file_rand

- `asio::random_access_file` on both targets, so both run files on io_uring. The default target's
  file rows compare io_uring with the thread pools of libuv and of vio on epoll.
- On the default target Asio submits each operation to a ring and learns of completions through
  an eventfd that epoll watches. In `file_seq`, `strace` counts per operation about one
  `io_uring_enter`, one `read` of the eventfd and three `epoll_wait`. The io_uring target needs
  about 1.5 `io_uring_enter` per operation and no epoll.
- Asio has no async `fsync`. `sync_all()` is a blocking `fsync` on the loop thread, inside the
  write time.
- Open (`read_write | create | exclusive`), close and delete are blocking calls outside the clock.
  The file is `bench_asio_<workload>_<pid>.tmp` in the working directory.
- `file_seq`: an awaited `async_write_some_at` per block (a short write goes on from where it
  stopped), `sync_all()`, then an awaited `async_read_some_at` per block. One operation in flight.
- `file_rand`: blocking `asio::write_at` calls of 1 MiB fill the file, outside the clock and with
  no `fsync`. Then `depth` coroutines each keep one `async_read_some_at` in flight. They draw
  offsets from one shared xorshift64 seeded with 42, in the order reads are issued. Latency runs
  from just before the read to its completion. A short read fails the run.

## timer_accuracy

- One `steady_timer`: `expires_after(interval)`, then an awaited `async_wait`, timed with the
  steady clock around both.
- Asio keeps expiry times in nanoseconds. The epoll target waits with a `timerfd`, the io_uring
  target with a ring timeout; neither rounds to milliseconds.

## timer_churn

- Phase one: one coroutine builds `count` `steady_timer`s and gives each a plain completion
  handler, not a coroutine of its own. `fire_cpu_ns` is process CPU from before the first arm to
  the last handler, divided by `count`, so it includes arming. `last_late_ms` is the time at the
  last handler minus that timer's own expiry.
- Phase two: `count` timers due in 60 s, each with a handler, then `cancel()` on each.
  `arm_cancel_ns` covers building, arming and cancelling. The handlers that get
  `operation_aborted` run after the clock stops and count `cancelled`. Both vectors are reserved
  before phase one.

## wake_pingpong and wake_flood

- Each loop's `io_context` owns one unarmed `steady_timer`, which makes Asio start its reactor. An
  `io_context` that owns no I/O object has no reactor: its idle thread waits on a condition
  variable, and a post wakes it with a futex. With the timer, an idle loop waits in `epoll_wait`
  (io_uring target: `io_uring_enter`), as any loop that does I/O waits, and a post from another
  thread wakes it with `epoll_ctl` on the reactor's eventfd (io_uring target: a no-op ring entry).
  The libuv, vio and Tokio loops wait the same way.
- Asio wakes the reactor only if it is not already woken, so one wake can run many posts, as in
  vio and libuv. In `wake_flood` each `asio::post` allocates one small operation on the heap
  (valgrind: one allocation per post), as vio's `run_in_loop` and Tokio's `Handle::spawn` do.
- `wake_pingpong`: A runs `co_await asio::post(fn, b_executor, asio::deferred)`. `fn` runs on B
  and is counted. Asio then hands the completion back to A's executor from B's thread, which queues
  it and wakes A. That is the round trip: two cross-thread hand-offs, the second made by Asio's
  completion, not by a second `asio::post` in the benchmark. A times each round trip, and the rate
  is `count` over the time from the first post to the last return.
- `wake_flood`: a plain thread calls `asio::post` with `count` lambdas, starting once it sees the
  loop run. The lambda that brings the counter to `count` records the time and releases the work
  guard.
