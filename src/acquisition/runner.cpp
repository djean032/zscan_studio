#include "zscan/acquisition/runner.hpp"

#include <chrono>
#include <cmath>

namespace zscan::acquisition {
namespace {

    schema::UtcTimestamp now_utc()
    {
        const auto now = std::chrono::system_clock::now();
        const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
        const auto nanos
            = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count();
        return { std::chrono::sys_seconds { seconds.time_since_epoch() },
            static_cast<std::uint32_t>(nanos) };
    }

    bool in_range(double value, double minimum, double maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }

} // namespace

Runner::Runner(RunConfiguration configuration, hardware::StageController& stage,
    hardware::DetectorAcquisition& detector)
    : configuration_(std::move(configuration))
    , stage_(stage)
    , detector_(detector)
{
}

bool Runner::stopped() const noexcept
{
    return cancel_requested_.load() || emergency_stop_requested_.load();
}

RunOutcome Runner::run()
{
    RunOutcome outcome;
    outcome.scan.header = { schema::binary::FileKind::Scan, configuration_.acquisition_id,
        configuration_.session_id, now_utc() };
    outcome.scan.config = { configuration_.kind, configuration_.maximum_move_attempts,
        configuration_.grid, configuration_.capture, configuration_.maximum_readback_error_m,
        configuration_.ai0_minimum_v, configuration_.ai0_maximum_v, configuration_.ai1_minimum_v,
        configuration_.ai1_maximum_v, configuration_.positions_m };
    outcome.scan.end = { schema::binary::ScanResult::Complete,
        schema::binary::TerminationReason::NormalCompletion, 0, 0, 0, 0 };

    if (configuration_.positions_m.empty()) {
        outcome.error = "acquisition has no positions";
        outcome.scan.end = { schema::binary::ScanResult::Incomplete,
            schema::binary::TerminationReason::ApplicationError, 0, 0, 0, 0 };
        return outcome;
    }

    for (std::size_t position_index = 0; position_index < configuration_.positions_m.size();
        ++position_index) {
        if (stopped()) {
            outcome.scan.end.result = schema::binary::ScanResult::Incomplete;
            outcome.scan.end.reason = emergency_stop_requested_.load()
                ? schema::binary::TerminationReason::ControllerError
                : schema::binary::TerminationReason::OperatorCancelled;
            outcome.error
                = emergency_stop_requested_.load() ? "emergency stop" : "operator cancelled";
            break;
        }

        const double commanded = configuration_.positions_m[position_index];
        if (configuration_.progress_callback)
            configuration_.progress_callback(
                { position_index, configuration_.positions_m.size(), 0, 0, 0 });
        schema::binary::ScanPosition position;
        position.position_index = static_cast<std::uint32_t>(position_index);
        position.commanded_z_m = commanded;
        outcome.scan.end.positions_started++;

        bool moved = false;
        for (std::uint16_t attempt = 0; attempt < configuration_.maximum_move_attempts; ++attempt) {
            if (stopped())
                break;
            const auto moved_result = stage_.move_and_readback(commanded);
            if (!moved_result) {
                if (attempt + 1 == configuration_.maximum_move_attempts) {
                    position.result = schema::binary::PositionResult::StagePositionError;
                    outcome.error = moved_result.error().message;
                }
                continue;
            }
            const double error = std::abs(moved_result->readback_z_m - commanded);
            const auto result = error <= configuration_.maximum_readback_error_m
                ? schema::binary::MoveResult::WithinTolerance
                : schema::binary::MoveResult::OutsideTolerance;
            position.move_checks.push_back(
                { static_cast<std::uint16_t>(position.move_checks.size()), result, commanded,
                    moved_result->readback_z_m });
            if (result == schema::binary::MoveResult::WithinTolerance) {
                moved = true;
                break;
            }
        }
        if (!moved) {
            position.result = stopped() ? schema::binary::PositionResult::OperatorCancelled
                                        : schema::binary::PositionResult::StagePositionError;
            outcome.scan.positions.push_back(std::move(position));
            outcome.scan.end.result = schema::binary::ScanResult::Incomplete;
            outcome.scan.end.reason = stopped()
                ? schema::binary::TerminationReason::OperatorCancelled
                : schema::binary::TerminationReason::StagePositionError;
            outcome.scan.end.positions_completed = static_cast<std::uint32_t>(position_index);
            if (outcome.error.empty())
                outcome.error = stopped() ? "operator cancelled"
                                          : "stage readback remained outside tolerance";
            break;
        }

        std::uint32_t attempts = 0;
        std::uint32_t accepted = 0;
        std::uint32_t rejected = 0;
        while (accepted < configuration_.capture.target_accepted_readings) {
            if (stopped()) {
                outcome.error
                    = emergency_stop_requested_.load() ? "emergency stop" : "operator cancelled";
                break;
            }
            const auto index = attempts++;
            const auto capture = detector_.capture_one();
            if (!capture) {
                position.attempts.push_back(
                    { schema::binary::RejectedAttempt { index, capture.error().reason, 0 } });
                ++rejected;
                if (configuration_.progress_callback)
                    configuration_.progress_callback(
                        { position_index, configuration_.positions_m.size(), accepted,
                            configuration_.capture.target_accepted_readings, rejected });
                if (rejected >= configuration_.capture.maximum_rejected_attempts)
                    break;
                continue;
            }
            const bool valid = in_range(capture->ai0_peak_v, configuration_.ai0_minimum_v,
                                   configuration_.ai0_maximum_v)
                && in_range(capture->ai1_peak_v, configuration_.ai1_minimum_v,
                    configuration_.ai1_maximum_v);
            if (!valid) {
                const auto reason = !in_range(capture->ai0_peak_v, configuration_.ai0_minimum_v,
                                        configuration_.ai0_maximum_v)
                    ? schema::binary::RejectionReason::Ai0OutOfRange
                    : schema::binary::RejectionReason::Ai1OutOfRange;
                position.attempts.push_back(
                    { schema::binary::RejectedAttempt { index, reason, 0 } });
                ++rejected;
                if (configuration_.progress_callback)
                    configuration_.progress_callback(
                        { position_index, configuration_.positions_m.size(), accepted,
                            configuration_.capture.target_accepted_readings, rejected });
                if (rejected >= configuration_.capture.maximum_rejected_attempts)
                    break;
                continue;
            }
            position.attempts.push_back({ schema::binary::AcceptedShot {
                index, capture->ai0_peak_v, capture->ai1_peak_v } });
            ++accepted;
            if (configuration_.progress_callback)
                configuration_.progress_callback(
                    { position_index, configuration_.positions_m.size(), accepted,
                        configuration_.capture.target_accepted_readings, rejected });
        }

        if (accepted < configuration_.capture.target_accepted_readings) {
            position.result = stopped() ? schema::binary::PositionResult::OperatorCancelled
                                        : schema::binary::PositionResult::ShotRetryLimit;
            outcome.scan.positions.push_back(std::move(position));
            outcome.scan.end.result = schema::binary::ScanResult::Incomplete;
            outcome.scan.end.reason = stopped()
                ? (emergency_stop_requested_.load()
                          ? schema::binary::TerminationReason::ControllerError
                          : schema::binary::TerminationReason::OperatorCancelled)
                : schema::binary::TerminationReason::ShotRetryLimit;
            break;
        }

        outcome.scan.end.total_accepted_shots += accepted;
        outcome.scan.end.total_rejected_attempts += rejected;
        position.result = schema::binary::PositionResult::Complete;
        outcome.scan.positions.push_back(std::move(position));
        outcome.scan.end.positions_completed++;
    }

    if (outcome.scan.end.positions_completed == configuration_.positions_m.size()
        && outcome.scan.positions.size() == configuration_.positions_m.size()) {
        outcome.scan.end.result = schema::binary::ScanResult::Complete;
        outcome.scan.end.reason = schema::binary::TerminationReason::NormalCompletion;
    } else {
        outcome.scan.end.result = schema::binary::ScanResult::Incomplete;
        if (outcome.error.empty())
            outcome.error = "acquisition did not complete";
    }
    return outcome;
}

} // namespace zscan::acquisition
