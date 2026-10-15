FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-38-2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(asio)
find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

function(add_asio_bench target)
    add_executable(${target}
        main.cpp
        tcp.cpp
        tls.cpp
        udp.cpp
        file.cpp
        timer.cpp
        wake.cpp)
    target_include_directories(${target} SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(${target} PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED ASIO_HAS_IO_URING ${ARGN})
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_link_libraries(${target} PRIVATE bench_common OpenSSL::SSL OpenSSL::Crypto PkgConfig::LIBURING)
endfunction()

add_asio_bench(bench_asio)
add_asio_bench(bench_asio_uring ASIO_DISABLE_EPOLL)
