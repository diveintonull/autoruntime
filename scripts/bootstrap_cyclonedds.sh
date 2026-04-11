#!/usr/bin/env bash
set -euo pipefail

readonly CYCLONEDDS_VERSION="11.0.1"
readonly CYCLONEDDS_SHA256="c25d46075ad6b5cee564bda9e5f49509e9a6dbb1a8b858e708eb360d335cf973"
readonly CYCLONEDDS_URL="https://codeload.github.com/eclipse-cyclonedds/cyclonedds/tar.gz/refs/tags/${CYCLONEDDS_VERSION}"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly INSTALL_PREFIX="${1:-${PROJECT_DIR}/.deps/cyclonedds-${CYCLONEDDS_VERSION}}"
readonly ARCHIVE="$(mktemp)"
readonly SOURCE_DIR="$(mktemp -d)"
readonly BUILD_DIR="$(mktemp -d)"

cleanup() {
  rm -f -- "${ARCHIVE}"
  rm -rf -- "${SOURCE_DIR}" "${BUILD_DIR}"
}
trap cleanup EXIT

curl --fail --location --retry 3 --output "${ARCHIVE}" "${CYCLONEDDS_URL}"
printf '%s  %s\n' "${CYCLONEDDS_SHA256}" "${ARCHIVE}" |
  sha256sum --check --status
tar --extract --gzip --file "${ARCHIVE}" --strip-components=1 \
  --directory "${SOURCE_DIR}"

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_DDSPERF=OFF \
  -DBUILD_IDLC=ON \
  -DENABLE_SSL=NO \
  -DENABLE_SECURITY=NO
cmake --build "${BUILD_DIR}" --target install

printf 'Cyclone DDS %s installed at %s\n' \
  "${CYCLONEDDS_VERSION}" "${INSTALL_PREFIX}"
