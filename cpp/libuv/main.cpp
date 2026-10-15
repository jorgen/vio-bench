#include "uv_bench.h"

#include <csignal>

int main(int argc, char **argv)
{
  std::signal(SIGPIPE, SIG_IGN);
  const bench::workloads_t workloads{
    {"tcp_rr", uvb::tcp_rr},
    {"tcp_bulk", uvb::tcp_bulk},
    {"tls_bulk", uvb::tls_bulk},
    {"tls_handshake", uvb::tls_handshake},
    {"udp_flood", uvb::udp_flood},
    {"file_seq", uvb::file_seq},
    {"file_rand", uvb::file_rand},
    {"timer_accuracy", uvb::timer_accuracy},
    {"timer_churn", uvb::timer_churn},
    {"wake_pingpong", uvb::wake_pingpong},
    {"wake_flood", uvb::wake_flood},
  };
  return bench::run(argc, argv, "libuv", uv_version_string(), workloads);
}
