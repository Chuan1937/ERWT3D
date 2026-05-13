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
    src/sb_parallel.cpp
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
if(ERWT3D_BUILD_TESTS)
    enable_testing()
    
    add_executable(test_morton tests/test_morton.cpp)
    target_link_libraries(test_morton PRIVATE erwt3d)
    add_test(NAME test_morton COMMAND test_morton)
    
    add_executable(test_roundtrip tests/test_roundtrip.cpp)
    target_link_libraries(test_roundtrip PRIVATE erwt3d)
    add_test(NAME test_roundtrip COMMAND test_roundtrip)
    
    add_executable(test_slice tests/test_slice.cpp)
    target_link_libraries(test_slice PRIVATE erwt3d)
    add_test(NAME test_slice COMMAND test_slice)
    
    add_executable(test_backend tests/test_backend.cpp)
    target_link_libraries(test_backend PRIVATE erwt3d)
    add_test(NAME test_backend COMMAND test_backend)
    
    add_executable(test_sb_parallel_modes tests/test_sb_parallel_modes.cpp)
    target_link_libraries(test_sb_parallel_modes PRIVATE erwt3d)
    add_test(NAME test_sb_parallel_modes COMMAND test_sb_parallel_modes)
    
    add_executable(test_panel tests/test_panel.cpp)
    target_link_libraries(test_panel PRIVATE erwt3d)
    add_test(NAME test_panel COMMAND test_panel)
    
    add_executable(test_sb_read_modes tests/test_sb_read_modes.cpp)
    target_link_libraries(test_sb_read_modes PRIVATE erwt3d)
    add_test(NAME test_sb_read_modes COMMAND test_sb_read_modes)
    
    add_executable(test_leaf_index tests/test_leaf_index.cpp)
    target_link_libraries(test_leaf_index PRIVATE erwt3d)
    add_test(NAME test_leaf_index COMMAND test_leaf_index)
    
    add_executable(test_line_reads tests/test_line_reads.cpp)
    target_link_libraries(test_line_reads PRIVATE erwt3d)
    add_test(NAME test_line_reads COMMAND test_line_reads)
endif()

add_executable(erwt3d_layout_sim tools/erwt3d_layout_sim.cpp)
