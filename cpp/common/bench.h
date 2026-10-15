#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/resource.h>

namespace bench
{
inline constexpr std::size_t server_read_buffer = 64 * 1024;
inline constexpr int udp_socket_buffer = 4 * 1024 * 1024;
inline constexpr std::int64_t udp_quiet_ns = 200'000'000;

class args_t
{
public:
  args_t(int argc, char **argv, int first)
  {
    for (int i = first; i < argc; ++i)
    {
      std::string_view arg(argv[i]);
      auto eq = arg.find('=');
      if (eq == std::string_view::npos || eq == 0)
      {
        std::fprintf(stderr, "bad argument '%s': expected key=value\n", argv[i]);
        std::exit(1);
      }
      _values.emplace(std::string(arg.substr(0, eq)), std::string(arg.substr(eq + 1)));
    }
  }

  [[nodiscard]] const std::string &text(std::string_view key) const
  {
    auto it = _values.find(std::string(key));
    if (it == _values.end())
    {
      std::fprintf(stderr, "missing parameter '%.*s'\n", int(key.size()), key.data());
      std::exit(1);
    }
    return it->second;
  }

  [[nodiscard]] std::int64_t integer(std::string_view key) const
  {
    const auto &value = text(key);
    char *end = nullptr;
    long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0')
    {
      std::fprintf(stderr, "parameter '%.*s' is not an integer: %s\n", int(key.size()), key.data(), value.c_str());
      std::exit(1);
    }
    return parsed;
  }

  [[nodiscard]] double real(std::string_view key) const
  {
    const auto &value = text(key);
    char *end = nullptr;
    double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0')
    {
      std::fprintf(stderr, "parameter '%.*s' is not a number: %s\n", int(key.size()), key.data(), value.c_str());
      std::exit(1);
    }
    return parsed;
  }

  [[nodiscard]] const std::map<std::string, std::string> &all() const
  {
    return _values;
  }

private:
  std::map<std::string, std::string> _values;
};

[[nodiscard]] inline std::int64_t now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] inline double process_cpu_s()
{
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

[[nodiscard]] inline std::int64_t warmup_ns(double seconds)
{
  return std::int64_t(std::min(1.0, seconds / 5.0) * 1e9);
}

[[nodiscard]] inline std::int64_t seconds_ns(double seconds)
{
  return std::int64_t(seconds * 1e9);
}

class latencies_t
{
public:
  void reserve(std::size_t n)
  {
    _ns.reserve(n);
  }

  void add(std::int64_t ns)
  {
    _ns.push_back(ns);
  }

  void merge(const latencies_t &other)
  {
    _ns.insert(_ns.end(), other._ns.begin(), other._ns.end());
  }

  [[nodiscard]] std::size_t size() const
  {
    return _ns.size();
  }

  [[nodiscard]] double percentile_us(double p) const
  {
    if (_ns.empty())
      return 0.0;
    std::vector<std::int64_t> copy = _ns;
    auto index = std::size_t(p / 100.0 * double(copy.size() - 1) + 0.5);
    std::nth_element(copy.begin(), copy.begin() + std::ptrdiff_t(index), copy.end());
    return double(copy[index]) / 1000.0;
  }

  [[nodiscard]] double max_us() const
  {
    if (_ns.empty())
      return 0.0;
    return double(*std::max_element(_ns.begin(), _ns.end())) / 1000.0;
  }

private:
  std::vector<std::int64_t> _ns;
};

struct result_t
{
  std::map<std::string, double> metrics;
  std::map<std::string, std::variant<std::int64_t, std::string>> work;
};

struct xorshift64_t
{
  std::uint64_t x = 42;

  std::uint64_t next()
  {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  }
};

[[noreturn]] inline void unsupported(std::string_view why)
{
  std::fprintf(stderr, "unsupported: %.*s\n", int(why.size()), why.data());
  std::exit(2);
}

[[noreturn]] inline void fail(std::string_view why)
{
  std::fprintf(stderr, "failed: %.*s\n", int(why.size()), why.data());
  std::exit(1);
}

inline void check(bool condition, std::string_view why)
{
  if (!condition)
    fail(why);
}

[[nodiscard]] inline std::string cert_dir()
{
  if (const char *dir = std::getenv("BENCH_CERT_DIR"))
    return dir;
#if defined(BENCH_DEFAULT_CERT_DIR)
  return BENCH_DEFAULT_CERT_DIR;
#else
  return "certs";
#endif
}

[[nodiscard]] inline std::string server_cert_path()
{
  return cert_dir() + "/server.pem";
}

[[nodiscard]] inline std::string server_key_path()
{
  return cert_dir() + "/server.key";
}

[[nodiscard]] inline std::string ca_cert_path()
{
  return cert_dir() + "/ca.pem";
}

[[nodiscard]] inline std::vector<std::byte> payload(std::size_t size)
{
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = std::byte(i * 131 + 7);
  return bytes;
}

[[nodiscard]] inline std::string json_string(std::string_view text)
{
  std::string out = "\"";
  for (char c : text)
  {
    switch (c)
    {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20)
        out += std::format("\\u{:04x}", int(c));
      else
        out += c;
    }
  }
  out += '"';
  return out;
}

using workload_fn = std::function<result_t(const args_t &)>;
using workloads_t = std::map<std::string, workload_fn, std::less<>>;

inline int run(int argc, char **argv, std::string_view library, std::string_view version, const workloads_t &workloads)
{
  if (argc < 2)
  {
    std::fprintf(stderr, "usage: %s <workload> key=value ...\nworkloads:", argv[0]);
    for (const auto &[name, fn] : workloads)
      std::fprintf(stderr, " %s", name.c_str());
    std::fprintf(stderr, "\n");
    return 1;
  }
  auto it = workloads.find(std::string_view(argv[1]));
  if (it == workloads.end())
    unsupported(std::format("{} has no workload '{}'", library, argv[1]));

  args_t args(argc, argv, 2);
  result_t result = it->second(args);

  const char *variant = std::getenv("BENCH_VARIANT");
  std::string line = "{\"library\":" + json_string(library) + ",\"library_version\":" + json_string(version) +
                     ",\"variant\":" + json_string(variant ? variant : "default") + ",\"workload\":" + json_string(argv[1]) +
                     ",\"params\":{";
  bool first = true;
  for (const auto &[key, value] : args.all())
  {
    line += std::format("{}{}:{}", first ? "" : ",", json_string(key), json_string(value));
    first = false;
  }
  line += "},\"metrics\":{";
  first = true;
  for (const auto &[key, value] : result.metrics)
  {
    if (std::isfinite(value))
      line += std::format("{}{}:{}", first ? "" : ",", json_string(key), value);
    else
      line += std::format("{}{}:null", first ? "" : ",", json_string(key));
    first = false;
  }
  line += "},\"work\":{";
  first = true;
  for (const auto &[key, value] : result.work)
  {
    line += std::format("{}{}:", first ? "" : ",", json_string(key));
    if (std::holds_alternative<std::int64_t>(value))
      line += std::format("{}", std::get<std::int64_t>(value));
    else
      line += json_string(std::get<std::string>(value));
    first = false;
  }
  line += "}}";
  std::printf("%s\n", line.c_str());
  return 0;
}
} // namespace bench
