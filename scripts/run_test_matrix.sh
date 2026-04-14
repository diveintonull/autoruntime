#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly CYCLONEDDS_ROOT="${PROJECT_DIR}/.deps/cyclonedds-11.0.1"
readonly JOBS="${AUTORUNTIME_BUILD_JOBS:-2}"

usage() {
  printf 'usage: %s [debug|release|asan|ubsan|tsan|all]\n' "$0"
}

run_profile() {
  local profile="$1"
  local build_type="Debug"
  local sanitizer=""
  local benchmarks="OFF"
  case "${profile}" in
    debug)
      ;;
    release)
      build_type="Release"
      benchmarks="ON"
      ;;
    asan)
      sanitizer="ASan"
      ;;
    ubsan)
      sanitizer="UBSan"
      ;;
    tsan)
      sanitizer="TSan"
      ;;
    *)
      usage
      return 2
      ;;
  esac

  local build_dir="${PROJECT_DIR}/build-verify-${profile}"
  cmake -S "${PROJECT_DIR}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DAUTORUNTIME_SANITIZER="${sanitizer}" \
    -DAUTORUNTIME_ENABLE_FASTIPC=ON \
    -DAUTORUNTIME_ENABLE_DDS=ON \
    -DAUTORUNTIME_BUILD_TESTS=ON \
    -DAUTORUNTIME_BUILD_BENCHMARKS="${benchmarks}"
  cmake --build "${build_dir}" --parallel "${JOBS}"

  case "${profile}" in
    asan)
      ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        ctest --test-dir "${build_dir}" --output-on-failure \
          --output-log "${build_dir}/test.log"
      ;;
    ubsan)
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        ctest --test-dir "${build_dir}" --output-on-failure \
          --output-log "${build_dir}/test.log"
      ;;
    tsan)
      if grep -qi microsoft /proc/version 2>/dev/null; then
        TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
          setarch "$(uname -m)" -R \
          ctest --test-dir "${build_dir}" --output-on-failure \
            --output-log "${build_dir}/test.log"
      else
        TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
          ctest --test-dir "${build_dir}" --output-on-failure \
            --output-log "${build_dir}/test.log"
      fi
      ;;
    *)
      ctest --test-dir "${build_dir}" --output-on-failure \
        --output-log "${build_dir}/test.log"
      ;;
  esac
}

if [[ ! -x "${CYCLONEDDS_ROOT}/bin/idlc" ]]; then
  printf 'Cyclone DDS 11.0.1 is missing. Run %s first.\n' \
    "${SCRIPT_DIR}/bootstrap_cyclonedds.sh" >&2
  exit 1
fi

readonly REQUESTED_PROFILE="${1:-all}"
if [[ "${REQUESTED_PROFILE}" == "all" ]]; then
  for profile in debug release asan ubsan tsan; do
    run_profile "${profile}"
  done
else
  run_profile "${REQUESTED_PROFILE}"
fi
