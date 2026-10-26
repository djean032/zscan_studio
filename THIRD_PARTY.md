# Third-party dependencies

Development dependencies and versions for the initial schema slice:

| Dependency | Pin / selection | License | Use |
| --- | --- | --- | --- |
| `zfit_cpp` | submodule `d3612e964ad8b8e8274aa322d32ffbac2b28b706` | GPL-3.0-or-later intended; verify upstream license file before redistributing | Future workup and fit integration; not linked yet |
| toml++ | `v3.4.0`, commit `30172438cee64926dc41fdd9c11fb3ba5b2ba9de` | MIT | Future TOML metadata I/O |
| OpenSSL | system `OpenSSL::Crypto` | Apache-2.0 | SHA-256 and UUID randomness |
| Catch2 | `v3.7.1` | BSL-1.0 | Tests only |

The exact system OpenSSL version and bundled licenses/notices must be captured
in each distributable build. `zfit_cpp` currently has no `LICENSE` file at its
pinned commit: resolve its licensing/redistribution terms before any public
source distribution or installer. Plotting, Qt, NI-DAQmx, SUNDIALS, cminpack,
and Eigen licensing still need review before those components are packaged.
