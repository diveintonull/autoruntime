# Build and verification matrix

Date: 2026-08-20

## Verified revision and host

| Field | Value |
| --- | --- |
| Full-matrix revision | `4e60e2192a113688070c19a8568d671db46b4896` |
| Host | Ubuntu 24.04.4 under WSL2 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Architecture | x86_64 |
| Compiler | GNU 13.3.0 |
| Generator | Ninja 1.11.1 |
| CMake | 3.28.3 |
| External DDS | Cyclone DDS 11.0.1, pinned bootstrap |

## Dependency-light default

The exact ordinary workflow is:

```bash
cmake -S projects/autoruntime -B projects/autoruntime/build -G Ninja
cmake --build projects/autoruntime/build
ctest --test-dir projects/autoruntime/build --output-on-failure
```

DDS defaults off. A fresh Debug directory built all default targets and passed
26/26 tests: [raw log](evidence/test-default.log). It includes FastIPC,
distributed sockets, 19 non-DDS fault cases, and the runnable pipeline example.

## Full local command

Bootstrap the pinned external DDS implementation once, then run one profile or
the whole matrix:

```bash
projects/autoruntime/scripts/bootstrap_cyclonedds.sh
projects/autoruntime/scripts/run_test_matrix.sh all
# or: debug | release | asan | ubsan | tsan
```

Every full profile enables the FastIPC adapter, real Cyclone DDS adapter,
distributed sockets, examples, and all tests. Release also builds the
experiments.

## Recorded full-matrix result

| Profile | Configuration | Result | Raw log |
| --- | --- | ---: | --- |
| Debug | FastIPC + Cyclone DDS 11.0.1 | 28/28 | [log](evidence/test-debug.log) |
| Release | FastIPC + Cyclone DDS 11.0.1 | 28/28 | [log](evidence/test-release.log) |
| ASan | Debug, address + leak checks | 28/28 | [log](evidence/test-asan.log) |
| UBSan | Debug, halt on first UB | 28/28 | [log](evidence/test-ubsan.log) |
| TSan | Debug, halt on first race/deadlock report | 28/28 | [log](evidence/test-tsan.log) |

The five profiles executed 140/140 registered CTest entries. Every profile
contains 20 separately named fault cases. The real FastIPC
SIGKILL/restart/data-flow test also passed ten consecutive Release iterations:
[repeat log](evidence/recovery-repeat-10.log).

Compiler warnings are enabled with `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion`. The recorded builds completed without a warning.

## Sanitizer scope

`AUTORUNTIME_SANITIZER` accepts exactly `ASan`, `UBSan`, `TSan`, or
empty. It instruments AutoRuntime, generated DDS types, the in-tree FastIPC
library, tests, and benchmark targets. The pinned external Cyclone DDS shared
library is not rebuilt under each sanitizer.

The stronger crash/restart integration exposed a real FastIPC
receive-versus-`munmap` bug under ASan. Commit `2bdd95f` added an
active-operation lease and direct regression; this recorded matrix is after the
fix. A passing sanitizer run means no finding in these exercised schedules,
not proof of absence.

## WSL2 TSan note

GCC TSan initially terminated before `main` on this WSL2 host with
`unexpected memory mapping`. Running CTest under:

```bash
setarch "$(uname -m)" -R ctest --test-dir BUILD --output-on-failure
```

avoids that virtual-address collision. The script applies the wrapper only on
WSL; native Linux runs CTest directly. No race or deadlock report is filtered.

## Continuous integration

The root [CI workflow](../../../.github/workflows/ci.yml) runs five FastIPC and
five AutoRuntime Ubuntu 24.04 jobs, caches the verified Cyclone DDS install,
uploads CTest logs, and runs all three Release benchmark smokes. Nested
upstream workflow files are provenance artifacts and are not active monorepo
entry points.

## Evidence boundary

Production sign-off still requires target-hardware soak, real network
impairment, security review, ABI/package validation, and deterministic
real-time analysis. The matrix establishes reproducible behavior for the
checked configurations only.
