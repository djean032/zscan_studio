# Session fixtures

- `valid/` is a production-valid, read-only session manifest with complete
  sample and instrument snapshots and no curve mappings yet. It is useful for
  testing session open/validation UI without requiring a measurement campaign.
- `invalid/` is identical except for an unknown field in the `curves` array;
  the validator must report that error.

The binary files under each directory are complete schema-v1 scan/dark/energy
fixtures available for later curve-mapping tests; the current minimal valid
manifest intentionally does not reference them.
