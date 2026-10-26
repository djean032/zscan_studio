# ZScan Studio (C++)

Qt 6 Widgets rewrite of ZScan Studio. The first implementation slice is a
headless C++23 library for the versioned session schema; see [PLANS.md](PLANS.md)
for the scientific and persistence contract. No hardware or GUI is required to
build this slice.

```sh
git clone --recurse-submodules <repository-url>
cmake --preset schema
cmake --build --preset schema
ctest --preset schema
```

The `zfit_cpp` submodule is pinned to a tested commit. Initial schema-only builds
do not enable CVODES or fitting. Requires CMake 3.20+, a C++23 compiler, and
OpenSSL development headers; tests also fetch pinned Catch2. A later phase adds
TOML reading and the optional GUI/hardware targets.

This project is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
