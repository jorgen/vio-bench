#include "asio_bench.h"

int main(int argc, char **argv)
{
#if defined(ASIO_HAS_IO_URING_AS_DEFAULT)
  asio_bench::require_io_uring("bench_asio_uring, built with ASIO_DISABLE_EPOLL,");
#endif
  const bench::workloads_t workloads{
    {"tcp_rr", asio_bench::tcp_rr},
    {"tcp_bulk", asio_bench::tcp_bulk},
    {"tls_bulk", asio_bench::tls_bulk},
    {"tls_handshake", asio_bench::tls_handshake},
    {"udp_flood", asio_bench::udp_flood},
    {"file_seq", asio_bench::file_seq},
    {"file_rand", asio_bench::file_rand},
    {"timer_accuracy", asio_bench::timer_accuracy},
    {"timer_churn", asio_bench::timer_churn},
    {"wake_pingpong", asio_bench::wake_pingpong},
    {"wake_flood", asio_bench::wake_flood},
  };
  return bench::run(argc, argv, asio_bench::library, asio_bench::version(), workloads);
}
