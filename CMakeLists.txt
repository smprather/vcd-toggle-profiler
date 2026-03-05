cmake_minimum_required(VERSION 3.16)
project(vcd_toggle_profiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vcd-toggle-profiler
  src/main.cpp
)

target_include_directories(vcd-toggle-profiler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include
)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(vcd-toggle-profiler PRIVATE -Wall -Wextra)
endif()
