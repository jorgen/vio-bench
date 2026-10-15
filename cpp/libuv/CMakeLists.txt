FetchContent_Declare(libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG v1.53.0
    GIT_SHALLOW TRUE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libuv)
find_package(OpenSSL 3 REQUIRED)

add_executable(bench_libuv
    main.cpp
    tcp.cpp
    tls.cpp
    udp.cpp
    file.cpp
    timer.cpp
    wake.cpp)
target_compile_options(bench_libuv PRIVATE -Wall -Wextra)
target_link_libraries(bench_libuv PRIVATE bench_common uv_a OpenSSL::SSL OpenSSL::Crypto)
