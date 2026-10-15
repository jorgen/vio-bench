FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-38-2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(asio)
find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

add_executable(bench_asio
    main.cpp
    tcp.cpp
    tls.cpp
    udp.cpp
    file.cpp
    timer.cpp
    wake.cpp)
target_include_directories(bench_asio SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(bench_asio PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED ASIO_HAS_IO_URING)
target_compile_options(bench_asio PRIVATE -Wall -Wextra)
target_link_libraries(bench_asio PRIVATE bench_common OpenSSL::SSL OpenSSL::Crypto PkgConfig::LIBURING)
