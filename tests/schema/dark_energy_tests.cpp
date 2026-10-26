#include "zscan/schema/dark_energy.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::binary;

std::vector<std::uint8_t> unhex(std::string_view text)
{
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(
            static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return result;
}

Header header(FileKind kind)
{
    return { kind, *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T14:03:00Z") };
}

DarkFile dark_fixture()
{
    DarkFile dark;
    dark.header = header(FileKind::Dark);
    dark.config = { { 1e6, 10, 140, 1, 1, 2, 1 }, -0.1, 10.0, -0.1, 10.0 };
    dark.attempts = { AcceptedShot { 0, 0.01, 0.02 },
        RejectedAttempt { 1, RejectionReason::TriggerTimeout, -12 },
        AcceptedShot { 2, 0.03, 0.04 } };
    dark.end = { DarkResult::Complete, DarkTerminationReason::NormalCompletion, 2, 1 };
    return dark;
}

EnergyFile energy_fixture() { return { header(FileKind::Energy), { 50e-9, 52e-9 } }; }

std::vector<std::uint8_t> tamper_frame(
    std::vector<std::uint8_t> bytes, FrameType type, const auto& change)
{
    std::size_t offset = header_size;
    while (offset < bytes.size()) {
        Reader reader { std::span(bytes).subspan(offset) };
        const auto found = *reader.u16();
        (void)reader.u16();
        const auto size = *reader.u32();
        const auto frame_size = frame_prefix_size + size + frame_checksum_size;
        if (found == static_cast<std::uint16_t>(type)) {
            change(std::span(bytes).subspan(offset + frame_prefix_size, size));
            Writer crc;
            crc.u32(crc32c(std::span(bytes).subspan(offset, frame_size - 4)));
            std::copy(
                crc.data().begin(), crc.data().end(), bytes.begin() + offset + frame_size - 4);
            return bytes;
        }
        offset += frame_size;
    }
    FAIL("frame not found");
    return bytes;
}
}

TEST_CASE("dark codec preserves paired accepted peaks and reason-only rejected attempts")
{
    const auto bytes = encode_dark(dark_fixture());
    REQUIRE(bytes);
    const auto decoded = decode_dark(*bytes);
    REQUIRE(decoded);
    REQUIRE(*encode_dark(*decoded) == *bytes);
    REQUIRE(decoded->header.timestamp == dark_fixture().header.timestamp);
    REQUIRE(decoded->attempts.size() == 3);
    REQUIRE(std::get<AcceptedShot>(decoded->attempts[0]).ai1_peak_v == 0.02);
    REQUIRE(std::get<RejectedAttempt>(decoded->attempts[1]).vendor_error_code == -12);
    REQUIRE(decoded->end.rejected_count == 1);
}

TEST_CASE("energy codec retains ordered individual joule readings and one entry time")
{
    const auto bytes = encode_energy(energy_fixture());
    REQUIRE(bytes);
    const auto decoded = decode_energy(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->readings_j == energy_fixture().readings_j);
    REQUIRE(decoded->header.timestamp == energy_fixture().header.timestamp);
    REQUIRE(*encode_energy(*decoded) == *bytes);
}

TEST_CASE("dark and energy golden frames match independent struct and CRC32C bytes")
{
    // Generated independently via Python struct.pack + bitwise CRC32C reference.
    const auto dark_config
        = unhex("012000004000000000000000000000000100010002000000010000000a000000"
                "8c000000000000000000000080842e419a9999999999b9bf0000000000002440"
                "9a9999999999b9bf0000000000002440d9f5d4de");
    const auto dark_shot = unhex("022000001800000001000000000000000000000000000000"
                                 "7b14ae47e17a843f7b14ae47e17a943f7595b485");
    const auto dark = *encode_dark(dark_fixture());
    REQUIRE(std::vector(dark.begin() + header_size, dark.begin() + header_size + dark_config.size())
        == dark_config);
    REQUIRE(std::vector(dark.begin() + header_size + dark_config.size(),
                dark.begin() + header_size + dark_config.size() + dark_shot.size())
        == dark_shot);

    const auto energy_config = unhex("013000000800000000000000000000000200000000000000cedaf1de");
    const auto energy_readings = unhex("023000001800000001000000000000000200000000000000"
                                       "48afbc9af2d76a3eb21c3f59d3ea6b3ee76cd8d0");
    const auto energy_end = unhex("ff3000000800000002000000000000000200000000000000df643ec0");
    const auto energy = *encode_energy(energy_fixture());
    const auto expected_size
        = header_size + energy_config.size() + energy_readings.size() + energy_end.size();
    REQUIRE(energy.size() == expected_size);
    REQUIRE(std::vector(energy.begin() + header_size, energy.end()) == [&] {
        auto frames = energy_config;
        frames.insert(frames.end(), energy_readings.begin(), energy_readings.end());
        frames.insert(frames.end(), energy_end.begin(), energy_end.end());
        return frames;
    }());
}

TEST_CASE("gracefully incomplete dark is preserved but not a completed production dark")
{
    auto dark = dark_fixture();
    dark.attempts.resize(2);
    dark.end = { DarkResult::Incomplete, DarkTerminationReason::OperatorCancelled, 1, 1 };
    REQUIRE(decode_dark(*encode_dark(dark)));
    dark.end.result = DarkResult::Complete;
    REQUIRE_FALSE(encode_dark(dark));
    dark.end = { DarkResult::Incomplete, DarkTerminationReason::ShotRetryLimit, 1, 1 };
    REQUIRE(decode_dark(*encode_dark(dark)));
    dark.attempts.pop_back();
    dark.end.rejected_count = 0;
    REQUIRE_FALSE(encode_dark(dark));
}

TEST_CASE("dark rejects invalid channel values, counts, reasons and malformed frames")
{
    auto dark = dark_fixture();
    std::get<AcceptedShot>(dark.attempts[0]).ai0_peak_v = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(encode_dark(dark));
    dark = dark_fixture();
    std::get<RejectedAttempt>(dark.attempts[1]).reason = static_cast<RejectionReason>(200);
    REQUIRE_FALSE(encode_dark(dark));
    dark = dark_fixture();
    dark.end.accepted_count = 3;
    REQUIRE_FALSE(encode_dark(dark));
    dark = dark_fixture();
    dark.attempts[0] = RejectedAttempt { 0, RejectionReason::Other, 0 };
    REQUIRE_FALSE(encode_dark(dark)); // Would exceed rejected limit.
    auto bytes = *encode_dark(dark_fixture());
    REQUIRE_FALSE(decode_dark(std::span(bytes).first(bytes.size() - 1)));
    bytes = tamper_frame(bytes, FrameType::DarkAcceptedShot,
        [](std::span<std::uint8_t> payload) { payload[4] = 1; });
    REQUIRE(decode_dark(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_dark(dark_fixture());
    bytes = tamper_frame(
        bytes, FrameType::DarkEnd, [](std::span<std::uint8_t> payload) { payload[4] = 99; });
    REQUIRE(decode_dark(bytes).error().code == ErrorCode::InvalidValue);
    bytes = *encode_dark(dark_fixture());
    bytes.push_back(0);
    REQUIRE(decode_dark(bytes).error().code == ErrorCode::InvalidBinary);
}

TEST_CASE("energy rejects nonpositive/nonfinite values, count mismatches and excess payload")
{
    auto energy = energy_fixture();
    energy.readings_j = { 50e-9 };
    REQUIRE_FALSE(encode_energy(energy));
    energy = energy_fixture();
    energy.readings_j[0] = 0;
    REQUIRE_FALSE(encode_energy(energy));
    energy.readings_j[0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(encode_energy(energy));
    auto bytes = *encode_energy(energy_fixture());
    bytes = tamper_frame(
        bytes, FrameType::EnergyReadings, [](std::span<std::uint8_t> payload) { payload[0] = 3; });
    REQUIRE(decode_energy(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_energy(energy_fixture());
    bytes = tamper_frame(
        bytes, FrameType::EnergyEnd, [](std::span<std::uint8_t> payload) { payload[0] = 3; });
    REQUIRE(decode_energy(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = *encode_energy(energy_fixture());
    bytes = tamper_frame(bytes, FrameType::EnergyReadings, [](std::span<std::uint8_t> payload) {
        std::fill(payload.begin() + 8, payload.begin() + 16, 0xff);
    });
    REQUIRE(decode_energy(bytes).error().code == ErrorCode::InvalidValue);
    REQUIRE(encode_energy(energy_fixture(), ResourceLimits { 16, 3 }).error().code
        == ErrorCode::ResourceLimit);
    REQUIRE(decode_energy(*encode_energy(energy_fixture()), ResourceLimits { 16, 3 }).error().code
        == ErrorCode::ResourceLimit);
}
