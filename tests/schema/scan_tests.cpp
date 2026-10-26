#include "zscan/schema/scan.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::binary;

std::vector<std::uint8_t> unhex(std::string_view text)
{
    std::vector<std::uint8_t> data;
    for (std::size_t i = 0; i < text.size(); i += 2)
        data.push_back(
            static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return data;
}

ScanFile complete_scan()
{
    ScanFile scan;
    scan.header = { FileKind::Scan, *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T14:03:00Z") };
    scan.config = {
        .kind = ScanKind::Sample,
        .maximum_move_attempts = 2,
        .grid = { -0.001, 0.001, 2, 0.002 },
        .capture = { 1e6, 10, 140, 1, 1, 2, 1 },
        .maximum_readback_error_m = 1e-6,
        .ai0_minimum_v = -0.1,
        .ai0_maximum_v = 10.0,
        .ai1_minimum_v = -0.1,
        .ai1_maximum_v = 10.0,
        .commanded_positions_m = { -0.001, 0.001 },
    };
    scan.positions = {
        { 0, -0.001,
            { { 0, MoveResult::OutsideTolerance, -0.001, -0.0009 },
                { 1, MoveResult::WithinTolerance, -0.001, -0.001 } },
            { RejectedAttempt { 0, RejectionReason::TriggerTimeout, -12 },
                AcceptedShot { 1, 2.5, 3.5 }, AcceptedShot { 2, 2.6, 3.6 } },
            PositionResult::Complete },
        { 1, 0.001, { { 0, MoveResult::WithinTolerance, 0.001, 0.001 } },
            { AcceptedShot { 0, 2.4, 3.4 }, AcceptedShot { 1, 2.3, 3.3 } },
            PositionResult::Complete },
    };
    scan.end = { ScanResult::Complete, TerminationReason::NormalCompletion, 2, 2, 4, 1 };
    return scan;
}

std::vector<std::uint8_t> replace_frame(
    std::vector<std::uint8_t> file, std::uint16_t type, const auto& change)
{
    std::size_t offset = header_size;
    while (offset < file.size()) {
        Reader reader { std::span(file).subspan(offset) };
        const auto found = *reader.u16();
        (void)reader.u16();
        const auto size = *reader.u32();
        const auto length = frame_prefix_size + size + frame_checksum_size;
        if (found == type) {
            change(std::span(file).subspan(offset + frame_prefix_size, size));
            Writer crc;
            crc.u32(crc32c(std::span(file).subspan(offset, length - 4)));
            std::copy(crc.data().begin(), crc.data().end(), file.begin() + offset + length - 4);
            return file;
        }
        offset += length;
    }
    FAIL("missing frame in fixture");
    return file;
}
}

TEST_CASE("complete scan round-trips every check, peak and rejected reason without shot timestamps")
{
    auto scan = complete_scan();
    const auto bytes = encode_scan(scan);
    REQUIRE(bytes);
    const auto decoded = decode_scan(*bytes);
    REQUIRE(decoded);
    REQUIRE(*encode_scan(*decoded) == *bytes);
    REQUIRE(decoded->header.timestamp == scan.header.timestamp);
    REQUIRE(decoded->config.commanded_positions_m == scan.config.commanded_positions_m);
    REQUIRE(decoded->positions[0].move_checks.size() == 2);
    REQUIRE(decoded->positions[0].move_checks[0].readback_z_m == -0.0009);
    REQUIRE(std::get<RejectedAttempt>(decoded->positions[0].attempts[0]).vendor_error_code == -12);
    REQUIRE(std::get<AcceptedShot>(decoded->positions[0].attempts[2]).ai1_peak_v == 3.6);
    REQUIRE(decoded->end.total_accepted_shots == 4);
    REQUIRE(decoded->end.total_rejected_attempts == 1);
}

TEST_CASE("scan config and position-begin frames match an independent struct/CRC32C fixture")
{
    // Independently calculated with Python struct.pack and a bitwise CRC32C
    // reference, not from encode_scan. First frame has 120 payload bytes.
    const auto config = unhex("0110000078000000000000000000000002020100010000000200000002000000"
                              "010000000a0000008c000000000000000000000080842e41fca9f1d24d6250bf"
                              "fca9f1d24d62503ffca9f1d24d62603f8dedb5a0f7c6b03e9a9999999999b9bf"
                              "00000000000024409a9999999999b9bf0000000000002440fca9f1d24d6250bf"
                              "fca9f1d24d62503fed4a90d4");
    const auto begin = unhex("021000001000000001000000000000000000000000000000"
                             "fca9f1d24d6250bf8d48e157");
    const auto encoded = *encode_scan(complete_scan());
    REQUIRE(
        std::vector(encoded.begin() + header_size, encoded.begin() + header_size + config.size())
        == config);
    REQUIRE(std::vector(encoded.begin() + header_size + config.size(),
                encoded.begin() + header_size + config.size() + begin.size())
        == begin);
}

TEST_CASE("gracefully incomplete scan preserves failed stage checks and never takes shots there")
{
    auto scan = complete_scan();
    auto& final = scan.positions.back();
    final.move_checks = { { 0, MoveResult::OutsideTolerance, 0.001, 0.0011 },
        { 1, MoveResult::OutsideTolerance, 0.001, 0.0011 } };
    final.attempts.clear();
    final.result = PositionResult::StagePositionError;
    scan.end = { ScanResult::Incomplete, TerminationReason::StagePositionError, 2, 1, 2, 1 };
    const auto encoded = encode_scan(scan);
    REQUIRE(encoded);
    const auto decoded = decode_scan(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->end.result == ScanResult::Incomplete);
    REQUIRE(decoded->positions.back().attempts.empty());
    REQUIRE(decoded->positions.back().move_checks.size() == 2);
    REQUIRE(*encode_scan(*decoded) == *encoded);

    scan.positions.back().move_checks.pop_back();
    REQUIRE_FALSE(encode_scan(scan));
    scan.positions.back().move_checks.push_back({ 1, MoveResult::OutsideTolerance, 0.001, 0.0011 });
    scan.positions.back().attempts = { AcceptedShot { 0, 1.0, 1.0 } };
    REQUIRE_FALSE(encode_scan(scan));
}

TEST_CASE("retry limit and operator cancellation can seal an incomplete acquisition")
{
    auto scan = complete_scan();
    scan.positions.resize(1);
    scan.positions[0].attempts
        = { AcceptedShot { 0, 2.5, 3.5 }, RejectedAttempt { 1, RejectionReason::DaqError, -1 } };
    scan.positions[0].result = PositionResult::ShotRetryLimit;
    scan.end = { ScanResult::Incomplete, TerminationReason::ShotRetryLimit, 1, 0, 1, 1 };
    REQUIRE(decode_scan(*encode_scan(scan)));
    scan.positions[0].attempts.pop_back();
    REQUIRE_FALSE(encode_scan(scan));
    scan.positions[0].result = PositionResult::OperatorCancelled;
    scan.end = { ScanResult::Incomplete, TerminationReason::OperatorCancelled, 1, 0, 1, 0 };
    REQUIRE(decode_scan(*encode_scan(scan)));
    scan.positions.clear();
    scan.end = { ScanResult::Incomplete, TerminationReason::OperatorCancelled, 0, 0, 0, 0 };
    REQUIRE(decode_scan(*encode_scan(scan)));
}

TEST_CASE("scan rejects wrong grids, readbacks, voltages, attempt indexes and aggregate totals")
{
    auto scan = complete_scan();
    scan.config.commanded_positions_m[1] = 0.0009;
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[1].commanded_z_m = 0.00100001;
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].move_checks[1].readback_z_m = -0.0009;
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].move_checks[1].move_attempt_index = 2;
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].attempts[1] = AcceptedShot { 2, 2.5, 3.5 };
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].attempts[1] = AcceptedShot { 1, 10.1, 3.5 };
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].attempts[1]
        = AcceptedShot { 1, std::numeric_limits<double>::quiet_NaN(), 3.5 };
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.end.total_rejected_attempts = 0;
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].result = PositionResult::StagePositionError;
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions[0].attempts = { AcceptedShot { 0, 2.0, 3.0 }, AcceptedShot { 1, 2.0, 3.0 },
        RejectedAttempt { 2, RejectionReason::DaqError, -1 } };
    REQUIRE_FALSE(encode_scan(scan));
    scan = complete_scan();
    scan.positions.resize(1);
    scan.end = { ScanResult::Incomplete, TerminationReason::StagePositionError, 1, 1, 2, 1 };
    REQUIRE_FALSE(encode_scan(scan)); // A stage failure must have a failed position record.
}

TEST_CASE(
    "scan decoder refuses wrong frame kind, reserved fields, invalid counts and truncated end")
{
    auto bytes = *encode_scan(complete_scan());
    for (std::size_t i = 1; i <= 5; ++i)
        REQUIRE_FALSE(decode_scan(std::span(bytes).first(bytes.size() - i)));
    bytes = replace_frame(bytes, static_cast<std::uint16_t>(FrameType::ScanConfig),
        [](std::span<std::uint8_t> payload) { payload[6] = 1; });
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_scan(complete_scan());
    bytes = replace_frame(bytes, static_cast<std::uint16_t>(FrameType::PositionEnd),
        [](std::span<std::uint8_t> payload) { payload[4] = 99; });
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_scan(complete_scan());
    bytes = replace_frame(bytes, static_cast<std::uint16_t>(FrameType::AcceptedShot),
        [](std::span<std::uint8_t> payload) { payload[4] = 99; });
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_scan(complete_scan());
    bytes = replace_frame(bytes, static_cast<std::uint16_t>(FrameType::AcceptedShot),
        [](std::span<std::uint8_t> payload) {
            std::fill(payload.begin() + 8, payload.begin() + 16, 0xff); // NaN
        });
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::InvalidValue);
    bytes = *encode_scan(complete_scan());
    bytes = replace_frame(bytes, static_cast<std::uint16_t>(FrameType::RejectedAttempt),
        [](std::span<std::uint8_t> payload) { payload[8] = 200; });
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_scan(complete_scan());
    bytes.push_back(0);
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_scan(complete_scan());
    bytes[header_size + 16] ^= 1;
    REQUIRE(decode_scan(bytes).error().code == ErrorCode::ChecksumMismatch);
    auto scan = complete_scan();
    scan.header.kind = FileKind::Energy;
    REQUIRE(encode_scan(scan).error().code == ErrorCode::InvalidBinary);
}

TEST_CASE("scan config payload limit and frame limit prevent oversized allocation")
{
    const auto scan = complete_scan();
    REQUIRE(
        encode_scan(scan, ResourceLimits { 110, 100 }).error().code == ErrorCode::ResourceLimit);
    REQUIRE(encode_scan(scan, ResourceLimits { 1024, 3 }).error().code == ErrorCode::ResourceLimit);
    const auto bytes = *encode_scan(scan);
    REQUIRE(
        decode_scan(bytes, ResourceLimits { 110, 100 }).error().code == ErrorCode::ResourceLimit);
}
