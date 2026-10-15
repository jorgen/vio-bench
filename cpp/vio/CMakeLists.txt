FetchContent_Declare(vio
    GIT_REPOSITORY https://github.com/jorgen/vio.git
    GIT_TAG 074a30ad83e393815eba2e3daffb802a97c0d182)
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
set_source_files_properties(tls.cpp PROPERTIES COMPILE_OPTIONS -fno-lifetime-dse)
