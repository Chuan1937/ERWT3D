cmake_minimum_required(VERSION 3.16)
project(ERWT3D VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options. Portable x86-64 is the default for competition binaries because
# the CentOS 7 evaluation CPU model is not fixed. Developers may explicitly
# enable native tuning for machine-local benchmarks.
option(ERWT3D_NATIVE_OPT "Use -march=native -mtune=native (Release only)" OFF)
option(ERWT3D_ENABLE_RZFP "Enable experimental ZFP pointwise-relative codec" OFF)

# Thread support
find_package(Threads REQUIRED)

# Optional ZFP dependency for RZFP codec
set(ERWT3D_ZFP_TARGET "")
if(ERWT3D_ENABLE_RZFP)
    find_package(zfp CONFIG QUIET)
    if(TARGET zfp::zfp)
        set(ERWT3D_ZFP_TARGET zfp::zfp)
        message(STATUS "Found ZFP via CMake package: zfp::zfp")
    else()
        find_path(ERWT3D_ZFP_INCLUDE_DIR NAMES zfp.h)
        find_library(ERWT3D_ZFP_LIBRARY NAMES zfp)
        if(NOT ERWT3D_ZFP_INCLUDE_DIR OR NOT ERWT3D_ZFP_LIBRARY)
            message(FATAL_ERROR
                "ERWT3D_ENABLE_RZFP=ON but ZFP was not found "
                "(neither CMake package nor headers/library)")
        endif()
        add_library(erwt3d_zfp_external INTERFACE)
        target_include_directories(erwt3d_zfp_external INTERFACE ${ERWT3D_ZFP_INCLUDE_DIR})
        target_link_libraries(erwt3d_zfp_external INTERFACE ${ERWT3D_ZFP_LIBRARY})
        set(ERWT3D_ZFP_TARGET erwt3d_zfp_external)
        message(STATUS "Found ZFP manually: ${ERWT3D_ZFP_LIBRARY}")
    endif()
endif()

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
else()
    message(STATUS "Portable x86-64 build enabled; ERWT3D_NATIVE_OPT=OFF")
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
    src/device_profile.cpp
    src/memory_budget.cpp
    src/window_cache.cpp
    src/sb_panel.cpp
    src/api.cpp
    src/lz4_probe.cpp
    src/lz4_xp_sidecar.cpp
    src/lz4_axis_plane_writer.cpp
    src/rzfp_axis_plane_writer.cpp
    src/auto_plan.cpp
    src/contest_positions.cpp
    src/contest_groups.cpp
    src/file_format_detect.cpp
    src/embedded_sections.cpp
    src/io_profile.cpp
    src/unified_read_config.cpp
    src/ssd/ssd_extent_planner.cpp
    src/ssd/ssd_executor.cpp
    src/ssd_cold/cold_request_plan.cpp
    src/ssd_cold/cold_extent_plan.cpp
    src/ssd_cold/cold_io_engine.cpp
    src/ssd_cold/cold_buffer_pool.cpp
    src/ssd_cold/cold_profile.cpp
    src/ssd_cold/rzfp_cold_executor.cpp
    src/ssd_cold/lz4_cold_executor.cpp
)

if(ERWT3D_ENABLE_RZFP)
    list(APPEND LIB_SOURCES
        src/relative_error.cpp
        src/rzfp_codec.cpp
        src/rzfp_axis_leaf.cpp
        src/rzfp_writer.cpp
        src/rzfp_reader.cpp
        src/rzfp_strategy.cpp
        src/rzfp_round_plan.cpp
        src/contest_phase_plan.cpp
        src/contest_round_executor.cpp
        src/rzfp_xplane_codec.cpp
        src/rzfp_xplane_writer.cpp
        src/rzfp_raw_sampler.cpp
        src/rzfp_auto_plan.cpp
    )
endif()

# Create library
add_library(erwt3d STATIC ${LIB_SOURCES})
target_include_directories(erwt3d PUBLIC include)
target_link_libraries(erwt3d PUBLIC Threads::Threads)
if(HAVE_LZ4)
    target_link_libraries(erwt3d PUBLIC ${LZ4_LIBRARY})
    target_include_directories(erwt3d PUBLIC ${LZ4_INCLUDE_DIR})
    target_compile_definitions(erwt3d PUBLIC ERWT3D_HAVE_LZ4=1)
endif()
if(ERWT3D_ENABLE_RZFP)
    target_link_libraries(erwt3d PUBLIC ${ERWT3D_ZFP_TARGET})
    target_compile_definitions(erwt3d PUBLIC ERWT3D_HAVE_RZFP=1)
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

add_executable(gen_fast_data tools/gen_fast_data.cpp)

add_executable(erwt3d_bench_raw tools/erwt3d_bench_raw.cpp)

add_executable(erwt3d_layout_sim tools/erwt3d_layout_sim.cpp)

add_executable(erwt3d_bench_contest tools/erwt3d_bench_contest.cpp)
target_link_libraries(erwt3d_bench_contest PRIVATE erwt3d)

add_executable(erwt3d_precompute_x tools/erwt3d_precompute_x.cpp)
target_link_libraries(erwt3d_precompute_x PRIVATE erwt3d)

add_executable(erwt3d_bench_output_floor tools/experimental/erwt3d_bench_output_floor.cpp)

add_executable(erwt3d_bench_codec tools/experimental/erwt3d_bench_codec.cpp)
target_link_libraries(erwt3d_bench_codec PRIVATE erwt3d)

add_executable(erwt3d_bench_xplane_raw tools/experimental/erwt3d_bench_xplane_raw.cpp)
target_link_libraries(erwt3d_bench_xplane_raw PRIVATE erwt3d)

add_executable(erwt3d_append_rawx tools/experimental/erwt3d_append_rawx.cpp)
target_link_libraries(erwt3d_append_rawx PRIVATE erwt3d)

add_executable(example_api tools/example_api.cpp)
target_link_libraries(example_api PRIVATE erwt3d)

# sw4-style single entry point
add_executable(erwt3d_cli tools/erwt3d_main.cpp)
set_target_properties(erwt3d_cli PROPERTIES OUTPUT_NAME erwt3d)

# RZFP tools and tests
if(ERWT3D_ENABLE_RZFP)
    add_executable(erwt3d_rzfp_probe tools/erwt3d_rzfp_probe.cpp)
    target_link_libraries(erwt3d_rzfp_probe PRIVATE erwt3d)

    add_executable(erwt3d_rzfp_xplane_probe tools/erwt3d_rzfp_xplane_probe.cpp)
    target_link_libraries(erwt3d_rzfp_xplane_probe PRIVATE erwt3d)

    add_executable(erwt3d_rzfp_xplane_gen tools/erwt3d_rzfp_xplane_gen.cpp)
    target_link_libraries(erwt3d_rzfp_xplane_gen PRIVATE erwt3d)

    add_executable(erwt3d_rzfp_axis_repack tools/erwt3d_rzfp_axis_repack.cpp)
    target_link_libraries(erwt3d_rzfp_axis_repack PRIVATE erwt3d)

    add_executable(erwt3d_axis_plane_gen tools/erwt3d_axis_plane_gen.cpp)
    target_link_libraries(erwt3d_axis_plane_gen PRIVATE erwt3d)

    add_executable(erwt3d_convert_rzfp tools/erwt3d_convert_rzfp.cpp)
    target_link_libraries(erwt3d_convert_rzfp PRIVATE erwt3d)

    add_executable(erwt3d_verify_rzfp tools/erwt3d_verify_rzfp.cpp)
    target_link_libraries(erwt3d_verify_rzfp PRIVATE erwt3d)

    add_executable(erwt3d_bench_rzfp tools/erwt3d_bench_rzfp.cpp)
    target_link_libraries(erwt3d_bench_rzfp PRIVATE erwt3d)

    add_executable(erwt3d_contest tools/erwt3d_contest.cpp)
    target_link_libraries(erwt3d_contest PRIVATE erwt3d)

    add_executable(erwt3d_rzfp_auto tools/erwt3d_rzfp_auto.cpp)
    target_link_libraries(erwt3d_rzfp_auto PRIVATE erwt3d)

    add_executable(erwt3d_lz4_probe tools/erwt3d_lz4_probe.cpp)
    target_link_libraries(erwt3d_lz4_probe PRIVATE erwt3d)

    add_executable(erwt3d_auto_plan tools/erwt3d_auto_plan.cpp)
    target_link_libraries(erwt3d_auto_plan PRIVATE erwt3d)

    enable_testing()
    add_subdirectory(tests)
endif()
