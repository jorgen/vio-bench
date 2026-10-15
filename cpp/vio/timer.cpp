#include "support.h"

#include <vio/cancellation.h>
#include <vio/operation/sleep.h>

#include <memory>
#include <vector>

namespace vio_bench
{
namespace
{
constexpr std::int64_t churn_spread_ms = 100;
constexpr std::int64_t churn_stride = 7919;
constexpr auto churn_cancel_delay = std::chrono::seconds(60);

struct accuracy_result_t
{
  bench::latencies_t late;
  std::int64_t sleeps = 0;
  std::int64_t early = 0;
};

vio::task_t<void> sleep_repeatedly(vio::event_loop_t &loop, std::int64_t count, std::int64_t interval_us, accuracy_result_t &result)
{
  const auto requested = std::chrono::microseconds(interval_us);
  const auto requested_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(requested).count();
  result.late.reserve(std::size_t(count));
  for (std::int64_t i = 0; i < count; ++i)
  {
    const auto start = bench::now_ns();
    require(co_await vio::sleep(loop, requested), "sleep");
    const auto late = bench::now_ns() - start - requested_ns;
    if (late < 0)
      ++result.early;
    result.late.add(late);
    ++result.sleeps;
  }
}

struct churn_result_t
{
  double fire_cpu_s = 0.0;
  std::int64_t last_late_ns = 0;
  std::int64_t arm_cancel_ns = 0;
  std::int64_t fired = 0;
  std::int64_t cancelled = 0;
};

std::int64_t churn_delay_ms(std::int64_t index)
{
  return (index * churn_stride) % churn_spread_ms;
}

vio::task_t<void> churn(vio::event_loop_t &loop, std::int64_t count, churn_result_t &result)
{
  using sleep_future_t = vio::future_t<vio::sleep_state_t>;
  std::int64_t latest_delay = 0;
  for (std::int64_t i = 0; i < count && i < churn_spread_ms; ++i)
    latest_delay = std::max(latest_delay, churn_delay_ms(i));

  {
    std::vector<sleep_future_t> timers;
    timers.reserve(std::size_t(count));
    std::int64_t latest_due = 0;
    const auto cpu_start = bench::process_cpu_s();
    for (std::int64_t i = 0; i < count; ++i)
    {
      const auto delay = churn_delay_ms(i);
      if (delay == latest_delay)
        latest_due = bench::now_ns() + delay * 1'000'000;
      timers.push_back(vio::sleep(loop, std::chrono::milliseconds(delay)));
    }
    for (auto &timer : timers)
    {
      if ((co_await timer).has_value())
        ++result.fired;
    }
    const auto all_fired = bench::now_ns();
    result.fire_cpu_s = bench::process_cpu_s() - cpu_start;
    result.last_late_ns = all_fired - latest_due;
  }

  {
    std::vector<sleep_future_t> timers;
    timers.reserve(std::size_t(count));
    const auto start = bench::now_ns();
    auto tokens = std::make_unique<vio::cancellation_t[]>(std::size_t(count));
    for (std::int64_t i = 0; i < count; ++i)
      timers.push_back(vio::sleep(loop, churn_cancel_delay, &tokens[std::size_t(i)]));
    for (std::int64_t i = 0; i < count; ++i)
      tokens[std::size_t(i)].cancel();
    result.arm_cancel_ns = bench::now_ns() - start;
    for (auto &timer : timers)
    {
      auto outcome = co_await timer;
      if (!outcome.has_value() && vio::is_cancelled(outcome.error()))
        ++result.cancelled;
    }
  }
}
}

bench::result_t timer_accuracy(const bench::args_t &args)
{
  const auto count = positive(args, "count");
  const auto interval_us = positive(args, "interval_us");

  accuracy_result_t result;
  const auto backend = run_loop([&](vio::event_loop_t &loop) { return sleep_repeatedly(loop, count, interval_us, result); });

  bench::result_t out;
  out.metrics["p50_late_us"] = result.late.percentile_us(50);
  out.metrics["p99_late_us"] = result.late.percentile_us(99);
  out.metrics["max_late_us"] = result.late.max_us();
  out.metrics["early"] = double(result.early);
  out.work["sleeps"] = result.sleeps;
  out.work["backend"] = backend;
  return out;
}

bench::result_t timer_churn(const bench::args_t &args)
{
  const auto count = positive(args, "count");

  churn_result_t result;
  const auto backend = run_loop([&](vio::event_loop_t &loop) { return churn(loop, count, result); });

  bench::result_t out;
  out.metrics["fire_cpu_ns"] = result.fire_cpu_s * 1e9 / double(count);
  out.metrics["last_late_ms"] = double(result.last_late_ns) / 1e6;
  out.metrics["arm_cancel_ns"] = double(result.arm_cancel_ns) / double(count);
  out.work["fired"] = result.fired;
  out.work["cancelled"] = result.cancelled;
  out.work["backend"] = backend;
  return out;
}
}
