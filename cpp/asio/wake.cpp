#include "asio_bench.h"

#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <thread>

namespace asio_bench
{
namespace
{
struct io_loop_t
{
  io_loop_t()
    : context(1)
    , timer_that_starts_reactor(context)
  {
  }

  asio::io_context context;
  asio::steady_timer timer_that_starts_reactor;
};

struct pingpong_t
{
  std::int64_t count = 0;
  std::int64_t done = 0;
  std::int64_t ran_on_b = 0;
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
  bench::latencies_t latencies;
};

asio::awaitable<void> ping(asio::io_context::executor_type b, pingpong_t &game)
{
  game.start_ns = bench::now_ns();
  for (std::int64_t i = 0; i < game.count; ++i)
  {
    auto posted = bench::now_ns();
    co_await asio::post([&game] { ++game.ran_on_b; }, b, asio::deferred);
    game.latencies.add(bench::now_ns() - posted);
    ++game.done;
  }
  game.end_ns = bench::now_ns();
}

struct flood_t
{
  std::int64_t count = 0;
  std::int64_t done = 0;
  std::int64_t end_ns = 0;
};
}

bench::result_t wake_pingpong(const bench::args_t &args)
{
  pingpong_t game;
  game.count = positive(args, "count");
  game.latencies.reserve(std::size_t(game.count));

  io_loop_t a;
  io_loop_t b;
  auto b_work = asio::make_work_guard(b.context);
  std::thread b_thread([&b] { b.context.run(); });
  spawn(a.context.get_executor(), ping(b.context.get_executor(), game), "wake_pingpong");
  a.context.run();
  b_work.reset();
  b_thread.join();

  bench::check(game.ran_on_b == game.done, "a round trip did not run its work on loop B");
  bench::result_t result;
  result.metrics["round_trips_per_s"] = double(game.done) / (double(game.end_ns - game.start_ns) / 1e9);
  result.metrics["p50_us"] = game.latencies.percentile_us(50);
  result.metrics["p99_us"] = game.latencies.percentile_us(99);
  result.work["round_trips"] = game.done;
  result.work["backend"] = reactor_backend();
  return result;
}

bench::result_t wake_flood(const bench::args_t &args)
{
  flood_t flood;
  flood.count = positive(args, "count");

  io_loop_t loop;
  auto work = asio::make_work_guard(loop.context);
  std::atomic<bool> running{false};
  asio::post(loop.context, [&running] { running.store(true, std::memory_order_release); });

  std::int64_t start_ns = 0;
  std::thread poster([&] {
    while (!running.load(std::memory_order_acquire))
      std::this_thread::yield();
    start_ns = bench::now_ns();
    for (std::int64_t i = 0; i < flood.count; ++i)
      asio::post(loop.context, [&flood, &work] {
        if (++flood.done == flood.count)
        {
          flood.end_ns = bench::now_ns();
          work.reset();
        }
      });
  });
  loop.context.run();
  poster.join();

  bench::check(flood.done == flood.count, "not every post ran");
  bench::result_t result;
  result.metrics["posts_per_s"] = double(flood.done) / (double(flood.end_ns - start_ns) / 1e9);
  result.work["posts"] = flood.done;
  result.work["backend"] = reactor_backend();
  return result;
}
}
