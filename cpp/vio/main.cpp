#include "support.h"

int main(int argc, char **argv)
{
  return bench::run(argc, argv, vio_bench::library, vio_bench::version,
                    {
                      {"tcp_rr", vio_bench::tcp_rr},
                      {"tcp_bulk", vio_bench::tcp_bulk},
                      {"tls_bulk", vio_bench::tls_bulk},
                      {"tls_handshake", vio_bench::tls_handshake},
                      {"udp_flood", vio_bench::udp_flood},
                      {"file_seq", vio_bench::file_seq},
                      {"file_rand", vio_bench::file_rand},
                      {"timer_accuracy", vio_bench::timer_accuracy},
                      {"timer_churn", vio_bench::timer_churn},
                      {"wake_pingpong", vio_bench::wake_pingpong},
                      {"wake_flood", vio_bench::wake_flood},
                    });
}
