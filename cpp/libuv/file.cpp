#include "uv_bench.h"

#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

namespace uvb
{
namespace
{
constexpr std::int64_t setup_block = 1024 * 1024;

std::string scratch_path(std::string_view workload)
{
  return std::format("bench_libuv_{}_{}.tmp", workload, ::getpid());
}

uv_file create_file(uv_loop_t *loop, const std::string &path)
{
  uv_fs_t request;
  int file = uv_fs_open(loop, &request, path.c_str(), UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_EXCL, 0644, nullptr);
  uv_fs_req_cleanup(&request);
  check_uv(file, std::format("create {}", path));
  return file;
}

void close_and_remove(uv_loop_t *loop, uv_file file, const std::string &path)
{
  uv_fs_t request;
  int rc = uv_fs_close(loop, &request, file, nullptr);
  uv_fs_req_cleanup(&request);
  check_uv(rc, "uv_fs_close");
  rc = uv_fs_unlink(loop, &request, path.c_str(), nullptr);
  uv_fs_req_cleanup(&request);
  check_uv(rc, std::format("remove {}", path));
}

std::int64_t take_result(uv_fs_t *request, std::string_view what)
{
  auto result = std::int64_t(request->result);
  uv_fs_req_cleanup(request);
  if (result < 0)
    check_uv(int(result), what);
  return result;
}

struct sequential_t
{
  uv_loop_t *loop = nullptr;
  uv_file file = -1;
  uv_fs_t request{};
  std::vector<std::byte> block;
  std::vector<char> read_buffer;
  std::int64_t total = 0;
  std::int64_t written = 0;
  std::int64_t read = 0;
  std::int64_t write_start_ns = 0;
  std::int64_t write_end_ns = 0;
  std::int64_t read_start_ns = 0;
  std::int64_t read_end_ns = 0;
};

std::size_t next_length(const sequential_t &job, std::int64_t done)
{
  return std::size_t(std::min<std::int64_t>(std::int64_t(job.block.size()), job.total - done));
}

void sequential_write(sequential_t &job);
void sequential_read(sequential_t &job);

void sequential_read_done(uv_fs_t *request)
{
  auto &job = *static_cast<sequential_t *>(request->data);
  auto result = take_result(request, "uv_fs_read");
  bench::check(result > 0, "the file ended before everything written was read back");
  job.read += result;
  if (job.read < job.total)
    sequential_read(job);
  else
    job.read_end_ns = bench::now_ns();
}

void sequential_read(sequential_t &job)
{
  auto buf = uv_buf_init(job.read_buffer.data(), unsigned(next_length(job, job.read)));
  check_uv(uv_fs_read(job.loop, &job.request, job.file, &buf, 1, job.read, sequential_read_done), "uv_fs_read");
}

void sequential_synced(uv_fs_t *request)
{
  auto &job = *static_cast<sequential_t *>(request->data);
  take_result(request, "uv_fs_fsync");
  job.write_end_ns = bench::now_ns();
  job.read_start_ns = bench::now_ns();
  sequential_read(job);
}

void sequential_write_done(uv_fs_t *request)
{
  auto &job = *static_cast<sequential_t *>(request->data);
  auto result = take_result(request, "uv_fs_write");
  bench::check(result > 0, "a write made no progress");
  job.written += result;
  if (job.written < job.total)
    sequential_write(job);
  else
    check_uv(uv_fs_fsync(job.loop, &job.request, job.file, sequential_synced), "uv_fs_fsync");
}

void sequential_write(sequential_t &job)
{
  auto buf = uv_buf_init(reinterpret_cast<char *>(job.block.data()), unsigned(next_length(job, job.written)));
  check_uv(uv_fs_write(job.loop, &job.request, job.file, &buf, 1, job.written, sequential_write_done), "uv_fs_write");
}

struct random_reader_t;

struct read_slot_t
{
  uv_fs_t request{};
  std::vector<char> buffer;
  std::int64_t started_ns = 0;
  random_reader_t *reader = nullptr;
};

struct random_reader_t
{
  random_reader_t(uv_loop_t *loop, double seconds)
    : window(loop, seconds)
    , loop(loop)
  {
  }

  window_t window;
  uv_loop_t *loop;
  uv_file file = -1;
  std::int64_t block = 0;
  std::int64_t blocks = 0;
  bench::xorshift64_t offsets;
  std::vector<std::unique_ptr<read_slot_t>> slots;
  std::int64_t reads = 0;
  bench::latencies_t latencies;
};

void random_read(read_slot_t &slot);

void random_read_done(uv_fs_t *request)
{
  auto now = bench::now_ns();
  auto &slot = *static_cast<read_slot_t *>(request->data);
  auto &reader = *slot.reader;
  auto result = take_result(request, "uv_fs_read");
  bench::check(result == reader.block, "a random read came back short");
  if (reader.window.measuring())
  {
    ++reader.reads;
    reader.latencies.add(now - slot.started_ns);
  }
  if (!reader.window.ended())
    random_read(slot);
}

void random_read(read_slot_t &slot)
{
  auto &reader = *slot.reader;
  auto offset = std::int64_t(reader.offsets.next() % std::uint64_t(reader.blocks)) * reader.block;
  auto buf = uv_buf_init(slot.buffer.data(), unsigned(reader.block));
  slot.started_ns = bench::now_ns();
  check_uv(uv_fs_read(reader.loop, &slot.request, reader.file, &buf, 1, offset, random_read_done), "uv_fs_read");
}

void fill_file(uv_loop_t *loop, uv_file file, std::int64_t total)
{
  auto pattern = bench::payload(std::size_t(std::min(setup_block, total)));
  std::int64_t written = 0;
  while (written < total)
  {
    auto length = std::min<std::int64_t>(std::int64_t(pattern.size()), total - written);
    auto buf = uv_buf_init(reinterpret_cast<char *>(pattern.data()), unsigned(length));
    uv_fs_t request;
    uv_fs_write(loop, &request, file, &buf, 1, written, nullptr);
    auto result = take_result(&request, "uv_fs_write");
    bench::check(result > 0, "a write made no progress");
    written += result;
  }
}
}

bench::result_t file_seq(const bench::args_t &args)
{
  auto bytes = positive(args, "bytes");
  auto block = positive(args, "block");

  loop_t loop;
  sequential_t job;
  job.loop = loop.get();
  job.total = bytes;
  job.block = bench::payload(std::size_t(block));
  job.read_buffer.resize(std::size_t(block));
  job.request.data = &job;
  auto path = scratch_path("file_seq");
  job.file = create_file(loop.get(), path);
  job.write_start_ns = bench::now_ns();
  sequential_write(job);
  loop.run();
  close_and_remove(loop.get(), job.file, path);
  loop.run_and_close();

  bench::check(job.read_end_ns != 0, "the read pass did not finish");
  bench::result_t result;
  result.metrics["write_mib_per_s"] = mib_per_s(job.written, job.write_end_ns - job.write_start_ns);
  result.metrics["read_mib_per_s"] = mib_per_s(job.read, job.read_end_ns - job.read_start_ns);
  result.work["written"] = job.written;
  result.work["read"] = job.read;
  return result;
}

bench::result_t file_rand(const bench::args_t &args)
{
  auto file_bytes = positive(args, "file_bytes");
  auto block = positive(args, "block");
  auto depth = positive(args, "depth");
  auto seconds = positive_seconds(args);
  bench::check(file_bytes >= block, "file_bytes must hold at least one block");

  loop_t loop;
  random_reader_t reader(loop.get(), seconds);
  reader.block = block;
  reader.blocks = file_bytes / block;
  auto path = scratch_path("file_rand");
  reader.file = create_file(loop.get(), path);
  fill_file(loop.get(), reader.file, file_bytes);

  for (std::int64_t i = 0; i < depth; ++i)
  {
    auto &slot = *reader.slots.emplace_back(std::make_unique<read_slot_t>());
    slot.buffer.resize(std::size_t(block));
    slot.reader = &reader;
    slot.request.data = &slot;
  }
  reader.window.start([] {});
  for (auto &slot : reader.slots)
    random_read(*slot);
  loop.run();
  close_and_remove(loop.get(), reader.file, path);
  loop.run_and_close();

  bench::result_t result;
  result.metrics["iops"] = double(reader.reads) / reader.window.elapsed_s();
  result.metrics["p50_us"] = reader.latencies.percentile_us(50);
  result.metrics["p99_us"] = reader.latencies.percentile_us(99);
  result.work["reads"] = reader.reads;
  return result;
}
}
