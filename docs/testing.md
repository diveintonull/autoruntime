# Build and verification matrix

Date: 2026-08-20

## Local command

Bootstrap the pinned external DDS implementation once, then run one profile or
the whole matrix:

```bash
projects/autoruntime/scripts/bootstrap_cyclonedds.sh
projects/autoruntime/scripts/run_test_matrix.sh all
# or: debug | release | asan | ubsan | tsan
```

Every profile enables the FastIPC adapter, real Cyclone DDS adapter,
distributed sockets, and all tests. Release also builds the experiments.
Build directories and their `test.log` files are ignored local artifacts.

## Recorded result

| Profile | Configuration | Result | Raw log |
| --- | --- | ---: | --- |
| Debug | FastIPC + Cyclone DDS 11.0.1 | 28/28 | [log](evidence/test-debug.log) |
| Release | FastIPC + Cyclone DDS 11.0.1 | 28/28 | [log](evidence/test-release.log) |
| ASan | Debug, address + leak checks | 28/28 | [log](evidence/test-asan.log) |
| UBSan | Debug, halt on first UB | 28/28 | [log](evidence/test-ubsan.log) |
| TSan | Debug, halt on first race/deadlock report | 28/28 | [log](evidence/test-tsan.log) |

Every profile contains 20 separately named fault tests.

Compiler warnings are enabled with `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion`. All recorded builds completed without a warning.

## Sanitizer option

`AUTORUNTIME_SANITIZER` accepts exactly `ASan`, `UBSan`, `TSan`, or
empty. It applies compile and link instrumentation to AutoRuntime, generated
DDS types, the in-tree FastIPC library, tests, and benchmark targets.

The external Cyclone DDS shared library is pinned and validated but is not
rebuilt with each sanitizer. This matrix therefore detects misuse at the
AutoRuntime adapter boundary but is not a sanitizer audit of Cyclone DDS
itself.

## WSL2 TSan note

GCC TSan initially terminated before `main` on this WSL2 host with
`unexpected memory mapping`. Running CTest under

```bash
setarch "$(uname -m)" -R ctest --test-dir BUILD --output-on-failure
```

removes that virtual-address collision. The matrix script applies this wrapper
only when `/proc/version` identifies WSL. After the wrapper, all 28 tests
passed with `halt_on_error=1`; no race or deadlock report was suppressed.

This is an environment launch condition, not a test exclusion. Native Linux
runs CTest directly.

## Continuous integration

The repository-level [CI workflow](../../../.github/workflows/ci.yml) is the
active GitHub Actions entry point. It runs five FastIPC jobs and five
AutoRuntime jobs on Ubuntu 24.04, caches the verified Cyclone DDS 11.0.1
installation, uploads CTest logs, and runs all three Release benchmark smokes.

The workflow files retained under each imported upstream subtree are
provenance artifacts; GitHub does not execute nested `.github/workflows`
directories in this monorepo.

## Evidence boundary

A passing sanitizer run means the exercised tests emitted no finding. It does
not prove the absence of memory bugs or data races in unexecuted schedules.
The fault matrix, repeated distributed tests, and benchmark assertions expand
the exercised state space, but production sign-off still requires long-running
target-hardware soak and network impairment tests.
