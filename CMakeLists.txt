cmake_minimum_required(VERSION 3.16)
project(ERWT3D VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(ERWT3D_BUILD_TESTS "Build tests" ON)

# Thread support
find_package(Threads REQUIRED)

# Library sources
set(LIB_SOURCES
    src/morton.cpp
    src/writer.cpp
    src/reader.cpp
    src/slice.cpp
    src/extent.cpp
    src/cache.cpp
    src/thread_pool.cpp
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
endif()