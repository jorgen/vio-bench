#include "support.h"

#include <atomic>
#include <future>
#include <thread>

namespace vio_bench
{
namespace
{
struct loop_thread_t
{
  std::promise<vio::event_loop_t *> ready;
  std::string backend;
  std::thread thread;

  void start()
  {
    thread = std::thread([this] {
      vio::event_loop_t loop;
      backend = requested_backend(loop);
      ready.set_value(&loop);
      loop.run();
    });
  }
};

struct pingpong_t
{
  vio::event_loop_t *a = nullptr;
  vio::event_loop_t *b = nullptr;
  std::int64_t count = 0;
  std::int64_t done = 0;
  std::int64_t sent_at = 0;
  std::int64_t started_at = 0;
  std::int64_t finished_at = 0;
  bench::latencies_t latencies;
};

void serve(pingpong_t &state);

void returned(pingpong_t &state)
{
  const auto now = bench::now_ns();
  state.latencies.add(now - state.sent_at);
  if (++state.done == state.count)
  {
    state.finished_at = now;
    state.b->stop();
    state.a->stop();
    return;
  }
  serve(state);
}

void serve(pingpong_t &state)
{
  state.sent_at = bench::now_ns();
  state.b->run_in_loop([&state] { state.a->run_in_loop([&state] { returned(state); }); });
}

struct flood_t
{
  vio::event_loop_t *loop = nullptr;
  std::int64_t count = 0;
  std::int64_t counter = 0;
  std::int64_t finished_at = 0;
};
}

bench::result_t wake_pingpong(const bench::args_t &args)
{
  pingpong_t state;
  state.count = positive(args, "count");
  state.latencies.reserve(std::size_t(state.count));

  loop_thread_t a;
  loop_thread_t b;
  b.start();
  state.b = b.ready.get_future().get();
  a.start();
  state.a = a.ready.get_future().get();
  state.a->run_in_loop([&state] {
    state.started_at = bench::now_ns();
    serve(state);
  });
  a.thread.join();
  b.thread.join();

  bench::result_t out;
  out.metrics["round_trips_per_s"] = per_second(state.done, state.finished_at - state.started_at);
  out.metrics["p50_us"] = state.latencies.percentile_us(50);
  out.metrics["p99_us"] = state.latencies.percentile_us(99);
  out.work["round_trips"] = state.done;
  out.work["backend"] = a.backend;
  return out;
}

bench::result_t wake_flood(const bench::args_t &args)
{
  flood_t state;
  state.count = positive(args, "count");

  loop_thread_t target;
  target.start();
  state.loop = target.ready.get_future().get();

  std::atomic<bool> running{false};
  state.loop->run_in_loop([&running] { running.store(true, std::memory_order_release); });
  while (!running.load(std::memory_order_acquire))
    std::this_thread::yield();

  std::int64_t started_at = 0;
  std::thread poster([&state, &started_at] {
    started_at = bench::now_ns();
    for (std::int64_t i = 0; i < state.count; ++i)
    {
      state.loop->run_in_loop([&state] {
        if (++state.counter == state.count)
        {
          state.finished_at = bench::now_ns();
          state.loop->stop();
        }
      });
    }
  });
  poster.join();
  target.thread.join();

  bench::result_t out;
  out.metrics["posts_per_s"] = per_second(state.counter, state.finished_at - started_at);
  out.work["posts"] = state.counter;
  out.work["backend"] = target.backend;
  return out;
}
}
