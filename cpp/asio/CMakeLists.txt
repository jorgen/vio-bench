FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-38-2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(asio)
find_package(OpenSSL REQUIRED)

add_executable(bench_asio main.cpp)
target_include_directories(bench_asio SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(bench_asio PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(bench_asio PRIVATE bench_common OpenSSL::SSL OpenSSL::Crypto)
