#include "zscan/schema/scan.hpp"

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

    bool same_bits(double a, double b)
    {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    }

    bool valid_reason(RejectionReason reason)
    {
        const auto value = static_cast<std::uint16_t>(reason);
        return (value >= 1 && value <= 9) || value == 255;
    }

    bool valid_termination(TerminationReason reason)
    {
        const auto value = static_cast<std::uint16_t>(reason);
        return value <= 6 || value == 255;
    }

    bool valid_position_result(PositionResult result)
    {
        return static_cast<std::uint16_t>(result) <= 4;
    }

    bool finite_between(double value, double minimum, double maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }

    std::uint32_t accepted_count(const ScanPosition& position)
    {
        return static_cast<std::uint32_t>(std::count_if(position.attempts.begin(),
            position.attempts.end(),
            [](const ShotAttempt& shot) { return std::holds_alternative<AcceptedShot>(shot); }));
    }

    // Payload fields are guaranteed present by the exact-size checks before reads.
    double raw_f64(Reader& reader) { return std::bit_cast<double>(*reader.u64()); }

    Result<void> write_f64(Writer& writer, double value)
    {
        // The shared primitive checks finiteness; semantic checks run first as well.
        return writer.f64(value);
    }

    Result<void> append_frame(std::vector<std::uint8_t>& file, FrameType type,
        std::uint64_t& sequence, Writer&& payload, ResourceLimits limits)
    {
        if (sequence >= limits.max_frames)
            return invalid<void>(
                ErrorCode::ResourceLimit, "scan exceeds frame count limit", "frame_count");
        auto frame = encode_frame(
            { static_cast<std::uint16_t>(type), 0, sequence, std::move(payload).take() }, limits);
        if (!frame)
            return std::unexpected(frame.error());
        file.insert(file.end(), frame->begin(), frame->end());
        ++sequence;
        return { };
    }

    Result<void> check_payload_size(const Frame& frame, std::size_t expected)
    {
        if (frame.payload.size() != expected)
            return invalid<void>(ErrorCode::InvalidBinary, "wrong scan frame payload size",
                "frame " + std::to_string(frame.sequence));
        return { };
    }

} // namespace

Result<void> validate_scan(const ScanFile& scan)
{
    if (scan.header.kind != FileKind::Scan)
        return invalid<void>(ErrorCode::InvalidBinary, "expected scan file kind", "header.kind");
    if (auto header = encode_header(scan.header); !header)
        return std::unexpected(header.error());
    const auto& config = scan.config;
    if (config.kind != ScanKind::Sample && config.kind != ScanKind::Blank)
        return invalid<void>(ErrorCode::InvalidValue, "unknown scan kind", "config.kind");
    if (config.maximum_move_attempts != 2)
        return invalid<void>(
            ErrorCode::InvalidValue, "v1 allows one move retry", "config.maximum_move_attempts");
    if (auto result = validate(config.grid); !result)
        return std::unexpected(result.error());
    if (auto result = validate(config.capture); !result)
        return std::unexpected(result.error());
    if (!std::isfinite(config.maximum_readback_error_m) || config.maximum_readback_error_m <= 0)
        return invalid<void>(ErrorCode::InvalidValue, "readback limit must be positive",
            "config.maximum_readback_error_m");
    for (const auto [minimum, maximum] : { std::pair { config.ai0_minimum_v, config.ai0_maximum_v },
             std::pair { config.ai1_minimum_v, config.ai1_maximum_v } }) {
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum)
            return invalid<void>(
                ErrorCode::InvalidValue, "invalid channel voltage range", "config.voltage_range_v");
    }
    const auto n = config.grid.position_count;
    if (config.commanded_positions_m.size() != n)
        return invalid<void>(
            ErrorCode::InvalidValue, "wrong commanded grid length", "config.commanded_positions_m");
    const double first = config.grid.start_z_m;
    const double last = config.grid.end_z_m;
    const double step = (last - first) / (n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const auto value = config.commanded_positions_m[i];
        const double expected = first + step * static_cast<double>(i);
        const double scale
            = std::max({ std::abs(first), std::abs(last), std::abs(expected), 1e-9 });
        if (!std::isfinite(value) || (i == 0 && !same_bits(value, first))
            || (i == n - 1 && !same_bits(value, last))
            || std::abs(value - expected) > 64 * std::numeric_limits<double>::epsilon() * scale
            || (i > 0
                && ((last > first && value <= config.commanded_positions_m[i - 1])
                    || (last < first && value >= config.commanded_positions_m[i - 1]))))
            return invalid<void>(ErrorCode::InvalidValue,
                "commanded positions must be a strict linear grid",
                "config.commanded_positions_m[" + std::to_string(i) + "]");
    }
    if (scan.positions.size() > n)
        return invalid<void>(ErrorCode::InvalidValue, "too many scan positions", "positions");

    std::uint64_t total_accepted = 0;
    std::uint64_t total_rejected = 0;
    std::uint32_t completed = 0;
    for (std::size_t i = 0; i < scan.positions.size(); ++i) {
        const auto& pos = scan.positions[i];
        const auto location = "positions[" + std::to_string(i) + "]";
        if (pos.position_index != i
            || !same_bits(pos.commanded_z_m, config.commanded_positions_m[i]))
            return invalid<void>(
                ErrorCode::InvalidValue, "position index or commanded z mismatch", location);
        if (pos.move_checks.size() > 2 || !valid_position_result(pos.result))
            return invalid<void>(
                ErrorCode::InvalidValue, "invalid position result or move count", location);
        bool move_succeeded = false;
        for (std::size_t j = 0; j < pos.move_checks.size(); ++j) {
            const auto& move = pos.move_checks[j];
            if (move.move_attempt_index != j || !same_bits(move.commanded_z_m, pos.commanded_z_m)
                || !std::isfinite(move.readback_z_m)
                || (move.result != MoveResult::WithinTolerance
                    && move.result != MoveResult::OutsideTolerance)
                || move_succeeded)
                return invalid<void>(ErrorCode::InvalidValue,
                    "invalid move check or retry after success", location + ".move_checks");
            const double delta = std::abs(move.readback_z_m - pos.commanded_z_m);
            if (!std::isfinite(delta)
                || (move.result == MoveResult::WithinTolerance)
                    != (delta <= config.maximum_readback_error_m))
                return invalid<void>(ErrorCode::InvalidValue,
                    "move result contradicts readback limit", location + ".move_checks");
            move_succeeded = move.result == MoveResult::WithinTolerance;
        }
        if (!move_succeeded && !pos.attempts.empty())
            return invalid<void>(ErrorCode::InvalidValue,
                "cannot record shots at an unchecked position", location + ".attempts");
        if (pos.attempts.size() > std::uint64_t(config.capture.target_accepted_readings)
                    + config.capture.maximum_rejected_attempts
            || pos.attempts.size() > std::numeric_limits<std::uint32_t>::max())
            return invalid<void>(ErrorCode::InvalidValue,
                "position attempts exceed configured limits", location + ".attempts");
        const auto accepted = accepted_count(pos);
        const auto rejected = static_cast<std::uint32_t>(pos.attempts.size()) - accepted;
        if (accepted > config.capture.target_accepted_readings
            || rejected > config.capture.maximum_rejected_attempts)
            return invalid<void>(ErrorCode::InvalidValue,
                "accepted or rejected count exceeds target", location + ".attempts");
        std::uint32_t accepted_so_far = 0;
        for (std::size_t j = 0; j < pos.attempts.size(); ++j) {
            const auto& shot = pos.attempts[j];
            if (accepted_so_far == config.capture.target_accepted_readings)
                return invalid<void>(ErrorCode::InvalidValue, "attempt after accepted-shot target",
                    location + ".attempts[" + std::to_string(j) + "]");
            if (const auto* reading = std::get_if<AcceptedShot>(&shot)) {
                if (reading->attempt_index != j
                    || !finite_between(
                        reading->ai0_peak_v, config.ai0_minimum_v, config.ai0_maximum_v)
                    || !finite_between(
                        reading->ai1_peak_v, config.ai1_minimum_v, config.ai1_maximum_v))
                    return invalid<void>(ErrorCode::InvalidValue, "invalid accepted detector pair",
                        location + ".attempts[" + std::to_string(j) + "]");
                ++accepted_so_far;
            } else {
                const auto& rejection = std::get<RejectedAttempt>(shot);
                if (rejection.attempt_index != j || !valid_reason(rejection.reason))
                    return invalid<void>(ErrorCode::InvalidValue, "invalid rejected attempt",
                        location + ".attempts[" + std::to_string(j) + "]");
            }
        }
        switch (pos.result) {
        case PositionResult::Complete:
            if (!move_succeeded || accepted != config.capture.target_accepted_readings)
                return invalid<void>(ErrorCode::InvalidValue,
                    "complete position needs an in-tolerance move and target readings", location);
            ++completed;
            break;
        case PositionResult::StagePositionError:
            if (pos.move_checks.size() != 2 || move_succeeded || !pos.attempts.empty())
                return invalid<void>(ErrorCode::InvalidValue,
                    "stage error needs two failed checks and no shots", location);
            break;
        case PositionResult::ShotRetryLimit:
            if (!move_succeeded || accepted == config.capture.target_accepted_readings
                || rejected == 0 || rejected != config.capture.maximum_rejected_attempts)
                return invalid<void>(ErrorCode::InvalidValue,
                    "retry-limit position must exhaust rejected attempts", location);
            break;
        case PositionResult::OperatorCancelled:
        case PositionResult::AcquisitionError:
            break;
        }
        if (pos.result != PositionResult::Complete && i + 1 != scan.positions.size())
            return invalid<void>(
                ErrorCode::InvalidValue, "cannot continue after an incomplete position", location);
        total_accepted += accepted;
        total_rejected += rejected;
    }
    const auto& end = scan.end;
    if (end.positions_started != scan.positions.size() || end.positions_completed != completed
        || end.total_accepted_shots != total_accepted
        || end.total_rejected_attempts != total_rejected)
        return invalid<void>(
            ErrorCode::InvalidValue, "scan-end totals disagree with frames", "scan_end");
    const bool all_complete = scan.positions.size() == n && completed == n;
    if (!valid_termination(end.reason)
        || (end.result == ScanResult::Complete
            && (!all_complete || end.reason != TerminationReason::NormalCompletion))
        || (end.result == ScanResult::Incomplete
            && (all_complete || end.reason == TerminationReason::NormalCompletion))
        || (end.result != ScanResult::Complete && end.result != ScanResult::Incomplete))
        return invalid<void>(
            ErrorCode::InvalidValue, "invalid scan completion state", "scan_end.result");
    if (!scan.positions.empty() && scan.positions.back().result != PositionResult::Complete) {
        const auto last_result = scan.positions.back().result;
        if ((last_result == PositionResult::StagePositionError
                && end.reason != TerminationReason::StagePositionError)
            || (last_result == PositionResult::ShotRetryLimit
                && end.reason != TerminationReason::ShotRetryLimit)
            || (last_result == PositionResult::OperatorCancelled
                && end.reason != TerminationReason::OperatorCancelled)
            || (last_result == PositionResult::AcquisitionError
                && end.reason != TerminationReason::DaqError
                && end.reason != TerminationReason::ControllerError
                && end.reason != TerminationReason::ApplicationError
                && end.reason != TerminationReason::Other))
            return invalid<void>(ErrorCode::InvalidValue,
                "scan termination contradicts final position", "scan_end.reason");
    }
    if (end.reason == TerminationReason::StagePositionError
        && (scan.positions.empty()
            || scan.positions.back().result != PositionResult::StagePositionError))
        return invalid<void>(ErrorCode::InvalidValue, "stage error needs a failed position record",
            "scan_end.reason");
    if (end.reason == TerminationReason::ShotRetryLimit
        && (scan.positions.empty()
            || scan.positions.back().result != PositionResult::ShotRetryLimit))
        return invalid<void>(ErrorCode::InvalidValue, "shot limit needs a failed position record",
            "scan_end.reason");
    return { };
}

Result<std::vector<std::uint8_t>> encode_scan(const ScanFile& scan, ResourceLimits limits)
{
    if (auto result = validate_scan(scan); !result)
        return std::unexpected(result.error());
    const auto header = encode_header(scan.header);
    if (!header)
        return std::unexpected(header.error());
    std::vector<std::uint8_t> file(header->begin(), header->end());
    std::uint64_t sequence = 0;
    const auto& config = scan.config;
    Writer setup;
    setup.u8(config.kind == ScanKind::Blank ? 1 : 2);
    setup.u8(config.maximum_move_attempts);
    setup.u16(config.capture.peak_extraction_rule);
    setup.u16(config.capture.peak_extraction_rule_version);
    setup.u16(0);
    setup.u32(config.grid.position_count);
    setup.u32(config.capture.target_accepted_readings);
    setup.u32(config.capture.maximum_rejected_attempts);
    setup.u32(config.capture.pretrigger_sample_count);
    setup.u32(config.capture.posttrigger_sample_count);
    setup.u32(0);
    (void)write_f64(setup, config.capture.sample_rate_hz);
    (void)write_f64(setup, config.grid.start_z_m);
    (void)write_f64(setup, config.grid.end_z_m);
    (void)write_f64(setup, config.grid.spacing_m);
    (void)write_f64(setup, config.maximum_readback_error_m);
    (void)write_f64(setup, config.ai0_minimum_v);
    (void)write_f64(setup, config.ai0_maximum_v);
    (void)write_f64(setup, config.ai1_minimum_v);
    (void)write_f64(setup, config.ai1_maximum_v);
    for (double z : config.commanded_positions_m)
        (void)write_f64(setup, z);
    if (auto result = append_frame(file, FrameType::ScanConfig, sequence, std::move(setup), limits);
        !result)
        return std::unexpected(result.error());

    for (const auto& pos : scan.positions) {
        Writer begin;
        begin.u32(pos.position_index);
        begin.u32(0);
        (void)write_f64(begin, pos.commanded_z_m);
        if (auto result
            = append_frame(file, FrameType::PositionBegin, sequence, std::move(begin), limits);
            !result)
            return std::unexpected(result.error());
        for (const auto& check : pos.move_checks) {
            Writer move;
            move.u32(pos.position_index);
            move.u16(check.move_attempt_index);
            move.u8(static_cast<std::uint8_t>(check.result));
            move.u8(0);
            (void)write_f64(move, check.commanded_z_m);
            (void)write_f64(move, check.readback_z_m);
            if (auto result
                = append_frame(file, FrameType::MoveCheck, sequence, std::move(move), limits);
                !result)
                return std::unexpected(result.error());
        }
        for (const auto& attempt : pos.attempts) {
            Writer shot;
            shot.u32(pos.position_index);
            if (const auto* accepted = std::get_if<AcceptedShot>(&attempt)) {
                shot.u32(accepted->attempt_index);
                (void)write_f64(shot, accepted->ai0_peak_v);
                (void)write_f64(shot, accepted->ai1_peak_v);
                if (auto result = append_frame(
                        file, FrameType::AcceptedShot, sequence, std::move(shot), limits);
                    !result)
                    return std::unexpected(result.error());
            } else {
                const auto& rejected = std::get<RejectedAttempt>(attempt);
                shot.u32(rejected.attempt_index);
                shot.u16(static_cast<std::uint16_t>(rejected.reason));
                shot.u16(0);
                shot.u32(std::bit_cast<std::uint32_t>(rejected.vendor_error_code));
                if (auto result = append_frame(
                        file, FrameType::RejectedAttempt, sequence, std::move(shot), limits);
                    !result)
                    return std::unexpected(result.error());
            }
        }
        Writer finish;
        finish.u32(pos.position_index);
        finish.u32(accepted_count(pos));
        finish.u32(static_cast<std::uint32_t>(pos.attempts.size()) - accepted_count(pos));
        finish.u16(static_cast<std::uint16_t>(pos.move_checks.size()));
        finish.u16(static_cast<std::uint16_t>(pos.result));
        if (auto result
            = append_frame(file, FrameType::PositionEnd, sequence, std::move(finish), limits);
            !result)
            return std::unexpected(result.error());
    }
    Writer end;
    end.u16(static_cast<std::uint16_t>(scan.end.result));
    end.u16(static_cast<std::uint16_t>(scan.end.reason));
    end.u32(scan.end.positions_started);
    end.u32(scan.end.positions_completed);
    end.u32(0);
    end.u64(scan.end.total_accepted_shots);
    end.u64(scan.end.total_rejected_attempts);
    if (auto result = append_frame(file, FrameType::ScanEnd, sequence, std::move(end), limits);
        !result)
        return std::unexpected(result.error());
    return file;
}

Result<ScanFile> decode_scan(std::span<const std::uint8_t> bytes, ResourceLimits limits)
{
    auto header = decode_header(bytes);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != FileKind::Scan)
        return invalid<ScanFile>(ErrorCode::InvalidBinary, "expected scan file", "header.kind");
    ScanFile scan;
    scan.header = *header;
    FrameReader reader(bytes.subspan(header_size), limits);
    bool have_config = false;
    std::optional<ScanPosition> active;
    bool ended = false;
    while (true) {
        auto next = reader.next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        const auto& frame = **next;
        const auto type = static_cast<FrameType>(frame.type);
        if ((!have_config && type != FrameType::ScanConfig) || ended)
            return invalid<ScanFile>(ErrorCode::InvalidBinary, "invalid scan frame ordering",
                "frame " + std::to_string(frame.sequence));
        Reader payload(frame.payload);
        switch (type) {
        case FrameType::ScanConfig: {
            if (have_config || active || frame.payload.size() < 104)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "invalid scan configuration frame", "scan_config");
            const auto kind = *payload.u8();
            if (kind != 1 && kind != 2)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "unknown scan kind", "scan_config.kind");
            scan.config.kind = kind == 1 ? ScanKind::Blank : ScanKind::Sample;
            scan.config.maximum_move_attempts = *payload.u8();
            scan.config.capture.peak_extraction_rule = *payload.u16();
            scan.config.capture.peak_extraction_rule_version = *payload.u16();
            if (*payload.u16() != 0)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "nonzero reserved scan config", "scan_config");
            scan.config.grid.position_count = *payload.u32();
            scan.config.capture.target_accepted_readings = *payload.u32();
            scan.config.capture.maximum_rejected_attempts = *payload.u32();
            scan.config.capture.pretrigger_sample_count = *payload.u32();
            scan.config.capture.posttrigger_sample_count = *payload.u32();
            if (*payload.u32() != 0)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "nonzero reserved scan config", "scan_config");
            scan.config.capture.sample_rate_hz = raw_f64(payload);
            scan.config.grid.start_z_m = raw_f64(payload);
            scan.config.grid.end_z_m = raw_f64(payload);
            scan.config.grid.spacing_m = raw_f64(payload);
            scan.config.maximum_readback_error_m = raw_f64(payload);
            scan.config.ai0_minimum_v = raw_f64(payload);
            scan.config.ai0_maximum_v = raw_f64(payload);
            scan.config.ai1_minimum_v = raw_f64(payload);
            scan.config.ai1_maximum_v = raw_f64(payload);
            const auto count = scan.config.grid.position_count;
            if (count < 2 || count > (frame.payload.size() - 104) / 8
                || frame.payload.size() != 104 + 8 * std::size_t(count))
                return invalid<ScanFile>(ErrorCode::InvalidBinary,
                    "wrong commanded grid payload length", "scan_config.commanded_positions_m");
            scan.config.commanded_positions_m.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                scan.config.commanded_positions_m.push_back(raw_f64(payload));
            have_config = true;
            break;
        }
        case FrameType::PositionBegin: {
            if (active || !check_payload_size(frame, 16))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "unexpected position begin", "position_begin");
            ScanPosition pos;
            pos.position_index = *payload.u32();
            if (*payload.u32() != 0)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "nonzero reserved position begin", "position_begin");
            pos.commanded_z_m = raw_f64(payload);
            if (scan.positions.size() >= scan.config.grid.position_count
                || pos.position_index != scan.positions.size())
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "positions are not sequential", "position_begin");
            active = std::move(pos);
            break;
        }
        case FrameType::MoveCheck: {
            if (!active || !active->attempts.empty() || !check_payload_size(frame, 24))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "move check out of order", "move_check");
            const auto index = *payload.u32();
            const auto move_index = *payload.u16();
            const auto result = *payload.u8();
            const auto reserved = *payload.u8();
            const auto commanded = raw_f64(payload);
            const auto readback = raw_f64(payload);
            if (index != active->position_index || reserved != 0 || result > 1
                || active->move_checks.size() >= 2 || move_index != active->move_checks.size())
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "invalid move check", "move_check");
            active->move_checks.push_back(
                { move_index, static_cast<MoveResult>(result), commanded, readback });
            break;
        }
        case FrameType::AcceptedShot: {
            if (!active || !check_payload_size(frame, 24))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "accepted shot out of order", "accepted_shot");
            const auto index = *payload.u32();
            const auto attempt = *payload.u32();
            const auto ai0 = raw_f64(payload);
            const auto ai1 = raw_f64(payload);
            if (index != active->position_index || attempt != active->attempts.size())
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "invalid accepted shot index", "accepted_shot");
            active->attempts.emplace_back(AcceptedShot { attempt, ai0, ai1 });
            break;
        }
        case FrameType::RejectedAttempt: {
            if (!active || !check_payload_size(frame, 16))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "rejected attempt out of order", "rejected_attempt");
            const auto index = *payload.u32();
            const auto attempt = *payload.u32();
            const auto reason = *payload.u16();
            const auto reserved = *payload.u16();
            const auto vendor = std::bit_cast<std::int32_t>(*payload.u32());
            if (index != active->position_index || attempt != active->attempts.size()
                || reserved != 0 || !valid_reason(static_cast<RejectionReason>(reason)))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "invalid rejected attempt", "rejected_attempt");
            active->attempts.emplace_back(
                RejectedAttempt { attempt, static_cast<RejectionReason>(reason), vendor });
            break;
        }
        case FrameType::PositionEnd: {
            if (!active || !check_payload_size(frame, 16))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "position end out of order", "position_end");
            const auto index = *payload.u32();
            const auto accepted = *payload.u32();
            const auto rejected = *payload.u32();
            const auto moves = *payload.u16();
            const auto result = *payload.u16();
            if (index != active->position_index || accepted != accepted_count(*active)
                || rejected != active->attempts.size() - accepted
                || moves != active->move_checks.size()
                || !valid_position_result(static_cast<PositionResult>(result)))
                return invalid<ScanFile>(ErrorCode::InvalidBinary,
                    "position footer counts or result disagree", "position_end");
            active->result = static_cast<PositionResult>(result);
            scan.positions.push_back(std::move(*active));
            active.reset();
            break;
        }
        case FrameType::ScanEnd: {
            if (active || !check_payload_size(frame, 32))
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "scan end before position end", "scan_end");
            scan.end.result = static_cast<ScanResult>(*payload.u16());
            scan.end.reason = static_cast<TerminationReason>(*payload.u16());
            scan.end.positions_started = *payload.u32();
            scan.end.positions_completed = *payload.u32();
            if (*payload.u32() != 0)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "nonzero reserved scan end", "scan_end");
            scan.end.total_accepted_shots = *payload.u64();
            scan.end.total_rejected_attempts = *payload.u64();
            ended = true;
            if (reader.offset() != bytes.size() - header_size)
                return invalid<ScanFile>(
                    ErrorCode::InvalidBinary, "frames after scan end", "scan_end");
            break;
        }
        default:
            return invalid<ScanFile>(ErrorCode::InvalidBinary, "unexpected non-scan frame",
                "frame " + std::to_string(frame.sequence));
        }
    }
    if (!have_config || !ended || active)
        return invalid<ScanFile>(
            ErrorCode::TruncatedBinary, "scan lacks a valid closing frame", "scan_end");
    if (auto result = validate_scan(scan); !result)
        return std::unexpected(result.error());
    return scan;
}

} // namespace zscan::schema::binary
