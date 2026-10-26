#include "zscan/schema/dark_energy.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace zscan::schema::binary {
namespace {

    template <typename T>
    Result<T> invalid(ErrorCode code, std::string message, std::string location)
    {
        return std::unexpected(
            SchemaError { code, std::move(message), { }, { }, std::move(location) });
    }

    Result<void> append(std::vector<std::uint8_t>& bytes, FrameType type, std::uint64_t& sequence,
        Writer&& payload, ResourceLimits limits)
    {
        if (sequence >= limits.max_frames)
            return invalid<void>(
                ErrorCode::ResourceLimit, "frame count exceeds limit", "frame_count");
        auto frame = encode_frame(
            { static_cast<std::uint16_t>(type), 0, sequence, std::move(payload).take() }, limits);
        if (!frame)
            return std::unexpected(frame.error());
        bytes.insert(bytes.end(), frame->begin(), frame->end());
        ++sequence;
        return { };
    }

    bool valid_reason(RejectionReason reason)
    {
        const auto code = static_cast<std::uint16_t>(reason);
        return (code >= 1 && code <= 9) || code == 255;
    }

    bool valid_end_reason(DarkTerminationReason reason)
    {
        const auto code = static_cast<std::uint16_t>(reason);
        return code <= 4 || code == 255;
    }

    double raw_f64(Reader& reader) { return std::bit_cast<double>(*reader.u64()); }

    Result<void> sized(const Frame& frame, std::size_t expected)
    {
        if (frame.payload.size() != expected)
            return invalid<void>(ErrorCode::InvalidBinary, "wrong frame payload size",
                "frame " + std::to_string(frame.sequence));
        return { };
    }

} // namespace

Result<void> validate_dark(const DarkFile& dark)
{
    if (dark.header.kind != FileKind::Dark)
        return invalid<void>(ErrorCode::InvalidBinary, "expected dark file kind", "header.kind");
    if (auto result = encode_header(dark.header); !result)
        return std::unexpected(result.error());
    if (auto result = validate(dark.config.capture); !result)
        return std::unexpected(result.error());
    if (dark.config.capture.target_accepted_readings < 2)
        return invalid<void>(ErrorCode::InvalidValue,
            "dark target must allow at least two readings", "dark_config.target_accepted_readings");
    for (auto [minimum, maximum] :
        { std::pair { dark.config.ai0_minimum_v, dark.config.ai0_maximum_v },
            std::pair { dark.config.ai1_minimum_v, dark.config.ai1_maximum_v } }) {
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum)
            return invalid<void>(ErrorCode::InvalidValue, "invalid dark DAQ channel range",
                "dark_config.voltage_range_v");
    }
    if (dark.attempts.size() > std::uint64_t(dark.config.capture.target_accepted_readings)
                + dark.config.capture.maximum_rejected_attempts
        || dark.attempts.size() > std::numeric_limits<std::uint32_t>::max())
        return invalid<void>(ErrorCode::InvalidValue, "too many dark attempts", "attempts");
    std::uint32_t accepted = 0;
    std::uint32_t rejected = 0;
    for (std::size_t i = 0; i < dark.attempts.size(); ++i) {
        if (accepted == dark.config.capture.target_accepted_readings)
            return invalid<void>(ErrorCode::InvalidValue, "attempt after dark target",
                "attempts[" + std::to_string(i) + "]");
        if (const auto* shot = std::get_if<AcceptedShot>(&dark.attempts[i])) {
            if (shot->attempt_index != i || !std::isfinite(shot->ai0_peak_v)
                || !std::isfinite(shot->ai1_peak_v) || shot->ai0_peak_v < dark.config.ai0_minimum_v
                || shot->ai0_peak_v > dark.config.ai0_maximum_v
                || shot->ai1_peak_v < dark.config.ai1_minimum_v
                || shot->ai1_peak_v > dark.config.ai1_maximum_v)
                return invalid<void>(ErrorCode::InvalidValue, "invalid accepted dark reading",
                    "attempts[" + std::to_string(i) + "]");
            ++accepted;
        } else {
            const auto& rejection = std::get<RejectedAttempt>(dark.attempts[i]);
            if (rejection.attempt_index != i || !valid_reason(rejection.reason))
                return invalid<void>(ErrorCode::InvalidValue, "invalid rejected dark attempt",
                    "attempts[" + std::to_string(i) + "]");
            ++rejected;
        }
    }
    if (rejected > dark.config.capture.maximum_rejected_attempts
        || dark.end.accepted_count != accepted || dark.end.rejected_count != rejected)
        return invalid<void>(ErrorCode::InvalidValue, "dark footer counts disagree", "dark_end");
    if (!valid_end_reason(dark.end.reason)
        || (dark.end.result == DarkResult::Complete
            && (accepted != dark.config.capture.target_accepted_readings
                || dark.end.reason != DarkTerminationReason::NormalCompletion))
        || (dark.end.result == DarkResult::Incomplete
            && (accepted == dark.config.capture.target_accepted_readings
                || dark.end.reason == DarkTerminationReason::NormalCompletion))
        || (dark.end.result != DarkResult::Complete && dark.end.result != DarkResult::Incomplete))
        return invalid<void>(
            ErrorCode::InvalidValue, "invalid dark lifecycle state", "dark_end.result");
    if (dark.end.reason == DarkTerminationReason::ShotRetryLimit
        && (rejected == 0 || rejected != dark.config.capture.maximum_rejected_attempts))
        return invalid<void>(
            ErrorCode::InvalidValue, "dark shot limit not exhausted", "dark_end.reason");
    return { };
}

Result<void> validate_energy(const EnergyFile& energy)
{
    if (energy.header.kind != FileKind::Energy)
        return invalid<void>(ErrorCode::InvalidBinary, "expected energy file kind", "header.kind");
    if (auto result = encode_header(energy.header); !result)
        return std::unexpected(result.error());
    if (energy.readings_j.size() < 2 || energy.readings_j.size() > UINT32_MAX)
        return invalid<void>(
            ErrorCode::InvalidValue, "energy burst needs at least two readings", "readings_j");
    for (std::size_t i = 0; i < energy.readings_j.size(); ++i)
        if (!std::isfinite(energy.readings_j[i]) || energy.readings_j[i] <= 0)
            return invalid<void>(ErrorCode::InvalidValue,
                "energy must be finite and positive in joules",
                "readings_j[" + std::to_string(i) + "]");
    return { };
}

Result<std::vector<std::uint8_t>> encode_dark(const DarkFile& dark, ResourceLimits limits)
{
    if (auto result = validate_dark(dark); !result)
        return std::unexpected(result.error());
    const auto header = encode_header(dark.header);
    if (!header)
        return std::unexpected(header.error());
    std::vector<std::uint8_t> bytes(header->begin(), header->end());
    std::uint64_t sequence = 0;
    Writer config;
    config.u16(dark.config.capture.peak_extraction_rule);
    config.u16(dark.config.capture.peak_extraction_rule_version);
    config.u32(dark.config.capture.target_accepted_readings);
    config.u32(dark.config.capture.maximum_rejected_attempts);
    config.u32(dark.config.capture.pretrigger_sample_count);
    config.u32(dark.config.capture.posttrigger_sample_count);
    config.u32(0);
    (void)config.f64(dark.config.capture.sample_rate_hz);
    (void)config.f64(dark.config.ai0_minimum_v);
    (void)config.f64(dark.config.ai0_maximum_v);
    (void)config.f64(dark.config.ai1_minimum_v);
    (void)config.f64(dark.config.ai1_maximum_v);
    if (auto result = append(bytes, FrameType::DarkConfig, sequence, std::move(config), limits);
        !result)
        return std::unexpected(result.error());
    for (const auto& attempt : dark.attempts) {
        Writer shot;
        if (const auto* accepted = std::get_if<AcceptedShot>(&attempt)) {
            shot.u32(accepted->attempt_index);
            shot.u32(0);
            (void)shot.f64(accepted->ai0_peak_v);
            (void)shot.f64(accepted->ai1_peak_v);
            if (auto result
                = append(bytes, FrameType::DarkAcceptedShot, sequence, std::move(shot), limits);
                !result)
                return std::unexpected(result.error());
        } else {
            const auto& rejected = std::get<RejectedAttempt>(attempt);
            shot.u32(rejected.attempt_index);
            shot.u16(static_cast<std::uint16_t>(rejected.reason));
            shot.u16(0);
            shot.u32(std::bit_cast<std::uint32_t>(rejected.vendor_error_code));
            shot.u32(0);
            if (auto result
                = append(bytes, FrameType::DarkRejectedAttempt, sequence, std::move(shot), limits);
                !result)
                return std::unexpected(result.error());
        }
    }
    Writer end;
    end.u16(static_cast<std::uint16_t>(dark.end.result));
    end.u16(static_cast<std::uint16_t>(dark.end.reason));
    end.u32(dark.end.accepted_count);
    end.u32(dark.end.rejected_count);
    end.u32(0);
    if (auto result = append(bytes, FrameType::DarkEnd, sequence, std::move(end), limits); !result)
        return std::unexpected(result.error());
    return bytes;
}

Result<DarkFile> decode_dark(std::span<const std::uint8_t> bytes, ResourceLimits limits)
{
    auto header = decode_header(bytes);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != FileKind::Dark)
        return invalid<DarkFile>(ErrorCode::InvalidBinary, "expected dark file", "header.kind");
    DarkFile dark;
    dark.header = *header;
    FrameReader reader(bytes.subspan(header_size), limits);
    bool have_config = false;
    bool ended = false;
    while (true) {
        auto next = reader.next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        const auto& frame = **next;
        const auto type = static_cast<FrameType>(frame.type);
        if ((!have_config && type != FrameType::DarkConfig) || ended)
            return invalid<DarkFile>(ErrorCode::InvalidBinary, "dark frame out of order",
                "frame " + std::to_string(frame.sequence));
        Reader payload(frame.payload);
        switch (type) {
        case FrameType::DarkConfig:
            if (have_config || !sized(frame, 64))
                return invalid<DarkFile>(
                    ErrorCode::InvalidBinary, "invalid dark config", "dark_config");
            dark.config.capture.peak_extraction_rule = *payload.u16();
            dark.config.capture.peak_extraction_rule_version = *payload.u16();
            dark.config.capture.target_accepted_readings = *payload.u32();
            dark.config.capture.maximum_rejected_attempts = *payload.u32();
            dark.config.capture.pretrigger_sample_count = *payload.u32();
            dark.config.capture.posttrigger_sample_count = *payload.u32();
            if (*payload.u32() != 0)
                return invalid<DarkFile>(
                    ErrorCode::InvalidBinary, "nonzero dark config reserved field", "dark_config");
            dark.config.capture.sample_rate_hz = raw_f64(payload);
            dark.config.ai0_minimum_v = raw_f64(payload);
            dark.config.ai0_maximum_v = raw_f64(payload);
            dark.config.ai1_minimum_v = raw_f64(payload);
            dark.config.ai1_maximum_v = raw_f64(payload);
            have_config = true;
            break;
        case FrameType::DarkAcceptedShot: {
            if (!sized(frame, 24))
                return invalid<DarkFile>(
                    ErrorCode::InvalidBinary, "wrong accepted-dark size", "dark_accepted_shot");
            const auto index = *payload.u32();
            if (*payload.u32() != 0 || index != dark.attempts.size())
                return invalid<DarkFile>(ErrorCode::InvalidBinary,
                    "invalid dark shot index or reserved field", "dark_accepted_shot");
            dark.attempts.emplace_back(AcceptedShot { index, raw_f64(payload), raw_f64(payload) });
            break;
        }
        case FrameType::DarkRejectedAttempt: {
            if (!sized(frame, 16))
                return invalid<DarkFile>(
                    ErrorCode::InvalidBinary, "wrong rejected-dark size", "dark_rejected_attempt");
            const auto index = *payload.u32();
            const auto reason = *payload.u16();
            const auto reserved = *payload.u16();
            const auto vendor = std::bit_cast<std::int32_t>(*payload.u32());
            if (index != dark.attempts.size() || reserved != 0 || *payload.u32() != 0
                || !valid_reason(static_cast<RejectionReason>(reason)))
                return invalid<DarkFile>(ErrorCode::InvalidBinary, "invalid rejected-dark fields",
                    "dark_rejected_attempt");
            dark.attempts.emplace_back(
                RejectedAttempt { index, static_cast<RejectionReason>(reason), vendor });
            break;
        }
        case FrameType::DarkEnd:
            if (!sized(frame, 16))
                return invalid<DarkFile>(
                    ErrorCode::InvalidBinary, "wrong dark end size", "dark_end");
            dark.end.result = static_cast<DarkResult>(*payload.u16());
            dark.end.reason = static_cast<DarkTerminationReason>(*payload.u16());
            dark.end.accepted_count = *payload.u32();
            dark.end.rejected_count = *payload.u32();
            if (*payload.u32() != 0 || reader.offset() != bytes.size() - header_size)
                return invalid<DarkFile>(
                    ErrorCode::InvalidBinary, "dark end reserved or trailing data", "dark_end");
            ended = true;
            break;
        default:
            return invalid<DarkFile>(ErrorCode::InvalidBinary, "non-dark frame",
                "frame " + std::to_string(frame.sequence));
        }
    }
    if (!have_config || !ended)
        return invalid<DarkFile>(
            ErrorCode::TruncatedBinary, "dark missing valid closing frame", "dark_end");
    if (auto result = validate_dark(dark); !result)
        return std::unexpected(result.error());
    return dark;
}

Result<std::vector<std::uint8_t>> encode_energy(const EnergyFile& energy, ResourceLimits limits)
{
    if (auto result = validate_energy(energy); !result)
        return std::unexpected(result.error());
    auto header = encode_header(energy.header);
    if (!header)
        return std::unexpected(header.error());
    std::vector<std::uint8_t> bytes(header->begin(), header->end());
    std::uint64_t sequence = 0;
    const auto count = static_cast<std::uint32_t>(energy.readings_j.size());
    Writer config;
    config.u32(count);
    config.u32(0);
    if (auto result = append(bytes, FrameType::EnergyConfig, sequence, std::move(config), limits);
        !result)
        return std::unexpected(result.error());
    Writer readings;
    readings.u32(count);
    readings.u32(0);
    for (double value : energy.readings_j)
        (void)readings.f64(value);
    if (auto result
        = append(bytes, FrameType::EnergyReadings, sequence, std::move(readings), limits);
        !result)
        return std::unexpected(result.error());
    Writer end;
    end.u32(count);
    end.u32(0);
    if (auto result = append(bytes, FrameType::EnergyEnd, sequence, std::move(end), limits);
        !result)
        return std::unexpected(result.error());
    return bytes;
}

Result<EnergyFile> decode_energy(std::span<const std::uint8_t> bytes, ResourceLimits limits)
{
    auto header = decode_header(bytes);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != FileKind::Energy)
        return invalid<EnergyFile>(ErrorCode::InvalidBinary, "expected energy file", "header.kind");
    EnergyFile energy;
    energy.header = *header;
    FrameReader reader(bytes.subspan(header_size), limits);
    std::uint32_t count = 0;
    for (FrameType expected :
        { FrameType::EnergyConfig, FrameType::EnergyReadings, FrameType::EnergyEnd }) {
        auto next = reader.next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next || (**next).type != static_cast<std::uint16_t>(expected))
            return invalid<EnergyFile>(ErrorCode::InvalidBinary,
                "energy frame sequence is incomplete or out of order", "energy_frames");
        const Frame& frame = **next;
        Reader payload(frame.payload);
        if (expected != FrameType::EnergyReadings && !sized(frame, 8))
            return invalid<EnergyFile>(
                ErrorCode::InvalidBinary, "wrong energy config/end size", "energy_frames");
        if (expected == FrameType::EnergyConfig) {
            count = *payload.u32();
            if (*payload.u32() != 0 || count < 2)
                return invalid<EnergyFile>(
                    ErrorCode::InvalidBinary, "invalid energy reading count", "energy_config");
        } else if (expected == FrameType::EnergyReadings) {
            if (frame.payload.size() < 8 || count > (frame.payload.size() - 8) / 8
                || frame.payload.size() != 8 + 8 * std::size_t(count))
                return invalid<EnergyFile>(ErrorCode::InvalidBinary,
                    "energy reading payload length disagrees", "energy_readings");
            if (*payload.u32() != count || *payload.u32() != 0)
                return invalid<EnergyFile>(ErrorCode::InvalidBinary,
                    "energy array count or reserved field differs", "energy_readings");
            energy.readings_j.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                energy.readings_j.push_back(raw_f64(payload));
        } else if (*payload.u32() != count || *payload.u32() != 0)
            return invalid<EnergyFile>(
                ErrorCode::InvalidBinary, "energy footer count differs", "energy_end");
    }
    if (reader.offset() != bytes.size() - header_size)
        return invalid<EnergyFile>(
            ErrorCode::InvalidBinary, "frames after energy end", "energy_end");
    if (auto result = validate_energy(energy); !result)
        return std::unexpected(result.error());
    return energy;
}

} // namespace zscan::schema::binary
