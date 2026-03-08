cmake_minimum_required(VERSION 3.16)
project(vcd_toggle_profiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(VCD_TOGGLE_PROFILER_BUILD_TESTS "Build C++ tests with vendored GoogleTest" OFF)

add_executable(vcd-toggle-profiler
  src/main.cpp
)

target_include_directories(vcd-toggle-profiler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include
)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(vcd-toggle-profiler PRIVATE -Wall -Wextra)
  target_compile_options(vcd-toggle-profiler PRIVATE
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-mtune=native>
    $<$<CONFIG:Release>:-flto>
  )
  target_link_options(vcd-toggle-profiler PRIVATE
    $<$<CONFIG:Release>:-flto>
  )
endif()

if (VCD_TOGGLE_PROFILER_BUILD_TESTS)
  include(CTest)
  enable_testing()

  if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    message(FATAL_ERROR "Vendored GoogleTest not found at third_party/googletest")
  endif()

  set(INSTALL_GTEST OFF CACHE BOOL "Disable gtest install" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "Disable gmock" FORCE)
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

  add_executable(vcd-toggle-profiler-tests
    tests/smoke_test.cpp
  )
  target_link_libraries(vcd-toggle-profiler-tests PRIVATE GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(vcd-toggle-profiler-tests)
endif()
