cmake_minimum_required(VERSION 3.30)
project(vio_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(BENCH_LIBS "vio;libuv;asio" CACHE STRING "C++ libraries to build benchmarks for")

include(FetchContent)
find_package(Threads REQUIRED)

add_library(bench_common INTERFACE)
target_include_directories(bench_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/common)
target_compile_definitions(bench_common INTERFACE BENCH_DEFAULT_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/certs")
target_link_libraries(bench_common INTERFACE Threads::Threads)

foreach(lib IN LISTS BENCH_LIBS)
    add_subdirectory(cpp/${lib})
endforeach()
