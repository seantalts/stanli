cmake_minimum_required(VERSION 3.21)
project(stanli_allocator_gate LANGUAGES C CXX)
# This standalone test always exercises the private allocator, independently
# of the parent project's platform-specific defaults.
set(STANLI_SHARED_ALLOCATOR MIMALLOC CACHE STRING "Allocator under test")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
include(../../cmake/StanliAllocator.cmake)
if(NOT STANLI_USE_PRIVATE_MIMALLOC)
  message(FATAL_ERROR "This gate must test the private allocator, not silently select SYSTEM")
endif()
file(GLOB _eigen "${STANLI_ALLOCATOR_SOURCE_DIR}/deps/math/lib/eigen_*")
add_library(stanmath INTERFACE)
target_include_directories(stanmath INTERFACE ${_eigen})
include(../../cmake/StanliAllocatorTests.cmake)
stanli_add_allocator_probe()
target_compile_definitions(test_private_allocator PRIVATE STANLI_ALLOCATOR_PROBE_ONLY=1)
enable_testing()
add_test(NAME private_allocator_ownership COMMAND test_private_allocator
  --probe-only $<TARGET_FILE:stanli_allocator_test_probe>)
add_test(NAME allocator_config COMMAND "${CMAKE_COMMAND}"
  -DSOURCE=${STANLI_ALLOCATOR_SOURCE_DIR}
  -P "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/test_allocator_config.cmake")
