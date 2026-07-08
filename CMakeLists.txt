cmake_minimum_required(VERSION 3.16)
project(ERWT3D VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(ERWT3D_NATIVE_OPT "Use -march=native -mtune=native (Release only)" ON)

# Thread support
find_package(Threads REQUIRED)

# lz4 for compression
find_library(LZ4_LIBRARY lz4)
find_path(LZ4_INCLUDE_DIR lz4.h)
if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
    message(STATUS "Found lz4: ${LZ4_LIBRARY}")
    set(HAVE_LZ4 TRUE)
else()
    message(WARNING "lz4 not found, compression disabled")
    set(HAVE_LZ4 FALSE)
endif()

# zfp for fixed-rate compression (tri-axis layout + scanner)
option(ERWT3D_ENABLE_ZFP "Enable ZFP compression (tri-axis layout + scanner)" OFF)
if(ERWT3D_ENABLE_ZFP)
    find_library(ZFP_LIBRARY zfp)
    find_path(ZFP_INCLUDE_DIR zfp.h)
    if(ZFP_LIBRARY AND ZFP_INCLUDE_DIR)
        message(STATUS "Found zfp: ${ZFP_LIBRARY}")
        set(HAVE_ZFP TRUE)
    else()
        message(FATAL_ERROR "ERWT3D_ENABLE_ZFP=ON but zfp not found")
    endif()
endif()

# Native optimization (optional, Release only)
if(ERWT3D_NATIVE_OPT)
    if(NOT CMAKE_BUILD_TYPE MATCHES "Release")
        message(WARNING "ERWT3D_NATIVE_OPT is intended for Release builds; current: ${CMAKE_BUILD_TYPE}")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-march=native>
            $<$<CONFIG:Release>:-mtune=native>
        )
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported)
        if(ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endif()
    message(STATUS "Native optimization enabled (Release only): -O3 -march=native -mtune=native")
endif()

# Library sources
set(LIB_SOURCES
    src/morton.cpp
    src/writer.cpp
    src/reader.cpp
    src/slice.cpp
    src/extent.cpp
    src/cache.cpp
    src/thread_pool.cpp
    src/sb_common.cpp
    src/sb_hdd.cpp
    src/sb_panel.cpp
    src/api.cpp
    src/tri_writer.cpp
    src/tri_reader.cpp
)

# Create library
add_library(erwt3d STATIC ${LIB_SOURCES})
target_include_directories(erwt3d PUBLIC include)
target_link_libraries(erwt3d PUBLIC Threads::Threads)
if(HAVE_LZ4)
    target_link_libraries(erwt3d PUBLIC ${LZ4_LIBRARY})
    target_include_directories(erwt3d PUBLIC ${LZ4_INCLUDE_DIR})
    target_compile_definitions(erwt3d PUBLIC ERWT3D_HAVE_LZ4=1)
endif()
if(HAVE_ZFP)
    target_link_libraries(erwt3d PUBLIC ${ZFP_LIBRARY})
    target_include_directories(erwt3d PUBLIC ${ZFP_INCLUDE_DIR})
    target_compile_definitions(erwt3d PUBLIC ERWT3D_HAVE_ZFP=1)
endif()

# Tools
add_executable(erwt3d_info tools/erwt3d_info.cpp)
target_link_libraries(erwt3d_info PRIVATE erwt3d)

add_executable(erwt3d_convert tools/erwt3d_convert.cpp)
target_link_libraries(erwt3d_convert PRIVATE erwt3d)

add_executable(erwt3d_slice tools/erwt3d_slice.cpp)
target_link_libraries(erwt3d_slice PRIVATE erwt3d)

add_executable(erwt3d_verify tools/erwt3d_verify.cpp)
target_link_libraries(erwt3d_verify PRIVATE erwt3d)

add_executable(erwt3d_bench tools/erwt3d_bench.cpp)
target_link_libraries(erwt3d_bench PRIVATE erwt3d)

add_executable(erwt3d_line tools/erwt3d_line.cpp)
target_link_libraries(erwt3d_line PRIVATE erwt3d)

add_executable(erwt3d_bench_line tools/erwt3d_bench_line.cpp)
target_link_libraries(erwt3d_bench_line PRIVATE erwt3d)

add_executable(gen_test_data tools/gen_test_data.cpp)

add_executable(erwt3d_bench_raw tools/erwt3d_bench_raw.cpp)

add_executable(erwt3d_layout_sim tools/erwt3d_layout_sim.cpp)

add_executable(erwt3d_bench_contest tools/erwt3d_bench_contest.cpp)
target_link_libraries(erwt3d_bench_contest PRIVATE erwt3d)

add_executable(erwt3d_precompute_x tools/erwt3d_precompute_x.cpp)
target_link_libraries(erwt3d_precompute_x PRIVATE erwt3d)

if(ERWT3D_ENABLE_ZFP AND HAVE_ZFP)
    add_executable(erwt3d_scan_zfp_rates tools/erwt3d_scan_zfp_rates.cpp)
    target_link_libraries(erwt3d_scan_zfp_rates PRIVATE erwt3d)
    target_include_directories(erwt3d_scan_zfp_rates PRIVATE ${ZFP_INCLUDE_DIR})

    add_executable(erwt3d_convert_tri tools/erwt3d_convert_tri.cpp)
    target_link_libraries(erwt3d_convert_tri PRIVATE erwt3d)

    add_executable(erwt3d_verify_tri tools/erwt3d_verify_tri.cpp)
    target_link_libraries(erwt3d_verify_tri PRIVATE erwt3d)

    add_executable(erwt3d_bench_tri tools/erwt3d_bench_tri.cpp)
    target_link_libraries(erwt3d_bench_tri PRIVATE erwt3d)
endif()

add_executable(example_api tools/example_api.cpp)
target_link_libraries(example_api PRIVATE erwt3d)

# sw4-style single entry point
add_executable(erwt3d_cli tools/erwt3d_main.cpp)
set_target_properties(erwt3d_cli PROPERTIES OUTPUT_NAME erwt3d)
