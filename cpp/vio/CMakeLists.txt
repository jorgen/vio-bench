set(VIO_PINNED_TAG 45f78b119581ab559eacabe384382d8460108307)
string(SUBSTRING "${VIO_PINNED_TAG}" 0 7 VIO_PINNED_SHORT)

if(FETCHCONTENT_SOURCE_DIR_VIO AND EXISTS "${FETCHCONTENT_SOURCE_DIR_VIO}/.git/HEAD")
    file(STRINGS "${FETCHCONTENT_SOURCE_DIR_VIO}/.git/HEAD" VIO_SOURCE_HEAD LIMIT_COUNT 1)
    if(NOT VIO_SOURCE_HEAD MATCHES "^[0-9a-f]+$")
        message(WARNING "FETCHCONTENT_SOURCE_DIR_VIO is on '${VIO_SOURCE_HEAD}'; cannot confirm it is ${VIO_PINNED_TAG}")
    elseif(NOT VIO_SOURCE_HEAD STREQUAL VIO_PINNED_TAG)
        message(FATAL_ERROR "FETCHCONTENT_SOURCE_DIR_VIO is at ${VIO_SOURCE_HEAD}, not the pinned ${VIO_PINNED_TAG}")
    endif()
endif()

FetchContent_Declare(vio
    GIT_REPOSITORY https://github.com/jorgen/vio.git
    GIT_TAG ${VIO_PINNED_TAG})
set(VIO_INSTALL OFF CACHE BOOL "" FORCE)
set(VIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vio)

add_executable(bench_vio
    main.cpp
    tcp.cpp
    tls.cpp
    udp.cpp
    file.cpp
    timer.cpp
    wake.cpp)
target_link_libraries(bench_vio PRIVATE bench_common vio::vio)
if(VIO_USE_SYSTEM_BORINGSSL)
    set(VIO_BENCH_TLS_VERSION "system")
else()
    set(VIO_BENCH_TLS_VERSION "${VIO_BORINGSSL_VERSION}")
endif()
target_compile_definitions(bench_vio PRIVATE VIO_BENCH_VERSION="${VIO_PINNED_SHORT}" VIO_BENCH_TLS_VERSION="${VIO_BENCH_TLS_VERSION}")
