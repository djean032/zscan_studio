#include "zscan/schema/binary.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::binary;

// These wire bytes were calculated independently using Python's struct and a
// bit-at-a-time reference CRC32C implementation, not the C++ writer.
std::vector<std::uint8_t> unhex(std::string_view hex)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(
            static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return bytes;
}

const auto golden_header = unhex("5a53434e42494e000100000003000000550e8400e29b41d4a716446655440000"
                                 "33e4792344664ec8b9573e4c39ad451600c88f15afcae1180000000000000000"
                                 "00000000000000000000000075324bcc");
const auto golden_frame = unhex("013000000800000000000000000000002a000000000000008dbbca91");
const auto golden_optional = unhex("01800100000000000100000000000000d6154487");

Header example_header()
{
    return { FileKind::Energy, *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T14:03:00Z") };
}
}

TEST_CASE("little-endian integers and finite IEEE-754 binary64 have exact wire bytes")
{
    Writer writer;
    writer.u8(0x7f);
    writer.u16(0x1234);
    writer.u32(0x89abcdef);
    writer.u64(0x0123456789abcdefULL);
    writer.i64(-1);
    REQUIRE(writer.f64(1.5));
    REQUIRE(
        writer.data() == unhex("7f3412efcdab89efcdab8967452301ffffffffffffffff000000000000f83f"));
    Reader reader(writer.data());
    REQUIRE(*reader.u8() == 0x7f);
    REQUIRE(*reader.u16() == 0x1234);
    REQUIRE(*reader.u32() == 0x89abcdef);
    REQUIRE(*reader.u64() == 0x0123456789abcdefULL);
    REQUIRE(*reader.i64() == -1);
    REQUIRE(*reader.f64() == 1.5);
    REQUIRE(reader.remaining() == 0);
    REQUIRE(reader.u8().error().code == ErrorCode::TruncatedBinary);
    REQUIRE_FALSE(writer.f64(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(writer.f64(std::numeric_limits<double>::infinity()));
    const auto invalid = unhex("000000000000f87f");
    REQUIRE(Reader(invalid).f64().error().code == ErrorCode::InvalidValue);
    REQUIRE(Reader(unhex("1234")).u32().error().code == ErrorCode::TruncatedBinary);
}

TEST_CASE("CRC32C and SHA-256 match external check vectors")
{
    const auto vector = unhex("313233343536373839");
    REQUIRE(crc32c(vector) == 0xe3069283);
    const auto abc = unhex("616263");
    const auto digest = sha256(abc);
    REQUIRE(digest);
    REQUIRE(
        digest->to_string() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(sha256(std::span<const std::uint8_t> { })->to_string()
        == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("secure UUID generation sets version and variant bits")
{
    const auto first = random_uuid_v4();
    const auto second = random_uuid_v4();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->bytes[6] >> 4 == 4);
    REQUIRE(first->bytes[8] >> 6 == 2);
    REQUIRE(Uuid::parse(first->to_string()));
    REQUIRE(*first != *second);
}

TEST_CASE("80-byte file header matches independently calculated golden bytes")
{
    const auto encoded = encode_header(example_header());
    REQUIRE(encoded);
    REQUIRE(std::vector(encoded->begin(), encoded->end()) == golden_header);
    const auto decoded = decode_header(golden_header);
    REQUIRE(decoded);
    REQUIRE(decoded->kind == FileKind::Energy);
    REQUIRE(decoded->record_id == example_header().record_id);
    REQUIRE(decoded->session_id == example_header().session_id);
    REQUIRE(decoded->timestamp == example_header().timestamp);
    auto timestamp = example_header();
    timestamp.timestamp = *UtcTimestamp::parse("2026-10-25T14:03:00.123456789Z");
    REQUIRE(decode_header(*encode_header(timestamp))->timestamp == timestamp.timestamp);
    timestamp.timestamp = *UtcTimestamp::parse("1969-12-31T23:59:59.5Z");
    REQUIRE(decode_header(*encode_header(timestamp))->timestamp == timestamp.timestamp);
    timestamp.timestamp = *UtcTimestamp::parse("9999-12-31T23:59:59Z");
    REQUIRE(encode_header(timestamp).error().code == ErrorCode::InvalidTimestamp);
}

TEST_CASE("header rejects version, reserved fields, UUID, CRC, and truncation")
{
    for (std::size_t size = 0; size < header_size; ++size) {
        REQUIRE(decode_header(std::span(golden_header).first(size)).error().code
            == ErrorCode::TruncatedBinary);
    }
    auto bytes = golden_header;
    bytes[0] ^= 1;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = golden_header;
    bytes[8] = 2;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::UnsupportedVersion);
    bytes = golden_header;
    bytes[14] = 1;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = golden_header;
    bytes[12] = 5;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = golden_header;
    bytes[56] = 1;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = golden_header;
    bytes[22] = 0;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::InvalidUuid);
    bytes = golden_header;
    bytes[60] ^= 1;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::InvalidBinary);
    bytes = golden_header;
    bytes[76] ^= 1;
    REQUIRE(decode_header(bytes).error().code == ErrorCode::ChecksumMismatch);
}

TEST_CASE("frame encoding matches independent golden bytes and skips optional extensions")
{
    Frame frame { static_cast<std::uint16_t>(FrameType::EnergyConfig), 0, 0,
        unhex("2a00000000000000") };
    REQUIRE(*encode_frame(frame) == golden_frame);
    REQUIRE(*encode_frame({ 0x8001, optional_frame_flag, 1, { } }) == golden_optional);
    auto both = golden_frame;
    both.insert(both.end(), golden_optional.begin(), golden_optional.end());
    FrameReader reader(both);
    auto decoded = reader.next();
    REQUIRE(decoded);
    REQUIRE(decoded->has_value());
    REQUIRE(decoded->value().type == static_cast<std::uint16_t>(FrameType::EnergyConfig));
    REQUIRE(decoded->value().payload == frame.payload);
    REQUIRE(reader.next()); // Unknown optional extension was verified and skipped.
    REQUIRE_FALSE(reader.next()->has_value());
    REQUIRE(reader.offset() == both.size());
    both.back() ^= 1;
    FrameReader invalid_optional(both);
    REQUIRE(invalid_optional.next());
    REQUIRE(invalid_optional.next().error().code == ErrorCode::ChecksumMismatch);
}

TEST_CASE("framing refuses truncation, damage, bad sequence, unknown required fields and limits")
{
    for (std::size_t size = 1; size < golden_frame.size(); ++size) {
        FrameReader reader { std::span(golden_frame).first(size) };
        REQUIRE(reader.next().error().code == ErrorCode::TruncatedBinary);
    }
    auto damaged = golden_frame;
    damaged[16] ^= 1;
    REQUIRE(FrameReader(damaged).next().error().code == ErrorCode::ChecksumMismatch);
    REQUIRE(FrameReader(golden_optional).next().error().code == ErrorCode::InvalidBinary);
    auto wrong_sequence
        = *encode_frame({ static_cast<std::uint16_t>(FrameType::EnergyEnd), 0, 1, { } });
    REQUIRE(FrameReader(wrong_sequence).next().error().code == ErrorCode::InvalidBinary);
    REQUIRE(encode_frame({ 0x4444, 0, 0, { } }).error().code == ErrorCode::InvalidBinary);
    REQUIRE(encode_frame(
                { static_cast<std::uint16_t>(FrameType::EnergyEnd), optional_frame_flag, 0, { } })
                .error()
                .code
        == ErrorCode::InvalidBinary);
    REQUIRE(
        encode_frame({ static_cast<std::uint16_t>(FrameType::EnergyEnd), 2, 0, { } }).error().code
        == ErrorCode::InvalidBinary);
    REQUIRE(encode_frame(
                { static_cast<std::uint16_t>(FrameType::EnergyConfig), 0, 0, unhex("00000000") },
                ResourceLimits { 3, 10 })
                .error()
                .code
        == ErrorCode::ResourceLimit);
    REQUIRE(FrameReader(golden_frame, ResourceLimits { 3, 10 }).next().error().code
        == ErrorCode::ResourceLimit);
    REQUIRE(FrameReader(golden_frame, ResourceLimits { 16, 0 }).next().error().code
        == ErrorCode::ResourceLimit);
}
