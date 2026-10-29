#include "zscan/schema/storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::binary;
using namespace zscan::schema::storage;

ScanFile scan_fixture()
{
    ScanFile scan;
    scan.header = { FileKind::Scan, *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T14:03:00Z") };
    scan.config = { ScanKind::Blank, 2, { -0.001, 0.001, 2, 0.002 }, { 1e6, 10, 140, 1, 1, 2, 1 },
        1e-6, -0.1, 10., -0.1, 10., { -0.001, 0.001 } };
    scan.positions = {
        { 0, -0.001, { { 0, MoveResult::WithinTolerance, -0.001, -0.001 } },
            { AcceptedShot { 0, 1., 1. }, AcceptedShot { 1, 1., 1. } }, PositionResult::Complete },
        { 1, 0.001, { { 0, MoveResult::WithinTolerance, 0.001, 0.001 } },
            { AcceptedShot { 0, 1., 1. }, AcceptedShot { 1, 1., 1. } }, PositionResult::Complete },
    };
    scan.end = { ScanResult::Complete, TerminationReason::NormalCompletion, 2, 2, 4, 0 };
    return scan;
}

struct TemporarySession {
    std::filesystem::path path;
    TemporarySession()
    {
        path = std::filesystem::temp_directory_path()
            / ("zscan_storage_test_"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path / "scans");
    }
    ~TemporarySession() { std::filesystem::remove_all(path); }
};
}

TEST_CASE("interrupted scan identifies CRC-valid prefix without altering original bytes")
{
    const auto full = *encode_scan(scan_fixture());
    REQUIRE(inspect_interrupted_scan(full).error().code == ErrorCode::InvalidBinary);
    for (std::size_t trim = 1; trim <= 20; ++trim) {
        const std::vector<std::uint8_t> original(full.begin(), full.end() - trim);
        const auto copy = original;
        const auto result = inspect_interrupted_scan(original);
        REQUIRE(result);
        REQUIRE(original == copy);
        REQUIRE(result->original.byte_size == original.size());
        REQUIRE(result->original.sha256 == *sha256(original));
        REQUIRE(result->header.record_id == scan_fixture().header.record_id);
        REQUIRE(result->tail_error);
        REQUIRE(result->tail_error->code == ErrorCode::TruncatedBinary);
        REQUIRE(result->valid_prefix_byte_size < original.size());
        REQUIRE(result->valid_frame_count > 1);
    }
    const auto header_only = inspect_interrupted_scan(std::span(full).first(header_size));
    REQUIRE(header_only);
    REQUIRE(header_only->valid_prefix_byte_size == header_size);
    REQUIRE(header_only->valid_frame_count == 0);
    REQUIRE_FALSE(header_only->tail_error);
    REQUIRE(inspect_interrupted_scan(std::span(full).first(header_size - 1)).error().code
        == ErrorCode::TruncatedBinary);
    // Every byte boundary after a valid header either ends at a complete frame
    // or reports a malformed tail, without ever claiming bytes past the cut.
    for (std::size_t size = header_size; size < full.size(); ++size) {
        const std::vector<std::uint8_t> original(full.begin(), full.begin() + size);
        const auto result = inspect_interrupted_scan(original);
        REQUIRE(result);
        REQUIRE(result->valid_prefix_byte_size <= size);
        REQUIRE(result->original.byte_size == size);
        if (result->valid_prefix_byte_size != size)
            REQUIRE(result->tail_error);
    }
}

TEST_CASE("CRC failure preserves the complete damaged file but excludes the failing frame")
{
    auto bytes = *encode_scan(scan_fixture());
    const auto before = bytes;
    bytes[header_size + 25] ^= 1;
    const auto result = inspect_interrupted_scan(bytes);
    REQUIRE(result);
    REQUIRE(result->valid_prefix_byte_size == header_size);
    REQUIRE(result->valid_frame_count == 0);
    REQUIRE(result->tail_error->code == ErrorCode::ChecksumMismatch);
    REQUIRE(result->original.sha256 != *sha256(before));
    REQUIRE(bytes.size() == before.size());
}

TEST_CASE("exclusive scan creation flushes positions and verifies seal on disk")
{
    TemporarySession session;
    const auto path = *SessionPath::parse("scans/scan.bin");
    const auto original = scan_fixture();
    const auto seal = write_sealed_scan(session.path, path, original);
    REQUIRE(seal);
    REQUIRE(seal->byte_size == encode_scan(original)->size());
    REQUIRE(*read_sealed_file(session.path, path, *seal) == *encode_scan(original));
    REQUIRE(
        write_sealed_scan(session.path, path, original).error().code == ErrorCode::FilesystemError);
    REQUIRE(
        read_sealed_file(session.path, path, *seal, 10).error().code == ErrorCode::ResourceLimit);
    std::fstream file(
        session.path / "scans/scan.bin", std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(header_size + 25);
    file.put('\1');
    file.flush();
    file.close();
    REQUIRE(
        read_sealed_file(session.path, path, *seal).error().code == ErrorCode::ChecksumMismatch);
    const auto outside
        = session.path.parent_path() / (session.path.filename().string() + "_outside");
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside, session.path / "escape");
    REQUIRE(write_sealed_scan(session.path, *SessionPath::parse("escape/scan.bin"), original)
                .error()
                .code
        == ErrorCode::UnsafePath);
    std::filesystem::remove_all(outside);
}

TEST_CASE("energy submission publishes a validated immutable burst without overwriting")
{
    TemporarySession session;
    std::filesystem::create_directory(session.path / "energy");
    const auto path = *SessionPath::parse("energy/burst.bin");
    EnergyFile energy { { FileKind::Energy, *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
                            *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
                            *UtcTimestamp::parse("2026-10-25T14:03:00Z") },
        { 50e-9, 52e-9 } };
    const auto seal = write_energy_file_atomic(session.path, path, energy);
    REQUIRE(seal);
    REQUIRE(decode_energy(*read_sealed_file(session.path, path, *seal))->readings_j
        == energy.readings_j);
    REQUIRE(write_energy_file_atomic(session.path, path, energy).error().code
        == ErrorCode::FilesystemError);
    REQUIRE(*read_sealed_file(session.path, path, *seal) == *encode_energy(energy));
}

TEST_CASE("derived arrays are sealed and replayed without overwrite")
{
    TemporarySession session;
    std::filesystem::create_directory(session.path / "derived");
    DerivedArrayFile arrays { { FileKind::DerivedArrays,
                                  *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
                                  *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
                                  *UtcTimestamp::parse("2026-10-25T15:00:00Z") },
        { { ArrayRole::Transmission, 0, 1, 2, { 0.8, 0.9 } } } };
    const auto path = *SessionPath::parse("derived/prepared.bin");
    const auto seal = write_derived_arrays_atomic(session.path, path, arrays);
    REQUIRE(seal);
    REQUIRE(decode_arrays(*read_sealed_file(session.path, path, *seal))->arrays[0].values_row_major
        == arrays.arrays[0].values_row_major);
    REQUIRE_FALSE(write_derived_arrays_atomic(session.path, path, arrays));
}

TEST_CASE("session manifest updates atomically at strictly increasing revisions")
{
    TemporarySession session;
    const auto file = [](std::string_view name) {
        return FileIdentity { *SessionPath::parse(name), 42,
            *Sha256Digest::parse(std::string(64, 'a')) };
    };
    SessionManifest manifest { *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *UtcTimestamp::parse("2026-10-25T14:03:00Z"), 1, file("sample.toml"),
        file("instrument.toml") };
    REQUIRE(write_session_manifest_atomic(session.path, manifest));
    REQUIRE(read_session_manifest(session.path)->manifest_revision == 1);
    REQUIRE_FALSE(write_session_manifest_atomic(session.path, manifest));
    manifest.manifest_revision = 3;
    REQUIRE_FALSE(write_session_manifest_atomic(session.path, manifest));
    manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(session.path, manifest));
    REQUIRE(read_session_manifest(session.path)->manifest_revision == 2);
    REQUIRE(read_session_manifest(session.path)->session_id == manifest.session_id);
}

TEST_CASE("recovery freezes an orphan scan and registers its immutable valid prefix")
{
    TemporarySession session;
    const auto snapshot = [](std::string_view path) {
        return FileIdentity { *SessionPath::parse(path), 1,
            *Sha256Digest::parse(std::string(64, 'a')) };
    };
    SessionManifest manifest { scan_fixture().header.session_id, scan_fixture().header.timestamp, 1,
        snapshot("sample.toml"), snapshot("instrument.toml") };
    REQUIRE(write_session_manifest_atomic(session.path, manifest));
    const auto full = *encode_scan(scan_fixture());
    const std::vector<std::uint8_t> interrupted(full.begin(), full.end() - 7);
    const auto relative = *SessionPath::parse("scans/orphan.bin");
    std::ofstream output(session.path / relative.value(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(interrupted.data()), interrupted.size());
    output.close();
    ScanRecord metadata { scan_fixture().header.record_id, ScanKind::Blank,
        AcquisitionState::InProgress, { relative, 0, { } }, scan_fixture().header.timestamp, "",
        "solvent-1", "blank-cuvette", "scan-config-1", scan_fixture().config.grid,
        scan_fixture().config.capture };
    const auto recovered = recover_interrupted_scan(session.path, metadata);
    REQUIRE(recovered);
    REQUIRE(recovered->state == AcquisitionState::Interrupted);
    REQUIRE(recovered->file.byte_size == interrupted.size());
    REQUIRE(recovered->file.sha256 == *sha256(interrupted));
    REQUIRE(recovered->valid_prefix_byte_size < interrupted.size());
    REQUIRE(read_session_manifest(session.path)->manifest_revision == 2);
    REQUIRE(read_session_manifest(session.path)->scans[0].state == AcquisitionState::Interrupted);
    REQUIRE(read_session_manifest(session.path)->scans[0].valid_prefix_byte_size
        == recovered->valid_prefix_byte_size);
    REQUIRE_FALSE(recover_interrupted_scan(session.path, metadata));
}
