cmake_minimum_required(VERSION 3.16)
project(exchange CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release: full optimisation + LTO
# Debug:   sanitisers + no optimisation
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -flto)
else()
    add_compile_options(-g -O0 -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Boost (header-only Asio + Boost.System for error_code)
find_package(Boost 1.74 REQUIRED COMPONENTS system)

add_executable(exchange src/main.cpp)

target_include_directories(exchange PRIVATE include)

target_link_libraries(exchange
    PRIVATE
    Boost::system
    pthread
)

# Optional: latency benchmark target
add_executable(bench_lob src/bench_lob.cpp)
target_include_directories(bench_lob PRIVATE include)
target_link_libraries(bench_lob PRIVATE pthread)
target_compile_options(bench_lob PRIVATE -O3 -march=native)
