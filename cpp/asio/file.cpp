#include "asio_bench.h"

#include <asio/random_access_file.hpp>
#include <asio/write_at.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace asio_bench
{
namespace
{
constexpr std::int64_t setup_block = 1024 * 1024;

std::string scratch_path(std::string_view workload)
{
  return std::format("bench_asio_{}_{}.tmp", workload, ::getpid());
}

asio::random_access_file create_file(asio::io_context &context, const std::string &path)
{
  require_io_uring("asio::random_access_file");
  return asio::random_access_file(context, path,
                                  asio::random_access_file::read_write | asio::random_access_file::create |
                                    asio::random_access_file::exclusive);
}

void close_and_remove(asio::random_access_file &file, const std::string &path)
{
  file.close();
  std::filesystem::remove(path);
}

struct sequential_result_t
{
  std::int64_t written = 0;
  std::int64_t read = 0;
  std::int64_t write_ns = 0;
  std::int64_t read_ns = 0;
};

asio::awaitable<void> sequential(asio::random_access_file &file, std::int64_t total, std::int64_t block, sequential_result_t &result)
{
  auto payload = bench::payload(std::size_t(block));
  auto buffer = std::vector<char>(std::size_t(block));

  auto write_start = bench::now_ns();
  while (result.written < total)
  {
    auto length = std::size_t(std::min(block, total - result.written));
    auto written = co_await file.async_write_some_at(std::uint64_t(result.written), asio::buffer(payload.data(), length), asio::deferred);
    bench::check(written > 0, "a write made no progress");
    result.written += std::int64_t(written);
  }
  file.sync_all();
  result.write_ns = bench::now_ns() - write_start;

  auto read_start = bench::now_ns();
  while (result.read < total)
  {
    auto length = std::size_t(std::min(block, total - result.read));
    auto [error, read] = co_await file.async_read_some_at(std::uint64_t(result.read), asio::buffer(buffer.data(), length), with_error);
    if (error == asio::error::eof)
      bench::fail("the file ended before everything written was read back");
    if (error)
      fail("file read", error);
    result.read += std::int64_t(read);
  }
  result.read_ns = bench::now_ns() - read_start;
}

struct random_reader_t
{
  explicit random_reader_t(double seconds)
    : window(seconds)
  {
  }

  window_t window;
  std::int64_t block = 0;
  std::int64_t blocks = 0;
  bench::xorshift64_t offsets;
  std::int64_t reads = 0;
  bench::latencies_t latencies;
};

asio::awaitable<void> random_reads(asio::random_access_file &file, random_reader_t &reader)
{
  auto buffer = std::vector<char>(std::size_t(reader.block));
  while (!reader.window.ended())
  {
    auto offset = (reader.offsets.next() % std::uint64_t(reader.blocks)) * std::uint64_t(reader.block);
    auto started = bench::now_ns();
    auto length = co_await file.async_read_some_at(offset, asio::buffer(buffer), asio::deferred);
    auto now = bench::now_ns();
    bench::check(std::int64_t(length) == reader.block, "a random read came back short");
    if (reader.window.measuring())
    {
      ++reader.reads;
      reader.latencies.add(now - started);
    }
  }
}

void fill_file(asio::random_access_file &file, std::int64_t total)
{
  auto pattern = bench::payload(std::size_t(std::min(setup_block, total)));
  for (std::int64_t written = 0; written < total;)
  {
    auto length = std::size_t(std::min<std::int64_t>(std::int64_t(pattern.size()), total - written));
    written += std::int64_t(asio::write_at(file, std::uint64_t(written), asio::buffer(pattern.data(), length)));
  }
}
}

bench::result_t file_seq(const bench::args_t &args)
{
  auto bytes = positive(args, "bytes");
  auto block = positive(args, "block");

  asio::io_context context(1);
  auto path = scratch_path("file_seq");
  auto file = create_file(context, path);
  sequential_result_t job;
  spawn(context.get_executor(), sequential(file, bytes, block, job), "file_seq");
  context.run();
  close_and_remove(file, path);

  bench::result_t result;
  result.metrics["write_mib_per_s"] = mib_per_s(job.written, job.write_ns);
  result.metrics["read_mib_per_s"] = mib_per_s(job.read, job.read_ns);
  result.work["written"] = job.written;
  result.work["read"] = job.read;
  result.work["backend"] = file_backend();
  return result;
}

bench::result_t file_rand(const bench::args_t &args)
{
  auto file_bytes = positive(args, "file_bytes");
  auto block = positive(args, "block");
  auto depth = positive(args, "depth");
  auto seconds = positive_seconds(args);
  bench::check(file_bytes >= block, "file_bytes must hold at least one block");

  asio::io_context context(1);
  auto path = scratch_path("file_rand");
  auto file = create_file(context, path);
  fill_file(file, file_bytes);

  random_reader_t reader(seconds);
  reader.block = block;
  reader.blocks = file_bytes / block;
  spawn(context.get_executor(), reader.window.run({}, {}), "file_rand window");
  for (std::int64_t i = 0; i < depth; ++i)
    spawn(context.get_executor(), random_reads(file, reader), "file_rand reader");
  context.run();
  close_and_remove(file, path);

  bench::result_t result;
  result.metrics["iops"] = double(reader.reads) / reader.window.elapsed_s();
  result.metrics["p50_us"] = reader.latencies.percentile_us(50);
  result.metrics["p99_us"] = reader.latencies.percentile_us(99);
  result.work["reads"] = reader.reads;
  result.work["backend"] = file_backend();
  return result;
}
}
