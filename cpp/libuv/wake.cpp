#include "uv_bench.h"

#include <functional>
#include <future>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>

namespace uvb
{
namespace
{
class mailbox_t
{
public:
  explicit mailbox_t(uv_loop_t *loop)
  {
    check_uv(uv_async_init(loop, &_async, drain), "uv_async_init");
    _async.data = this;
  }

  mailbox_t(const mailbox_t &) = delete;
  mailbox_t &operator=(const mailbox_t &) = delete;

  void post(std::function<void()> work)
  {
    {
      std::lock_guard lock(_mutex);
      _queue.push_back(std::move(work));
    }
    check_uv(uv_async_send(&_async), "uv_async_send");
  }

  void close()
  {
    close_handle(&_async);
  }

private:
  static void drain(uv_async_t *async)
  {
    auto *self = static_cast<mailbox_t *>(async->data);
    {
      std::lock_guard lock(self->_mutex);
      self->_running.swap(self->_queue);
    }
    for (auto &work : self->_running)
      work();
    self->_running.clear();
  }

  std::mutex _mutex;
  std::vector<std::function<void()>> _queue;
  std::vector<std::function<void()>> _running;
  uv_async_t _async{};
};

struct pingpong_t
{
  mailbox_t *a = nullptr;
  mailbox_t *b = nullptr;
  std::int64_t count = 0;
  std::int64_t done = 0;
  std::int64_t start_ns = 0;
  std::int64_t posted_ns = 0;
  std::int64_t end_ns = 0;
  bench::latencies_t latencies;
};

void round_trip(pingpong_t &game);

void returned(pingpong_t &game)
{
  auto now = bench::now_ns();
  game.latencies.add(now - game.posted_ns);
  if (++game.done < game.count)
  {
    round_trip(game);
    return;
  }
  game.end_ns = now;
  game.b->post([&game] { game.b->close(); });
  game.a->close();
}

void round_trip(pingpong_t &game)
{
  game.posted_ns = bench::now_ns();
  game.b->post([&game] { game.a->post([&game] { returned(game); }); });
}

struct flood_t
{
  uv_loop_t *loop = nullptr;
  std::int64_t count = 0;
  std::int64_t done = 0;
  std::int64_t end_ns = 0;
};
}

bench::result_t wake_pingpong(const bench::args_t &args)
{
  auto count = positive(args, "count");

  std::promise<mailbox_t *> b_ready;
  auto b_mailbox = b_ready.get_future();
  std::thread b_thread([&b_ready] {
    loop_t loop;
    mailbox_t mailbox(loop.get());
    b_ready.set_value(&mailbox);
    loop.run_and_close();
  });

  loop_t loop;
  mailbox_t a(loop.get());
  pingpong_t game;
  game.a = &a;
  game.b = b_mailbox.get();
  game.count = count;
  game.latencies.reserve(std::size_t(count));
  a.post([&game] {
    game.start_ns = bench::now_ns();
    round_trip(game);
  });
  loop.run_and_close();
  b_thread.join();

  bench::result_t result;
  result.metrics["round_trips_per_s"] = double(game.done) / (double(game.end_ns - game.start_ns) / 1e9);
  result.metrics["p50_us"] = game.latencies.percentile_us(50);
  result.metrics["p99_us"] = game.latencies.percentile_us(99);
  result.work["round_trips"] = game.done;
  return result;
}

bench::result_t wake_flood(const bench::args_t &args)
{
  auto count = positive(args, "count");

  loop_t loop;
  mailbox_t mailbox(loop.get());
  flood_t flood;
  flood.loop = loop.get();
  flood.count = count;
  std::int64_t start_ns = 0;
  std::atomic<bool> loop_running = false;
  uv_timer_t started;
  uv_timer_init(loop.get(), &started);
  started.data = &loop_running;
  uv_timer_start(&started, [](uv_timer_t *timer) {
    static_cast<std::atomic<bool> *>(timer->data)->store(true, std::memory_order_release);
    uv_close(reinterpret_cast<uv_handle_t *>(timer), nullptr);
  }, 0, 0);
  std::thread poster([&] {
    while (!loop_running.load(std::memory_order_acquire))
      std::this_thread::yield();
    start_ns = bench::now_ns();
    for (std::int64_t i = 0; i < count; ++i)
      mailbox.post([&flood] {
        if (++flood.done == flood.count)
        {
          flood.end_ns = bench::now_ns();
          uv_stop(flood.loop);
        }
      });
  });
  loop.run();
  poster.join();
  mailbox.close();
  loop.run_and_close();

  bench::check(flood.done == count, "not every post ran");
  bench::result_t result;
  result.metrics["posts_per_s"] = double(flood.done) / (double(flood.end_ns - start_ns) / 1e9);
  result.work["posts"] = flood.done;
  return result;
}
}
