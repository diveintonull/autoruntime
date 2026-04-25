cmake_minimum_required(VERSION 3.20)
project(autoruntime_rclcpp_baseline VERSION 0.1.0 LANGUAGES CXX)
include(CTest)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmw REQUIRED)
find_package(std_msgs REQUIRED)
find_package(Git QUIET)

set(RCLCPP_BASELINE_SOURCE_REVISION "unknown" CACHE STRING
    "Git revision recorded in rclcpp benchmark output")
if(Git_FOUND AND RCLCPP_BASELINE_SOURCE_REVISION STREQUAL "unknown")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE RCLCPP_BASELINE_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(RCLCPP_BASELINE_GIT_REVISION)
    set(RCLCPP_BASELINE_SOURCE_REVISION
        "${RCLCPP_BASELINE_GIT_REVISION}")
  endif()
endif()

add_executable(rclcpp_comparative_benchmark
  src/rclcpp_comparative_benchmark.cpp)
target_compile_features(rclcpp_comparative_benchmark PRIVATE cxx_std_20)
target_compile_definitions(
  rclcpp_comparative_benchmark PRIVATE
  RCLCPP_BASELINE_SOURCE_REVISION="${RCLCPP_BASELINE_SOURCE_REVISION}"
  RCLCPP_BASELINE_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
  RCLCPP_BASELINE_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rclcpp_comparative_benchmark PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
ament_target_dependencies(
  rclcpp_comparative_benchmark rclcpp rmw std_msgs)
if(BUILD_TESTING)
  add_test(
    NAME autoruntime.rclcpp.sequential_cases
    COMMAND rclcpp_comparative_benchmark
      --payload 64
      --frequency 50
      --duration-ms 200
      --warmup 3
      --trials 3
      --run-id sequential-regression)
  set_tests_properties(
    autoruntime.rclcpp.sequential_cases
    PROPERTIES TIMEOUT 30 LABELS "benchmark;comparative")
endif()

install(
  TARGETS rclcpp_comparative_benchmark
  DESTINATION lib/${PROJECT_NAME})

ament_package()
