#pragma once

#include "zscan/schema/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace zscan::schema::binary {

inline constexpr std::uint16_t format_major = 1;
inline constexpr std::uint16_t format_minor = 0;
inline constexpr std::size_t header_size = 80;
inline constexpr std::size_t frame_prefix_size = 16;
inline constexpr std::size_t frame_checksum_size = 4;
inline constexpr std::uint16_t optional_frame_flag = 0x0001;

enum class FileKind : std::uint16_t { Scan = 1, Dark = 2, Energy = 3, DerivedArrays = 4 };

// Stable globally unique frame types. Optional extensions use 0x8000..0xfffe.
enum class FrameType : std::uint16_t {
    ScanConfig = 0x1001,
    PositionBegin = 0x1002,
    MoveCheck = 0x1003,
    AcceptedShot = 0x1004,
    RejectedAttempt = 0x1005,
    PositionEnd = 0x1006,
    ScanEnd = 0x10ff,
    DarkConfig = 0x2001,
    DarkAcceptedShot = 0x2002,
    DarkRejectedAttempt = 0x2003,
    DarkEnd = 0x20ff,
    EnergyConfig = 0x3001,
    EnergyReadings = 0x3002,
    EnergyEnd = 0x30ff,
    ArrayBlock = 0x4001,
    ArrayEnd = 0x40ff,
};

struct ResourceLimits {
    std::uint32_t max_frame_payload_bytes { 16 * 1024 * 1024 };
    std::uint64_t max_frames { 1'000'000 };
};

struct Header {
    FileKind kind { FileKind::Scan };
    Uuid record_id;
    Uuid session_id;
    UtcTimestamp timestamp;
};

struct Frame {
    std::uint16_t type { };
    std::uint16_t flags { };
    std::uint64_t sequence { };
    std::vector<std::uint8_t> payload;
};

class Writer {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    Result<void> f64(double value);
    void bytes(std::span<const std::uint8_t> data);

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data)
        : data_(data)
    {
    }
    [[nodiscard]] Result<std::uint8_t> u8();
    [[nodiscard]] Result<std::uint16_t> u16();
    [[nodiscard]] Result<std::uint32_t> u32();
    [[nodiscard]] Result<std::uint64_t> u64();
    [[nodiscard]] Result<std::int64_t> i64();
    [[nodiscard]] Result<double> f64();
    [[nodiscard]] Result<std::span<const std::uint8_t>> bytes(std::size_t count);
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_ { };
};

// CRC-32C / Castagnoli over the supplied exact wire bytes.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] Result<Sha256Digest> sha256(std::span<const std::uint8_t> data);
[[nodiscard]] Result<Uuid> random_uuid_v4();

[[nodiscard]] Result<std::array<std::uint8_t, header_size>> encode_header(const Header& header);
[[nodiscard]] Result<Header> decode_header(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(
    const Frame& frame, ResourceLimits limits = { });

// FrameReader takes bytes AFTER the 80-byte header. Unknown optional frames are
// verified and skipped; required unknown frames and malformed tails are errors.
class FrameReader {
public:
    explicit FrameReader(std::span<const std::uint8_t> frames, ResourceLimits limits = { })
        : frames_(frames)
        , limits_(limits)
    {
    }
    [[nodiscard]] Result<std::optional<Frame>> next();
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::uint64_t frames_seen() const noexcept { return next_sequence_; }

private:
    std::span<const std::uint8_t> frames_;
    ResourceLimits limits_;
    std::size_t offset_ { };
    std::uint64_t next_sequence_ { };
};

} // namespace zscan::schema::binary
