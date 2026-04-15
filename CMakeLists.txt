cmake_minimum_required(VERSION 3.20)
project(AutoRuntime VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)

option(AUTORUNTIME_BUILD_TESTS "Build AutoRuntime tests" ON)
option(AUTORUNTIME_BUILD_BENCHMARKS "Build AutoRuntime benchmarks" ON)
option(AUTORUNTIME_BUILD_EXAMPLES "Build AutoRuntime examples" ON)
option(AUTORUNTIME_ENABLE_FASTIPC "Build the FastIPC transport adapter" ON)
option(AUTORUNTIME_ENABLE_DDS "Build the Cyclone DDS transport adapter" ON)
set(AUTORUNTIME_CYCLONEDDS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/cyclonedds-11.0.1"
    CACHE PATH "Cyclone DDS 11.0.1 installation prefix")
option(AUTORUNTIME_BUILD_TCP_BASELINE
       "Build the imported tcp_pubsub baseline target" OFF)

set(AUTORUNTIME_SANITIZER "" CACHE STRING
    "Runtime sanitizer: ASan, UBSan, TSan, or empty")
set_property(CACHE AUTORUNTIME_SANITIZER PROPERTY STRINGS
  "" ASan UBSan TSan)
if(AUTORUNTIME_SANITIZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "AUTORUNTIME_SANITIZER requires GCC or Clang")
  endif()
  if(AUTORUNTIME_SANITIZER STREQUAL "ASan")
    set(AUTORUNTIME_SANITIZER_FLAGS
      -fsanitize=address -fno-omit-frame-pointer)
  elseif(AUTORUNTIME_SANITIZER STREQUAL "UBSan")
    set(AUTORUNTIME_SANITIZER_FLAGS
      -fsanitize=undefined -fno-omit-frame-pointer
      -fno-sanitize-recover=all)
  elseif(AUTORUNTIME_SANITIZER STREQUAL "TSan")
    set(AUTORUNTIME_SANITIZER_FLAGS
      -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR
      "AUTORUNTIME_SANITIZER must be ASan, UBSan, TSan, or empty")
  endif()
  add_compile_options(${AUTORUNTIME_SANITIZER_FLAGS})
  add_link_options(${AUTORUNTIME_SANITIZER_FLAGS})
endif()

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

if(AUTORUNTIME_ENABLE_FASTIPC)
  set(FASTIPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FASTIPC_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../fastipc"
    "${CMAKE_BINARY_DIR}/fastipc" EXCLUDE_FROM_ALL)
endif()

if(AUTORUNTIME_ENABLE_DDS)
  list(PREPEND CMAKE_PROGRAM_PATH
    "${AUTORUNTIME_CYCLONEDDS_ROOT}/bin")
  find_package(CycloneDDS 11.0.1 CONFIG REQUIRED
    PATHS "${AUTORUNTIME_CYCLONEDDS_ROOT}"
    NO_DEFAULT_PATH)
  idlc_generate(
    TARGET autoruntime_dds_types
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/transport/dds/autoruntime_message.idl"
    WARNINGS no-implicit-extensibility)
endif()

add_library(autoruntime STATIC
  scheduler/src/executor.cpp
  runtime/src/node.cpp
  health/src/health_monitor.cpp
  observability/src/observability.cpp
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

if(UNIX)
  target_sources(autoruntime PRIVATE
    distributed/src/discovery.cpp
    distributed/src/rpc.cpp)
  target_include_directories(autoruntime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/distributed/include")
  target_compile_definitions(autoruntime PUBLIC
    AUTORUNTIME_HAS_DISTRIBUTED=1)
endif()

function(autoruntime_enable_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  endif()
endfunction()
autoruntime_enable_warnings(autoruntime)
if(AUTORUNTIME_ENABLE_FASTIPC)
  target_sources(autoruntime PRIVATE transport/src/fastipc_transport.cpp)
  target_link_libraries(autoruntime PUBLIC FastIPC::fastipc)
  target_compile_definitions(autoruntime PUBLIC AUTORUNTIME_HAS_FASTIPC=1)
endif()

if(AUTORUNTIME_ENABLE_DDS)
  target_sources(autoruntime PRIVATE transport/src/dds_transport.cpp)
  target_link_libraries(
    autoruntime PRIVATE autoruntime_dds_types CycloneDDS::ddsc)
  target_compile_definitions(autoruntime PUBLIC AUTORUNTIME_HAS_DDS=1)
endif()

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

  add_executable(autoruntime_health_monitor_test
    tests/health_monitor_test.cpp)
  target_link_libraries(autoruntime_health_monitor_test
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_health_monitor_test)
  add_test(NAME autoruntime.health_monitor
    COMMAND autoruntime_health_monitor_test)

  add_executable(autoruntime_observability_test
    tests/observability_test.cpp)
  target_link_libraries(autoruntime_observability_test
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_observability_test)
  add_test(NAME autoruntime.observability
    COMMAND autoruntime_observability_test)

  if(AUTORUNTIME_ENABLE_FASTIPC)
    add_executable(autoruntime_fastipc_transport_test
      tests/fastipc_transport_test.cpp)
    target_link_libraries(autoruntime_fastipc_transport_test
      PRIVATE AutoRuntime::autoruntime Threads::Threads)
    autoruntime_enable_warnings(autoruntime_fastipc_transport_test)
    add_test(NAME autoruntime.fastipc_transport
      COMMAND autoruntime_fastipc_transport_test)
  endif()

  if(AUTORUNTIME_ENABLE_DDS)
    add_executable(autoruntime_dds_transport_test
      tests/dds_transport_test.cpp)
    target_link_libraries(autoruntime_dds_transport_test
      PRIVATE AutoRuntime::autoruntime Threads::Threads)
    autoruntime_enable_warnings(autoruntime_dds_transport_test)
    add_test(NAME autoruntime.dds_transport
      COMMAND autoruntime_dds_transport_test)
    set_tests_properties(autoruntime.dds_transport PROPERTIES TIMEOUT 20)
  endif()

  if(UNIX)
    add_executable(autoruntime_distributed_test
      tests/distributed_test.cpp)
    target_link_libraries(autoruntime_distributed_test
      PRIVATE AutoRuntime::autoruntime Threads::Threads)
    autoruntime_enable_warnings(autoruntime_distributed_test)
    add_test(NAME autoruntime.distributed
      COMMAND autoruntime_distributed_test)
    set_tests_properties(autoruntime.distributed PROPERTIES TIMEOUT 20)

    add_executable(autoruntime_fault_injection_test
      tests/fault_injection_test.cpp)
    target_link_libraries(autoruntime_fault_injection_test
      PRIVATE AutoRuntime::autoruntime Threads::Threads)
    autoruntime_enable_warnings(autoruntime_fault_injection_test)

    set(AUTORUNTIME_FAULT_CASES
      transport_closed
      queue_overflow
      slow_callback_isolation
      deadline_miss
      cancelled_task
      heartbeat_loss
      no_progress
      backlog_degraded
      process_exit
      stale_generation
      rpc_disconnect
      rpc_corrupt_frame
      duplicate_membership
      service_timeout
      message_delay
      message_drop
      slow_consumer
      node_restart
      shutdown_during_load)
    foreach(fault_case IN LISTS AUTORUNTIME_FAULT_CASES)
      add_test(NAME autoruntime.fault.${fault_case}
        COMMAND autoruntime_fault_injection_test ${fault_case})
      set_tests_properties(autoruntime.fault.${fault_case}
        PROPERTIES LABELS fault TIMEOUT 5)
    endforeach()

    if(AUTORUNTIME_ENABLE_DDS)
      add_test(NAME autoruntime.fault.dds_participant_loss
        COMMAND autoruntime_fault_injection_test dds_participant_loss)
      set_tests_properties(autoruntime.fault.dds_participant_loss
        PROPERTIES LABELS fault TIMEOUT 5)
    endif()
  endif()
endif()

if(AUTORUNTIME_BUILD_BENCHMARKS AND AUTORUNTIME_ENABLE_DDS)
  add_executable(autoruntime_dds_qos_experiment
    benchmarks/dds_qos_experiment.cpp)
  target_link_libraries(autoruntime_dds_qos_experiment
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_dds_qos_experiment)
endif()

if(AUTORUNTIME_BUILD_BENCHMARKS)
  add_executable(autoruntime_pipeline_latency_benchmark
    benchmarks/pipeline_latency_benchmark.cpp)
  target_link_libraries(autoruntime_pipeline_latency_benchmark
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_pipeline_latency_benchmark)

  add_executable(autoruntime_scheduling_isolation_experiment
    benchmarks/scheduling_isolation_experiment.cpp)
  target_link_libraries(autoruntime_scheduling_isolation_experiment
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_scheduling_isolation_experiment)
endif()

if(AUTORUNTIME_BUILD_EXAMPLES AND UNIX)
  add_executable(autoruntime_pipeline_demo
    examples/pipeline_demo.cpp)
  target_link_libraries(autoruntime_pipeline_demo
    PRIVATE AutoRuntime::autoruntime Threads::Threads)
  autoruntime_enable_warnings(autoruntime_pipeline_demo)
  if(BUILD_TESTING AND AUTORUNTIME_BUILD_TESTS)
    add_test(NAME autoruntime.example.pipeline
      COMMAND autoruntime_pipeline_demo)
    set_tests_properties(autoruntime.example.pipeline PROPERTIES TIMEOUT 5)
  endif()
endif()
