# vio

Read this before you read a vio number. It says which API the code uses where `METHOD.md` leaves
a choice, where it departs from `METHOD.md` and why, and what makes a number hard to compare.

## Setup

- vio is pinned at `45f78b1` (`VIO_PINNED_TAG` in `CMakeLists.txt`); `library_version` reports
  it. When `FETCHCONTENT_SOURCE_DIR_VIO` replaces the fetch, configure reads that tree's
  `.git/HEAD` as a file, without running git. It stops if HEAD names another commit, and only
  warns if HEAD names a branch.
- One binary serves both targets; `VIO_BACKEND` picks `epoll` or `io_uring`. When the ring
  cannot start, vio falls back to epoll. The binary then exits 2 and names both backends, so the
  io_uring column never holds epoll numbers. `work.backend` names the backend that ran.
- epoll: edge-triggered, each socket registered once, waits with `epoll_pwait2` and a
  nanosecond timeout.
- io_uring: the ring reports socket readiness with a one-shot `POLL_ADD` armed each time an
  operation waits (multishot only for the cross-thread wake); socket reads and writes are still
  plain system calls. File reads, writes and `fsync` are ring operations. No SQPOLL. The ring
  needs Linux 5.18 or newer (vio asks for `IORING_SETUP_SUBMIT_ALL`); on an older kernel it does
  not start, and the binary exits 2 as above.
- On epoll, file operations run on vio's file pool: four threads, shared by the process
  (`VIO_FILE_THREADS`, which the runner does not set).
- TCP reads and writes and UDP sends try the system call first and wait only on `EAGAIN`. An
  await that finds data, or room, does not suspend.
- TLS is BoringSSL `269525e`, built with vio. `tls_library` reports `BoringSSL 269525e`.

## All workloads

- Where a workload has two sides, each runs its own `event_loop_t` on its own thread. File and
  timer workloads run one loop on the main thread.
- In `tcp_rr`, `tls_handshake` and `file_rand`, an event counts when the clock reading taken at
  its end falls inside the window, and rates divide by the window's planned length. The other
  three libraries flip a flag from timer callbacks and divide by the time measured between those
  callbacks. Either way a late timer does not skew a rate. `udp_flood` is described below.
- In `tcp_rr` and `tls_handshake`, two `vio::sleep` timers on the client loop read `cpu_s` at the
  end of warm-up and at the end of the window, so each edge is good to one loop turn.
- Any vio error fails the run, except on a connection the peer is closing.

## vio API gaps

The code works around four gaps in vio's API. Rule 2 allows setting socket options through a
native handle; the TLS items apply the same idea to the TLS context. Every handle below is a
public member, not `detail` code.

1. **No way to set `TCP_NODELAY`.** The code calls `setsockopt` on `socket.native()`.
   - Plain TCP: right after accept and right after connect.
   - TLS server: right after `ssl_server_accept`. The server has not read the ClientHello yet, so
     it has sent nothing.
   - TLS client: right after `ssl_client_connect` returns. On loopback the connect is still in
     progress then: `strace` shows `EINPROGRESS`, `setsockopt`, then the ClientHello. If the
     socket is not open at that point, the run fails rather than measure with Nagle on.
2. **No way to turn TLS 1.3 session tickets off.** `ssl_config_t::enable_session_cache = false`
   leaves BoringSSL's default, which sends two tickets. The code reaches the `SSL_CTX` through
   `ssl_server_t::handle->ssl_ctx.ctx` and `ssl_client_t::state->ssl_ctx.ctx` and makes the same
   calls as the libuv and Asio code: `SSL_SESS_CACHE_OFF`, `SSL_CTX_set_num_tickets(0)` and
   `SSL_OP_NO_TICKET` on the server, `SSL_SESS_CACHE_OFF` on each client. `strace` shows the
   server sends its first flight and then only `close_notify`.
3. **No way to ask which cipher and group were agreed.** The code reads them from the `SSL*` at
   `ssl_client_t::state->engine.ssl` with `SSL_get_current_cipher` and `SSL_get_group_name`. It
   does no I/O there.
4. **No TCP half-close.** vio has no public call to shut down the sending side, only
   `detail::socket_shutdown_write`. The `tcp_bulk` client closes the socket instead, as
   `METHOD.md` allows for vio. This one needs no native handle.

Two more limits shape the TLS rows and are not worked around:

- vio cannot share one client `SSL_CTX` between connections; each `ssl_client_create` builds its
  own. See `tls_handshake`.
- `ssl_config_t::ciphersuites` does nothing on BoringSSL, which has no TLS 1.3 cipher setting.
  The code sets it anyway to state intent. BoringSSL picks `TLS_AES_128_GCM_SHA256` on a CPU
  with AES instructions and ChaCha20 on one without; `work.cipher` says which.

## tcp_rr

- Server: `tcp_reader_t::read_into` a 64 KiB buffer, then `co_await write_tcp` of those bytes,
  then read again.
- Client: connects every socket and sets `TCP_NODELAY`, then starts warm-up. One coroutine per
  connection writes `depth` requests, each an awaited `write_tcp`, and writes another each time a
  full response arrives, until the window ends. It then reads what is still in flight and drops
  its socket. The server takes end of stream or any error as a close.
- Latency runs from just before `write_tcp` to the end of the read that completes the response.
- `requests` and latencies count responses completed inside the window. `bytes` counts whole
  reads that end inside the window. A response split across a window edge can make `bytes` differ
  from `requests x size`, and the runner then reports a work mismatch.

## tcp_bulk

- Client: one awaited `write_tcp` per chunk. A write resolves once the kernel holds all its
  bytes. After the last one the client closes the socket, since vio has no half-close.
- Server: `read_into` a 64 KiB buffer until end of stream.
- Time and `cpu_s` run from just before the first write to the server seeing end of stream.

## tls_bulk

- vio's own TLS client and server. TLS 1.3 only (`min_protocol` and `max_protocol`),
  `groups = "X25519"`. Here `work` reports `TLS_AES_128_GCM_SHA256` and `X25519`.
- The client trusts `certs/ca.pem`, read once into `ca_mem`, and checks the name `localhost`,
  which `ssl_client_connect` sets with `SSL_set1_host` and sends as SNI. The server asks for no
  client certificate.
- Client: one awaited `ssl_client_write` per chunk. A write resolves once its ciphertext is in
  the kernel. Then `ssl_client_shutdown` sends `close_notify`.
- Server: awaits `ssl_server_client_handshake`, then reads with the queue reader
  (`co_await reader`) until `close_notify` or end of stream. vio decrypts ahead into a queue of up
  to nine 64 KiB chunks; each await takes one. The chunks come from a `buffer_pool_t` of 64 KiB
  blocks passed to `ssl_server_create`, so the server reuses its buffers as the other libraries
  reuse one. Without the pool, vio allocates every chunk.
- The handshake is not timed. Time and `cpu_s` run from just before the first write to the server
  seeing `close_notify` or end of stream. The server then drops the connection, which sends its
  own `close_notify` outside the window.

## tls_handshake

- Client, per connection: `ssl_client_create`, start the clock, then `ssl_client_connect`, which
  resolves when the handshake is done. So latency covers the TCP connect, `SSL_new` and the
  handshake.
- `ssl_client_create` builds a new `SSL_CTX` and parses the CA, because vio has no shared client
  context. It runs outside p50 and p99 but inside `handshakes_per_s` and `cpu_s`. Callgrind puts
  it at 126,000 instructions per connection, about 3% of what both sides spend on a handshake.
- Close: the client drops the connection, which sends `close_notify` and closes the socket
  without waiting for the server's reply. It then yields one loop turn (`vio::sleep(loop, 0ns)`)
  before the next connect. vio resumes the coroutine from inside a callback that still holds a
  reference to the connection; without the yield, the `close_notify` and `close` of one
  connection ran after the next `connect`, inside its latency. With it, `strace` shows connect,
  ClientHello, Finished, `close_notify`, close for each connection, in order, on both backends.
  The yield counts in `handshakes_per_s`, not in p50 or p99.
- Server: accept, set `TCP_NODELAY`, await the handshake, read until the client closes, then drop
  the connection, which answers with `close_notify`. It builds its `SSL_CTX` once, before it
  listens, outside the clock. A failed server handshake fails the run.
- Stop: the server cannot know how many connections will come. When the client thread ends, the
  harness posts `cancel()` of a `cancellation_t` to the server loop with `run_in_loop`, and the
  pending listen resolves as cancelled. Only that signal crosses threads.

## udp_flood

- Both sockets bind `127.0.0.1:0` and set 4 MiB `SO_SNDBUF` and `SO_RCVBUF` through vio's own
  calls. The sender is not connected; it sends to the receiver's address.
- `batch=0`, sender: an awaited `send_udp` per datagram. It tries `sendto` first and waits for
  room only on `EAGAIN`. A send that fails with `EAGAIN` or `ENOBUFS` is tried again and not
  counted.
- `batch=0`, receiver: `udp_reader_t` with recvmmsg off, so one `recvmsg` per datagram. When its
  queue is empty, vio reads up to 32 datagrams into it; each await takes one. Buffers come from a
  `buffer_pool_t` of 2048-byte blocks (`udp_use_buffer_pool`), as in vio's own UDP benchmark.
  Without the pool, vio allocates a 64 KiB buffer per datagram.
- `batch=32`, sender: `send_segments` (UDP GSO) with `min(batch, 64, 65507 / size)` datagrams
  per call, 32 here. vio also has `udp_try_send_batch` (sendmmsg); the code does not use it.
  libuv's batch row uses sendmmsg, so the two batch rows compare GSO with sendmmsg.
  `send_segments` is a plain call, not an await: on `EAGAIN` or `ENOBUFS` the sender yields one
  loop turn and tries again. If the kernel refuses the first call, the row exits 2.
- `batch=32`, receiver: `datagram_reader_t::receive()`, which uses recvmmsg and returns up to 32
  datagrams per await. vio fixes that 32; the code cannot pass `batch` to it. Receive coalescing
  (GRO) is off, vio's default.
- Window: the sender runs for `seconds`. The window opens at the first clock check after warm-up;
  the sender checks the clock every 16 send attempts at `batch=0` and on every call at
  `batch=32`. `sent` and `received` count only sequence numbers from that point on. The receiver
  keeps a bitmap, so a duplicate counts once; libuv, Asio and Tokio fail the run on one instead.
  Both rates divide by the sender's window.
  `loss_pct` is `100 × (sent − received) / sent`.
- Stop: the two sides share nothing, so the receiver cannot see the sender finish. It checks its
  count every 10 ms and stops once the count has not moved for 200 ms, which is 200 to 210 ms
  after the last arrival, but never before its first arrival plus `seconds`. So unlike the
  libuv, Asio and Tokio receivers, a 200 ms stall mid-run does not end it early. If nothing
  arrives by `seconds` plus 10 s, it gives up and the run fails.

## file_seq

- `open_file` with `creat | excl | rdwr` creates `vio-bench-file-seq-<pid>.dat` in the current
  directory (`file_rand`: `vio-bench-file-rand-<pid>.dat`). Open, close and `unlink_file` are
  plain calls on the loop thread, outside the clock.
- An awaited `write_file` per block, then `fsync_file`, then an awaited `read_file` per block.
  One operation in flight.
- epoll: each operation is a job on the file pool, which posts its result back to the loop.
  io_uring: each operation is one ring entry.

## file_rand

- The code fills the file with 1 MiB `write_file` calls, outside the clock, with no `fsync`.
- `depth` coroutines each keep one `read_file` in flight. They draw offsets from one shared
  xorshift64 seeded with 42, so the offsets follow `METHOD.md`'s sequence, but which coroutine
  gets which depends on the order reads finish.
- Latency runs from just before the offset is drawn to the read's completion. `reads` and
  latencies count completions inside the window.

## timer_accuracy

- `co_await vio::sleep(loop, interval)`, `count` times, each timed with `steady_clock` around the
  await.
- vio reads the clock when it arms a sleep, not a cached loop time, and both backends wait with
  a nanosecond timeout, so sleeps are not rounded up to whole milliseconds. `epoll_pwait2` needs
  Linux 5.11; on an older kernel vio falls back to `epoll_wait`, which does round up.

## timer_churn

- Uses `vio::sleep` futures, the timer API vio's examples use (`examples/hello_timer.cpp`), not
  `vio::timer_t`.
- Phase one arms `count` sleeps, then awaits them in index order. `fire_cpu_ns` is process CPU
  from before the first arm to the last fire, divided by `count`. `last_late_ms` is the time all
  have fired minus the due time of the latest-due timer; the code reads the clock only when it
  arms a timer with the largest delay.
- Phase two gives each sleep its own `cancellation_t`, then calls `cancel()` on each, which takes
  the timer off the heap at once. One shared token would make the phase quadratic, because each
  registration scans the token's whole callback list. `arm_cancel_ns` covers allocating the
  tokens, arming and cancelling. The futures vector is reserved before the clock starts.
- Neither metric includes destroying the futures.

## wake_pingpong

- Two `event_loop_t`, one per thread. A posts to B with `run_in_loop`; B's function posts back to
  A. One round trip at a time; A measures each. The rate is `count` over the time from the first
  post to the last return.
- `run_in_loop` allocates a node that holds a `std::function`, queues it under a mutex, and writes
  the loop's eventfd only if no wake-up is already pending.

## wake_flood

- A plain thread posts `count` lambdas with `run_in_loop`, starting only once the loop runs. Time
  runs from its first post to the lambda that brings the counter to `count`, which stops the loop.
- Posts made while a wake-up is pending only queue; they do not write the eventfd again.
