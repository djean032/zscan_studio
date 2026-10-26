# Headless schema quality gates

Verified on Linux on 2026-10-26 with GCC 16.2.1 and CMake 4.4.4.

## Phase 10 requested checks

- Full schema/workup/fitting integration configured with
  `-fsanitize=address,undefined -fno-omit-frame-pointer`.
- ASan leak detection and halt-on-error enabled; UBSan halt-on-error and stack
  traces enabled.
- Result: **58/58 tests passed**. These include malformed/truncated headers and
  frames, every interrupted scan byte boundary, CRC/SHA failures, unsafe file
  identity, TOML refusal cases, workup, `make_fit_request()`, and derived replay.
- The pinned `zfit_cpp` commit
  `d3612e964ad8b8e8274aa322d32ffbac2b28b706` was configured independently with
  CVODES and fitting enabled. Result: **110/110 acceptance tests passed**.
- `clang-format --dry-run --Werror` passes for first-party headers, sources, and
  tests using the repository's copied `zfit_cpp` format.

Windows configuration and a separate clean-clone/path-traversal gate were
intentionally deferred by the project owner. Unit tests for relative-path and
symlink escape refusal still exist, but they were not counted as a separate
Phase 10 deliverable.

## Published wire references

- `docs/binary_v1.md` is the normative v1 envelope and payload field table.
- `examples/schema/*.toml` are complete parse-tested readable examples for
  session, sample, instrument, run, fit request, and fit result documents.
- `tests/schema/binary_tests.cpp` contains independently generated golden
  header/common-frame bytes and CRC/SHA reference vectors.
- `tests/schema/scan_tests.cpp` contains independently generated scan config
  and position frame bytes.
- `tests/schema/dark_energy_tests.cpp` contains independently generated dark
  and energy frame bytes.
- The golden frame bytes were calculated separately with Python `struct.pack`
  and a bit-at-a-time CRC32C reference, not by the production encoder. Tests
  compare production output byte-for-byte and validate reverse parsing.
- `tests/schema/derived_arrays_tests.cpp` verifies exact binary replay for
  prepared curves and covariance arrays.

Build artifacts under `/tmp/opencode` are disposable and are not part of the
repository.
