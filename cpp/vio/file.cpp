#include "support.h"

#include <vio/operation/file.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <unistd.h>

namespace vio_bench
{
namespace
{
constexpr std::int64_t fill_block = 1024 * 1024;

std::string scratch_path(std::string_view stem)
{
  return std::format("{}-{}.dat", stem, ::getpid());
}

vio::auto_close_file_t create_file(vio::event_loop_t &loop, const std::string &path)
{
  auto opened = vio::open_file(loop, path, vio::file_open_flags_t(vio::file_open_flag_t::creat, vio::file_open_flag_t::excl, vio::file_open_flag_t::rdwr), 0644);
  require(opened, std::format("open_file {}", path));
  return std::move(*opened);
}

std::vector<std::uint8_t> pattern(std::size_t size)
{
  auto bytes = bench::payload(size);
  std::vector<std::uint8_t> out(size);
  std::memcpy(out.data(), bytes.data(), size);
  return out;
}

struct file_seq_result_t
{
  std::int64_t written = 0;
  std::int64_t read = 0;
  std::int64_t write_ns = 0;
  std::int64_t read_ns = 0;
};

vio::task_t<void> sequential(vio::event_loop_t &loop, std::int64_t bytes, std::int64_t block, file_seq_result_t &result)
{
  const auto path = scratch_path("vio-bench-file-seq");
  {
    auto file = create_file(loop, path);
    const auto data = pattern(std::size_t(block));

    const auto write_start = bench::now_ns();
    for (std::int64_t offset = 0; offset < bytes;)
    {
      const auto length = std::size_t(std::min(block, bytes - offset));
      auto wrote = co_await vio::write_file(loop, *file, data.data(), length, offset);
      require(wrote, "write_file");
      bench::check(*wrote > 0, "write_file wrote nothing");
      offset += std::int64_t(*wrote);
      result.written += std::int64_t(*wrote);
    }
    require(co_await vio::fsync_file(loop, *file), "fsync_file");
    result.write_ns = bench::now_ns() - write_start;

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(block));
    const auto read_start = bench::now_ns();
    for (std::int64_t offset = 0; offset < bytes;)
    {
      const auto length = std::size_t(std::min(block, bytes - offset));
      auto got = co_await vio::read_file(loop, *file, buffer.data(), length, offset);
      require(got, "read_file");
      bench::check(*got > 0, "read_file hit end of file early");
      offset += std::int64_t(*got);
      result.read += std::int64_t(*got);
    }
    result.read_ns = bench::now_ns() - read_start;
  }
  require(vio::unlink_file(loop, path), "unlink_file");
}

struct file_rand_shared_t
{
  vio::event_loop_t &loop;
  vio::file_t &file;
  std::int64_t block = 0;
  std::uint64_t blocks = 0;
  window_t window;
  bench::xorshift64_t random;
  bench::latencies_t latencies;
  std::int64_t reads = 0;
};

vio::task_t<void> fill(vio::event_loop_t &loop, vio::file_t &file, std::int64_t bytes)
{
  const auto data = pattern(std::size_t(std::min(fill_block, bytes)));
  for (std::int64_t offset = 0; offset < bytes;)
  {
    const auto length = std::size_t(std::min(std::int64_t(data.size()), bytes - offset));
    auto wrote = co_await vio::write_file(loop, file, data.data(), length, offset);
    require(wrote, "write_file");
    bench::check(*wrote > 0, "write_file wrote nothing");
    offset += std::int64_t(*wrote);
  }
}

vio::task_t<void> random_reader(file_rand_shared_t &shared)
{
  std::vector<std::uint8_t> buffer(std::size_t(shared.block));
  for (;;)
  {
    const auto issued = bench::now_ns();
    if (issued >= shared.window.end)
      break;
    const auto offset = std::int64_t(shared.random.next() % shared.blocks) * shared.block;
    auto got = co_await vio::read_file(shared.loop, shared.file, buffer.data(), buffer.size(), offset);
    require(got, "read_file");
    bench::check(std::int64_t(*got) == shared.block, "short random read");
    const auto done = bench::now_ns();
    if (shared.window.contains(done))
    {
      ++shared.reads;
      shared.latencies.add(done - issued);
    }
  }
}

struct file_rand_result_t
{
  window_t window;
  bench::latencies_t latencies;
  std::int64_t reads = 0;
};

vio::task_t<void> random_reads(vio::event_loop_t &loop, std::int64_t file_bytes, std::int64_t block, std::int64_t depth, double seconds, file_rand_result_t &result)
{
  const auto path = scratch_path("vio-bench-file-rand");
  {
    auto file = create_file(loop, path);
    co_await fill(loop, *file, file_bytes);

    file_rand_shared_t shared{.loop = loop, .file = *file, .block = block, .blocks = std::uint64_t(file_bytes / block)};
    shared.window = window_t::starting(bench::now_ns(), seconds);
    std::vector<vio::task_t<void>> readers;
    readers.reserve(std::size_t(depth));
    for (std::int64_t i = 0; i < depth; ++i)
      readers.push_back(random_reader(shared));
    for (auto &reader : readers)
      co_await std::move(reader);

    result.window = shared.window;
    result.latencies = std::move(shared.latencies);
    result.reads = shared.reads;
  }
  require(vio::unlink_file(loop, path), "unlink_file");
}
}

bench::result_t file_seq(const bench::args_t &args)
{
  const auto bytes = positive(args, "bytes");
  const auto block = positive(args, "block");

  file_seq_result_t result;
  const auto backend = run_loop([&](vio::event_loop_t &loop) { return sequential(loop, bytes, block, result); });

  bench::result_t out;
  out.metrics["write_mib_per_s"] = mib_per_second(result.written, result.write_ns);
  out.metrics["read_mib_per_s"] = mib_per_second(result.read, result.read_ns);
  out.work["written"] = result.written;
  out.work["read"] = result.read;
  out.work["backend"] = backend;
  return out;
}

bench::result_t file_rand(const bench::args_t &args)
{
  const auto file_bytes = positive(args, "file_bytes");
  const auto block = positive(args, "block");
  const auto depth = positive(args, "depth");
  const auto seconds = positive_real(args, "seconds");
  bench::check(file_bytes >= block, "parameter 'file_bytes' must hold at least one block");

  file_rand_result_t result;
  const auto backend = run_loop([&](vio::event_loop_t &loop) { return random_reads(loop, file_bytes, block, depth, seconds, result); });

  bench::result_t out;
  out.metrics["iops"] = double(result.reads) / result.window.measured_s();
  out.metrics["p50_us"] = result.latencies.percentile_us(50);
  out.metrics["p99_us"] = result.latencies.percentile_us(99);
  out.work["reads"] = result.reads;
  out.work["backend"] = backend;
  return out;
}
}
