#pragma once

#include "zscan/schema/dark_energy.hpp"
#include "zscan/schema/derived_arrays.hpp"

#include <optional>

namespace zscan::schema::storage {

struct Seal {
    std::uint64_t byte_size { };
    Sha256Digest sha256;
};

struct InterruptedScan {
    binary::Header header;
    Seal original; // SHA-256 covers ALL bytes, including the invalid tail.
    std::uint64_t valid_prefix_byte_size { }; // Includes the 80-byte header.
    std::uint64_t valid_frame_count { };
    std::optional<SchemaError> tail_error;
};

[[nodiscard]] Result<InterruptedScan> inspect_interrupted_scan(
    std::span<const std::uint8_t> original, binary::ResourceLimits limits = { });

// Freeze an in-progress or orphan scan exactly as found and atomically record
// it as interrupted. Header identity/time must match the supplied metadata.
[[nodiscard]] Result<ScanRecord> recover_interrupted_scan(const std::filesystem::path& session_root,
    ScanRecord metadata, std::uint64_t max_bytes = 512ull * 1024 * 1024);

// Creation is exclusive: an existing scan is never overwritten. Writes a
// prevalidated sealed scan in frame order, syncs after POSITION_END and at
// completion, then rereads and validates the exact bytes from disk.
[[nodiscard]] Result<Seal> write_sealed_scan(const std::filesystem::path& session_root,
    const SessionPath& path, const binary::ScanFile& scan);

// Submit an energy burst to an exclusive temporary path, sync and validate it,
// then publish the finished bytes without overwriting an existing burst.
[[nodiscard]] Result<Seal> write_energy_file_atomic(const std::filesystem::path& session_root,
    const SessionPath& path, const binary::EnergyFile& energy);
[[nodiscard]] Result<Seal> write_derived_arrays_atomic(const std::filesystem::path& session_root,
    const SessionPath& path, const binary::DerivedArrayFile& arrays);

// Read with a size cap before allocation; refuse altered bytes or sizes.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_sealed_file(
    const std::filesystem::path& session_root, const SessionPath& path, const Seal& seal,
    std::uint64_t max_bytes = 512ull * 1024 * 1024);

// No manifest exists -> revision 1; otherwise new revision must be exactly
// previous + 1. Publishes via a validated temporary file and atomic rename.
[[nodiscard]] Result<void> write_session_manifest_atomic(
    const std::filesystem::path& session_root, const SessionManifest& manifest);
[[nodiscard]] Result<SessionManifest> read_session_manifest(
    const std::filesystem::path& session_root);

} // namespace zscan::schema::storage
