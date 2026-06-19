cmake_minimum_required(VERSION 3.16)
project(ERWT3D VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(ERWT3D_BUILD_TESTS "Build tests" ON)
option(ERWT3D_NATIVE_OPT "Experimental: use -march=native -mtune=native (Release only)" OFF)

# Thread support
find_package(Threads REQUIRED)

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
    src/sb_ssd.cpp
    src/sb_hdd.cpp
    src/sb_panel.cpp
    src/api.cpp
)

# Create library
add_library(erwt3d STATIC ${LIB_SOURCES})
target_include_directories(erwt3d PUBLIC include)
target_link_libraries(erwt3d PUBLIC Threads::Threads)

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

add_executable(gen_test_data tools/gen_test_data.cpp)

add_executable(erwt3d_bench_raw tools/erwt3d_bench_raw.cpp)

# Tests
if(ERWT3D_BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests")
    enable_testing()
    file(GLOB TEST_SOURCES tests/test_*.cpp)
    foreach(TEST_SRC ${TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SRC})
        target_link_libraries(${TEST_NAME} PRIVATE erwt3d)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()

add_executable(erwt3d_layout_sim tools/erwt3d_layout_sim.cpp)

add_executable(erwt3d_bench_contest tools/erwt3d_bench_contest.cpp)
target_link_libraries(erwt3d_bench_contest PRIVATE erwt3d)

add_executable(example_api tools/example_api.cpp)
target_link_libraries(example_api PRIVATE erwt3d)
