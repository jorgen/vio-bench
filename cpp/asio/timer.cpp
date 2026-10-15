#include "asio_bench.h"

#include <vector>

namespace asio_bench
{
namespace
{
constexpr std::chrono::seconds cancelled_timeout(60);

struct accuracy_result_t
{
  bench::latencies_t late;
  std::int64_t sleeps = 0;
  std::int64_t early = 0;
};

asio::awaitable<void> sleep_repeatedly(std::int64_t count, std::int64_t interval_us, accuracy_result_t &result)
{
  asio::steady_timer timer(co_await asio::this_coro::executor);
  const std::chrono::microseconds interval(interval_us);
  const auto interval_ns = std::chrono::nanoseconds(interval).count();
  for (std::int64_t i = 0; i < count; ++i)
  {
    auto start = bench::now_ns();
    timer.expires_after(interval);
    co_await timer.async_wait(asio::deferred);
    auto late = bench::now_ns() - start - interval_ns;
    if (late < 0)
      ++result.early;
    result.late.add(late);
    ++result.sleeps;
  }
}

std::chrono::milliseconds churn_due(std::int64_t index)
{
  return std::chrono::milliseconds((index * 7919) % 100);
}

struct churn_t
{
  std::int64_t count = 0;
  std::vector<asio::steady_timer> firing;
  std::vector<asio::steady_timer> cancelling;
  asio::steady_timer *all_fired = nullptr;
  std::int64_t fired = 0;
  std::int64_t cancelled = 0;
  std::int64_t last_late_ns = 0;
  double fire_start_cpu = 0.0;
  double fire_cpu_s = 0.0;
  std::int64_t arm_cancel_ns = 0;
};

void fired(churn_t &churn, std::size_t index, const std::error_code &error)
{
  if (error)
    fail("churn timer", error);
  if (++churn.fired < churn.count)
    return;
  auto now = bench::now_ns();
  churn.fire_cpu_s = bench::process_cpu_s() - churn.fire_start_cpu;
  churn.last_late_ns = now - std::chrono::nanoseconds(churn.firing[index].expiry().time_since_epoch()).count();
  churn.all_fired->cancel();
}

void cancelled(churn_t &churn, const std::error_code &error)
{
  if (error != asio::error::operation_aborted)
    bench::fail("a cancelled timer fired");
  ++churn.cancelled;
}

asio::awaitable<void> churn(churn_t &churn)
{
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer all_fired(executor, asio::steady_timer::time_point::max());
  churn.all_fired = &all_fired;
  churn.firing.reserve(std::size_t(churn.count));
  churn.cancelling.reserve(std::size_t(churn.count));

  churn.fire_start_cpu = bench::process_cpu_s();
  for (std::int64_t i = 0; i < churn.count; ++i)
  {
    auto &timer = churn.firing.emplace_back(executor, churn_due(i));
    timer.async_wait([&churn, index = std::size_t(i)](const std::error_code &error) { fired(churn, index, error); });
  }
  co_await all_fired.async_wait(with_error);

  auto arm_start = bench::now_ns();
  for (std::int64_t i = 0; i < churn.count; ++i)
  {
    auto &timer = churn.cancelling.emplace_back(executor, cancelled_timeout);
    timer.async_wait([&churn](const std::error_code &error) { cancelled(churn, error); });
  }
  for (auto &timer : churn.cancelling)
    timer.cancel();
  churn.arm_cancel_ns = bench::now_ns() - arm_start;
}
}

bench::result_t timer_accuracy(const bench::args_t &args)
{
  auto count = positive(args, "count");
  auto interval_us = non_negative(args, "interval_us");

  asio::io_context context(1);
  accuracy_result_t accuracy;
  accuracy.late.reserve(std::size_t(count));
  spawn(context.get_executor(), sleep_repeatedly(count, interval_us, accuracy), "timer_accuracy");
  context.run();

  bench::result_t result;
  result.metrics["p50_late_us"] = accuracy.late.percentile_us(50);
  result.metrics["p99_late_us"] = accuracy.late.percentile_us(99);
  result.metrics["max_late_us"] = accuracy.late.max_us();
  result.metrics["early"] = double(accuracy.early);
  result.work["sleeps"] = accuracy.sleeps;
  result.work["backend"] = reactor_backend();
  return result;
}

bench::result_t timer_churn(const bench::args_t &args)
{
  asio::io_context context(1);
  churn_t state;
  state.count = positive(args, "count");
  spawn(context.get_executor(), churn(state), "timer_churn");
  context.run();
  bench::check(state.fired == state.count, "not every timer fired");

  bench::result_t result;
  result.metrics["fire_cpu_ns"] = state.fire_cpu_s * 1e9 / double(state.count);
  result.metrics["last_late_ms"] = double(state.last_late_ns) / 1e6;
  result.metrics["arm_cancel_ns"] = double(state.arm_cancel_ns) / double(state.count);
  result.work["fired"] = state.fired;
  result.work["cancelled"] = state.cancelled;
  result.work["backend"] = reactor_backend();
  return result;
}
}
