#include "zscan/schema/derived_arrays.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>

namespace zscan::schema::binary {
namespace {
    template <typename T> Result<T> bad(ErrorCode code, std::string message, std::string location)
    {
        return std::unexpected(
            SchemaError { code, std::move(message), { }, { }, std::move(location) });
    }
    bool role_valid(ArrayRole role)
    {
        const auto value = static_cast<std::uint16_t>(role);
        return value >= 1 && value <= 13;
    }
}

Result<void> validate_arrays(const DerivedArrayFile& file)
{
    if (file.header.kind != FileKind::DerivedArrays || file.arrays.empty())
        return bad<void>(ErrorCode::InvalidValue, "expected nonempty derived-array file", "arrays");
    if (auto header = encode_header(file.header); !header)
        return std::unexpected(header.error());
    std::set<std::pair<std::uint16_t, std::uint32_t>> keys;
    for (std::size_t i = 0; i < file.arrays.size(); ++i) {
        const auto& array = file.arrays[i];
        if (!role_valid(array.role) || array.rows == 0 || array.columns == 0
            || array.rows > std::numeric_limits<std::uint64_t>::max() / array.columns
            || array.rows * array.columns != array.values_row_major.size()
            || !keys.insert({ static_cast<std::uint16_t>(array.role), array.curve_index }).second)
            return bad<void>(ErrorCode::InvalidValue, "invalid shape, role or duplicate array",
                "arrays[" + std::to_string(i) + "]");
        if (!std::all_of(array.values_row_major.begin(), array.values_row_major.end(),
                [](double value) { return std::isfinite(value); }))
            return bad<void>(ErrorCode::InvalidValue, "nonfinite derived value",
                "arrays[" + std::to_string(i) + "]");
    }
    return { };
}

Result<std::vector<std::uint8_t>> encode_arrays(const DerivedArrayFile& file, ResourceLimits limits)
{
    if (auto result = validate_arrays(file); !result)
        return std::unexpected(result.error());
    auto header = encode_header(file.header);
    if (!header)
        return std::unexpected(header.error());
    std::vector<std::uint8_t> bytes(header->begin(), header->end());
    std::uint64_t sequence = 0;
    for (const auto& array : file.arrays) {
        Writer payload;
        payload.u16(static_cast<std::uint16_t>(array.role));
        payload.u16(0);
        payload.u32(array.curve_index);
        payload.u64(array.rows);
        payload.u64(array.columns);
        for (double value : array.values_row_major)
            (void)payload.f64(value);
        auto frame = encode_frame({ static_cast<std::uint16_t>(FrameType::ArrayBlock), 0,
                                      sequence++, std::move(payload).take() },
            limits);
        if (!frame)
            return std::unexpected(frame.error());
        bytes.insert(bytes.end(), frame->begin(), frame->end());
    }
    Writer end;
    end.u32(static_cast<std::uint32_t>(file.arrays.size()));
    end.u32(0);
    auto frame = encode_frame(
        { static_cast<std::uint16_t>(FrameType::ArrayEnd), 0, sequence, std::move(end).take() },
        limits);
    if (!frame)
        return std::unexpected(frame.error());
    bytes.insert(bytes.end(), frame->begin(), frame->end());
    return bytes;
}

Result<DerivedArrayFile> decode_arrays(std::span<const std::uint8_t> bytes, ResourceLimits limits)
{
    auto header = decode_header(bytes);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != FileKind::DerivedArrays)
        return bad<DerivedArrayFile>(
            ErrorCode::InvalidBinary, "expected derived arrays", "header.kind");
    DerivedArrayFile result { *header };
    FrameReader frames(bytes.subspan(header_size), limits);
    bool ended = false;
    while (true) {
        auto next = frames.next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        const auto& frame = **next;
        if (ended)
            return bad<DerivedArrayFile>(
                ErrorCode::InvalidBinary, "frame after array end", "array_end");
        Reader payload(frame.payload);
        if (frame.type == static_cast<std::uint16_t>(FrameType::ArrayBlock)) {
            if (frame.payload.size() < 24 || (frame.payload.size() - 24) % 8 != 0)
                return bad<DerivedArrayFile>(
                    ErrorCode::InvalidBinary, "invalid array block size", "array_block");
            DenseArray array;
            array.role = static_cast<ArrayRole>(*payload.u16());
            if (*payload.u16() != 0)
                return bad<DerivedArrayFile>(
                    ErrorCode::InvalidBinary, "nonzero array reserved field", "array_block");
            array.curve_index = *payload.u32();
            array.rows = *payload.u64();
            array.columns = *payload.u64();
            if (array.rows == 0 || array.columns == 0
                || array.rows > std::numeric_limits<std::uint64_t>::max() / array.columns
                || array.rows * array.columns != (frame.payload.size() - 24) / 8)
                return bad<DerivedArrayFile>(
                    ErrorCode::InvalidBinary, "array shape disagrees with payload", "array_block");
            array.values_row_major.reserve(static_cast<std::size_t>(array.rows * array.columns));
            while (payload.remaining()) {
                auto value = payload.f64();
                if (!value)
                    return std::unexpected(value.error());
                array.values_row_major.push_back(*value);
            }
            result.arrays.push_back(std::move(array));
        } else if (frame.type == static_cast<std::uint16_t>(FrameType::ArrayEnd)) {
            if (frame.payload.size() != 8 || *payload.u32() != result.arrays.size()
                || *payload.u32() != 0)
                return bad<DerivedArrayFile>(
                    ErrorCode::InvalidBinary, "array footer mismatch", "array_end");
            ended = true;
            if (frames.offset() != bytes.size() - header_size)
                return bad<DerivedArrayFile>(
                    ErrorCode::InvalidBinary, "trailing array frames", "array_end");
        } else
            return bad<DerivedArrayFile>(
                ErrorCode::InvalidBinary, "unexpected frame in arrays", "array_block");
    }
    if (!ended)
        return bad<DerivedArrayFile>(ErrorCode::TruncatedBinary, "missing array end", "array_end");
    if (auto valid = validate_arrays(result); !valid)
        return std::unexpected(valid.error());
    return result;
}

} // namespace zscan::schema::binary
