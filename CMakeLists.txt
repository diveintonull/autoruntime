cmake_minimum_required(VERSION 3.20)
project(AutoRuntime VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

option(AUTORUNTIME_BUILD_TESTS "Build AutoRuntime tests" ON)
option(AUTORUNTIME_BUILD_BENCHMARKS "Build AutoRuntime benchmarks" ON)
option(AUTORUNTIME_BUILD_TCP_BASELINE
       "Build the imported tcp_pubsub baseline target" OFF)

if(AUTORUNTIME_BUILD_TCP_BASELINE)
  if(NOT EXISTS
     "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/asio/asio/asio/include/asio.hpp"
     OR NOT EXISTS
     "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/recycle/recycle/src/recycle/shared_pool.hpp")
    message(FATAL_ERROR
      "AUTORUNTIME_BUILD_TCP_BASELINE requires the pinned asio and recycle "
      "submodules. Initialize them at the imported revisions.")
  endif()
  list(APPEND CMAKE_MODULE_PATH
       "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/asio/Module"
       "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/recycle/Module")
  include("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/asio/build-asio.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/recycle/build-recycle.cmake")
  add_subdirectory(tcp_pubsub)
endif()

add_library(autoruntime STATIC
  scheduler/src/executor.cpp
  runtime/src/node.cpp
  transport/src/in_memory_transport.cpp)
add_library(AutoRuntime::autoruntime ALIAS autoruntime)
target_compile_features(autoruntime PUBLIC cxx_std_20)
target_link_libraries(autoruntime PUBLIC Threads::Threads)
target_include_directories(
  autoruntime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/scheduler/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/transport/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/health/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/observability/include")

function(autoruntime_enable_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  endif()
endfunction()
autoruntime_enable_warnings(autoruntime)

if(BUILD_TESTING AND AUTORUNTIME_BUILD_TESTS)
  add_executable(autoruntime_executor_test tests/executor_test.cpp)
  target_link_libraries(autoruntime_executor_test
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_executor_test)
  add_test(NAME autoruntime.executor COMMAND autoruntime_executor_test)

  add_executable(autoruntime_runtime_api_test tests/runtime_api_test.cpp)
  target_link_libraries(autoruntime_runtime_api_test
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_runtime_api_test)
  add_test(NAME autoruntime.runtime_api COMMAND autoruntime_runtime_api_test)
endif()
