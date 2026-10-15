#include "uv_bench.h"

#include <memory>

namespace uvb
{
namespace
{
constexpr std::uint64_t cancelled_timeout_ms = 60'000;

struct sleeper_t
{
  uv_timer_t timer{};
  std::uint64_t timeout_ms = 0;
  std::int64_t interval_ns = 0;
  std::int64_t count = 0;
  std::int64_t sleeps = 0;
  std::int64_t early = 0;
  std::int64_t started_ns = 0;
  bench::latencies_t late;
};

void woke(uv_timer_t *timer);

void sleep_once(sleeper_t &sleeper)
{
  sleeper.started_ns = bench::now_ns();
  check_uv(uv_timer_start(&sleeper.timer, woke, sleeper.timeout_ms, 0), "uv_timer_start");
}

void woke(uv_timer_t *timer)
{
  auto now = bench::now_ns();
  auto &sleeper = *static_cast<sleeper_t *>(timer->data);
  auto late = now - sleeper.started_ns - sleeper.interval_ns;
  sleeper.late.add(late);
  if (late < 0)
    ++sleeper.early;
  if (++sleeper.sleeps < sleeper.count)
    sleep_once(sleeper);
  else
    close_handle(timer);
}

std::uint64_t churn_due_ms(std::int64_t index)
{
  return std::uint64_t((index * 7919) % 100);
}

struct churn_t
{
  std::unique_ptr<uv_timer_t[]> timers;
  std::int64_t count = 0;
  std::int64_t fired = 0;
  std::int64_t last_fired_ns = 0;
  std::uint64_t last_due_ms = 0;
  double last_fired_cpu = 0.0;
};

void churn_fired(uv_timer_t *timer)
{
  auto &churn = *static_cast<churn_t *>(timer->data);
  if (++churn.fired < churn.count)
    return;
  churn.last_fired_ns = bench::now_ns();
  churn.last_fired_cpu = bench::process_cpu_s();
  churn.last_due_ms = churn_due_ms(timer - churn.timers.get());
}

void never_fires(uv_timer_t *)
{
  bench::fail("a cancelled timer fired");
}
}

bench::result_t timer_accuracy(const bench::args_t &args)
{
  auto count = positive(args, "count");
  auto interval_us = non_negative(args, "interval_us");

  loop_t loop;
  sleeper_t sleeper;
  sleeper.count = count;
  sleeper.interval_ns = interval_us * 1000;
  sleeper.timeout_ms = ceil_ms(sleeper.interval_ns);
  sleeper.late.reserve(std::size_t(count));
  check_uv(uv_timer_init(loop.get(), &sleeper.timer), "uv_timer_init");
  sleeper.timer.data = &sleeper;
  uv_update_time(loop.get());
  sleep_once(sleeper);
  loop.run_and_close();

  bench::result_t result;
  result.metrics["p50_late_us"] = sleeper.late.percentile_us(50);
  result.metrics["p99_late_us"] = sleeper.late.percentile_us(99);
  result.metrics["max_late_us"] = sleeper.late.max_us();
  result.metrics["early"] = double(sleeper.early);
  result.work["sleeps"] = sleeper.sleeps;
  return result;
}

bench::result_t timer_churn(const bench::args_t &args)
{
  auto count = positive(args, "count");

  loop_t loop;
  churn_t churn;
  churn.count = count;
  churn.timers = std::make_unique<uv_timer_t[]>(std::size_t(count));
  auto cancelling = std::make_unique<uv_timer_t[]>(std::size_t(count));

  uv_update_time(loop.get());
  auto fire_start_ns = bench::now_ns();
  auto fire_start_cpu = bench::process_cpu_s();
  for (std::int64_t i = 0; i < count; ++i)
  {
    auto *timer = &churn.timers[std::size_t(i)];
    check_uv(uv_timer_init(loop.get(), timer), "uv_timer_init");
    timer->data = &churn;
    check_uv(uv_timer_start(timer, churn_fired, churn_due_ms(i), 0), "uv_timer_start");
  }
  loop.run();
  bench::check(churn.fired == count, "not every timer fired");

  std::int64_t cancelled = 0;
  auto arm_start_ns = bench::now_ns();
  for (std::int64_t i = 0; i < count; ++i)
  {
    auto *timer = &cancelling[std::size_t(i)];
    check_uv(uv_timer_init(loop.get(), timer), "uv_timer_init");
    check_uv(uv_timer_start(timer, never_fires, cancelled_timeout_ms, 0), "uv_timer_start");
  }
  for (std::int64_t i = 0; i < count; ++i)
  {
    auto *timer = &cancelling[std::size_t(i)];
    if (uv_is_active(as_handle(timer)))
      ++cancelled;
    check_uv(uv_timer_stop(timer), "uv_timer_stop");
  }
  auto arm_end_ns = bench::now_ns();

  for (std::int64_t i = 0; i < count; ++i)
  {
    close_handle(&churn.timers[std::size_t(i)]);
    close_handle(&cancelling[std::size_t(i)]);
  }
  loop.run_and_close();

  auto last_due_ns = fire_start_ns + std::int64_t(churn.last_due_ms) * 1'000'000;
  bench::result_t result;
  result.metrics["fire_cpu_ns"] = (churn.last_fired_cpu - fire_start_cpu) * 1e9 / double(count);
  result.metrics["last_late_ms"] = double(churn.last_fired_ns - last_due_ns) / 1e6;
  result.metrics["arm_cancel_ns"] = double(arm_end_ns - arm_start_ns) / double(count);
  result.work["fired"] = churn.fired;
  result.work["cancelled"] = cancelled;
  return result;
}
}
