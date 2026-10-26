#include "zscan/schema/binary.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace zscan::schema::binary {
namespace {

    constexpr std::array<std::uint8_t, 8> magic { 'Z', 'S', 'C', 'N', 'B', 'I', 'N', 0 };
    constexpr std::int64_t max_wire_seconds = 9'223'372'035;
    static_assert(
        sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);

    template <typename T> Result<T> fail(ErrorCode code, std::string message, std::size_t offset)
    {
        return std::unexpected(
            SchemaError { code, std::move(message), { }, { }, "byte " + std::to_string(offset) });
    }

    bool known_frame(std::uint16_t type)
    {
        switch (static_cast<FrameType>(type)) {
        case FrameType::ScanConfig:
        case FrameType::PositionBegin:
        case FrameType::MoveCheck:
        case FrameType::AcceptedShot:
        case FrameType::RejectedAttempt:
        case FrameType::PositionEnd:
        case FrameType::ScanEnd:
        case FrameType::DarkConfig:
        case FrameType::DarkAcceptedShot:
        case FrameType::DarkRejectedAttempt:
        case FrameType::DarkEnd:
        case FrameType::EnergyConfig:
        case FrameType::EnergyReadings:
        case FrameType::EnergyEnd:
        case FrameType::ArrayBlock:
        case FrameType::ArrayEnd:
            return true;
        }
        return false;
    }

    Result<std::int64_t> to_wire_time(const UtcTimestamp& timestamp)
    {
        const auto seconds = timestamp.seconds.time_since_epoch().count();
        if (timestamp.nanosecond >= 1'000'000'000 || seconds < -max_wire_seconds
            || seconds > max_wire_seconds) {
            return fail<std::int64_t>(ErrorCode::InvalidTimestamp,
                "UTC timestamp is outside the signed-nanosecond wire range", 48);
        }
        return seconds * 1'000'000'000 + timestamp.nanosecond;
    }

    bool valid_uuid(const Uuid& uuid) { return Uuid::parse(uuid.to_string()).has_value(); }

} // namespace

void Writer::u8(std::uint8_t value) { data_.push_back(value); }

void Writer::u16(std::uint16_t value)
{
    for (int i = 0; i < 2; ++i)
        u8(static_cast<std::uint8_t>(value >> (8 * i)));
}

void Writer::u32(std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        u8(static_cast<std::uint8_t>(value >> (8 * i)));
}

void Writer::u64(std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        u8(static_cast<std::uint8_t>(value >> (8 * i)));
}

void Writer::i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }

Result<void> Writer::f64(double value)
{
    if (!std::isfinite(value))
        return fail<void>(ErrorCode::InvalidValue, "nonfinite binary64", data_.size());
    u64(std::bit_cast<std::uint64_t>(value));
    return { };
}

void Writer::bytes(std::span<const std::uint8_t> data)
{
    data_.insert(data_.end(), data.begin(), data.end());
}

Result<std::span<const std::uint8_t>> Reader::bytes(std::size_t count)
{
    if (count > remaining()) {
        return fail<std::span<const std::uint8_t>>(
            ErrorCode::TruncatedBinary, "truncated fixed-width field", offset_);
    }
    const auto result = data_.subspan(offset_, count);
    offset_ += count;
    return result;
}

Result<std::uint8_t> Reader::u8()
{
    auto value = bytes(1);
    if (!value)
        return std::unexpected(value.error());
    return (*value)[0];
}

Result<std::uint16_t> Reader::u16()
{
    auto value = bytes(2);
    if (!value)
        return std::unexpected(value.error());
    return std::uint16_t((*value)[0]) | (std::uint16_t((*value)[1]) << 8);
}

Result<std::uint32_t> Reader::u32()
{
    auto value = bytes(4);
    if (!value)
        return std::unexpected(value.error());
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i)
        result |= std::uint32_t((*value)[i]) << (8 * i);
    return result;
}

Result<std::uint64_t> Reader::u64()
{
    auto value = bytes(8);
    if (!value)
        return std::unexpected(value.error());
    std::uint64_t result = 0;
    for (int i = 0; i < 8; ++i)
        result |= std::uint64_t((*value)[i]) << (8 * i);
    return result;
}

Result<std::int64_t> Reader::i64()
{
    auto result = u64();
    if (!result)
        return std::unexpected(result.error());
    return std::bit_cast<std::int64_t>(*result);
}

Result<double> Reader::f64()
{
    auto bits = u64();
    if (!bits)
        return std::unexpected(bits.error());
    const double value = std::bit_cast<double>(*bits);
    if (!std::isfinite(value))
        return fail<double>(ErrorCode::InvalidValue, "nonfinite binary64", offset_ - 8);
    return value;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

Result<Sha256Digest> sha256(std::span<const std::uint8_t> data)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    Sha256Digest digest;
    unsigned int count = 0;
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1
        || EVP_DigestFinal_ex(ctx.get(), digest.bytes.data(), &count) != 1
        || count != digest.bytes.size()) {
        return fail<Sha256Digest>(ErrorCode::CryptoFailure, "OpenSSL SHA-256 failed", 0);
    }
    return digest;
}

Result<Uuid> random_uuid_v4()
{
    Uuid id;
    if (RAND_bytes(id.bytes.data(), static_cast<int>(id.bytes.size())) != 1)
        return fail<Uuid>(ErrorCode::CryptoFailure, "OpenSSL secure random generation failed", 0);
    id.bytes[6] = (id.bytes[6] & 0x0f) | 0x40;
    id.bytes[8] = (id.bytes[8] & 0x3f) | 0x80;
    return id;
}

Result<std::array<std::uint8_t, header_size>> encode_header(const Header& header)
{
    const auto kind = static_cast<std::uint16_t>(header.kind);
    if (kind < 1 || kind > 4 || !valid_uuid(header.record_id) || !valid_uuid(header.session_id))
        return fail<std::array<std::uint8_t, header_size>>(
            ErrorCode::InvalidBinary, "invalid binary header kind or UUID", 12);
    auto time = to_wire_time(header.timestamp);
    if (!time)
        return std::unexpected(time.error());
    Writer writer;
    writer.bytes(magic);
    writer.u16(format_major);
    writer.u16(format_minor);
    writer.u16(kind);
    writer.u16(0);
    writer.bytes(header.record_id.bytes);
    writer.bytes(header.session_id.bytes);
    writer.i64(*time);
    const std::array<std::uint8_t, 20> reserved { };
    writer.bytes(reserved);
    writer.u32(crc32c(writer.data()));
    std::array<std::uint8_t, header_size> result { };
    std::copy(writer.data().begin(), writer.data().end(), result.begin());
    return result;
}

Result<Header> decode_header(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < header_size)
        return fail<Header>(ErrorCode::TruncatedBinary, "truncated binary header", bytes.size());
    bytes = bytes.first(header_size);
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()))
        return fail<Header>(ErrorCode::InvalidBinary, "wrong binary magic", 0);
    Reader reader(bytes);
    (void)reader.bytes(8);
    const auto major = *reader.u16();
    const auto minor = *reader.u16();
    if (major != format_major || minor != format_minor)
        return fail<Header>(ErrorCode::UnsupportedVersion, "unsupported binary format version", 8);
    const auto kind = *reader.u16();
    if (kind < 1 || kind > 4 || *reader.u16() != 0)
        return fail<Header>(ErrorCode::InvalidBinary, "invalid file kind or header flags", 12);
    Header header;
    header.kind = static_cast<FileKind>(kind);
    const auto record = *reader.bytes(16);
    const auto session = *reader.bytes(16);
    std::copy(record.begin(), record.end(), header.record_id.bytes.begin());
    std::copy(session.begin(), session.end(), header.session_id.bytes.begin());
    if (!valid_uuid(header.record_id) || !valid_uuid(header.session_id))
        return fail<Header>(ErrorCode::InvalidUuid, "invalid binary header UUID", 16);
    const auto time_ns = *reader.i64();
    const auto reserved = *reader.bytes(20);
    if (std::any_of(reserved.begin(), reserved.end(), [](std::uint8_t x) { return x != 0; }))
        return fail<Header>(ErrorCode::InvalidBinary, "nonzero reserved header bytes", 56);
    const auto stored_crc = *reader.u32();
    if (stored_crc != crc32c(bytes.first(header_size - 4)))
        return fail<Header>(ErrorCode::ChecksumMismatch, "binary header CRC32C mismatch", 76);
    const auto since_epoch = std::chrono::nanoseconds { time_ns };
    const auto seconds = std::chrono::floor<std::chrono::seconds>(since_epoch);
    header.timestamp = { std::chrono::sys_seconds { seconds },
        static_cast<std::uint32_t>((since_epoch - seconds).count()) };
    const auto parsed = UtcTimestamp::parse(header.timestamp.to_string());
    if (!parsed)
        return fail<Header>(ErrorCode::InvalidTimestamp, "invalid binary UTC timestamp", 48);
    return header;
}

Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, ResourceLimits limits)
{
    if (frame.payload.size() > limits.max_frame_payload_bytes || frame.payload.size() > UINT32_MAX)
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::ResourceLimit, "frame payload exceeds limit", 4);
    if (frame.flags & ~optional_frame_flag)
        return fail<std::vector<std::uint8_t>>(ErrorCode::InvalidBinary, "unknown frame flags", 2);
    const bool known = known_frame(frame.type);
    if ((known && frame.flags != 0)
        || (!known
            && (frame.type < 0x8000 || frame.type == 0xffff || frame.flags != optional_frame_flag)))
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::InvalidBinary, "invalid required or optional frame type", 0);
    Writer writer;
    writer.u16(frame.type);
    writer.u16(frame.flags);
    writer.u32(static_cast<std::uint32_t>(frame.payload.size()));
    writer.u64(frame.sequence);
    writer.bytes(frame.payload);
    writer.u32(crc32c(writer.data()));
    return std::move(writer).take();
}

Result<std::optional<Frame>> FrameReader::next()
{
    while (offset_ < frames_.size()) {
        if (next_sequence_ >= limits_.max_frames)
            return fail<std::optional<Frame>>(
                ErrorCode::ResourceLimit, "frame count exceeds limit", offset_);
        const auto start = offset_;
        if (frames_.size() - start < frame_prefix_size)
            return fail<std::optional<Frame>>(
                ErrorCode::TruncatedBinary, "truncated frame prefix", start);
        Reader reader(frames_.subspan(start));
        const auto type = *reader.u16();
        const auto flags = *reader.u16();
        const auto payload_size = *reader.u32();
        const auto sequence = *reader.u64();
        if (flags & ~optional_frame_flag)
            return fail<std::optional<Frame>>(
                ErrorCode::InvalidBinary, "unknown frame flags", start + 2);
        const bool known = known_frame(type);
        if ((known && flags != 0)
            || (!known && (type < 0x8000 || type == 0xffff || flags != optional_frame_flag)))
            return fail<std::optional<Frame>>(
                ErrorCode::InvalidBinary, "unsupported required frame type", start);
        if (sequence != next_sequence_)
            return fail<std::optional<Frame>>(
                ErrorCode::InvalidBinary, "nonconsecutive frame sequence", start + 8);
        if (payload_size > limits_.max_frame_payload_bytes)
            return fail<std::optional<Frame>>(
                ErrorCode::ResourceLimit, "frame payload exceeds limit", start + 4);
        // Subtraction avoids overflow for untrusted lengths.
        if (frames_.size() - start - frame_prefix_size < frame_checksum_size
            || payload_size > frames_.size() - start - frame_prefix_size - frame_checksum_size)
            return fail<std::optional<Frame>>(
                ErrorCode::TruncatedBinary, "truncated frame payload or CRC", start);
        const auto payload = frames_.subspan(start + frame_prefix_size, payload_size);
        const auto end = start + frame_prefix_size + payload_size + frame_checksum_size;
        Reader checksum(frames_.subspan(end - 4, 4));
        if (*checksum.u32() != crc32c(frames_.subspan(start, end - start - 4)))
            return fail<std::optional<Frame>>(
                ErrorCode::ChecksumMismatch, "frame CRC32C mismatch", end - 4);
        offset_ = end;
        ++next_sequence_;
        if (!known)
            continue;
        return std::optional { Frame {
            type, flags, sequence, std::vector<std::uint8_t>(payload.begin(), payload.end()) } };
    }
    return std::optional<Frame> { };
}

} // namespace zscan::schema::binary
