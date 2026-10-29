#include "zscan/acquisition/dark_runner.hpp"

#include <chrono>
#include <cmath>

namespace zscan::acquisition {

DarkRunner::DarkRunner(schema::binary::DarkConfig config, schema::Uuid session_id,
    schema::Uuid record_id, hardware::DetectorAcquisition& detector)
    : config_(config)
    , session_id_(session_id)
    , record_id_(record_id)
    , detector_(detector)
{
}

DarkOutcome DarkRunner::run()
{
    const auto now
        = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    DarkOutcome outcome;
    auto& dark = outcome.dark;
    dark.header = { schema::binary::FileKind::Dark, record_id_, session_id_,
        { std::chrono::sys_seconds { now.time_since_epoch() }, 0 } };
    dark.config = config_;
    for (std::uint32_t accepted = 0; accepted < config_.capture.target_accepted_readings;) {
        if (cancelled_.load()) {
            outcome.error = "dark cancelled";
            dark.end.reason = schema::binary::DarkTerminationReason::OperatorCancelled;
            break;
        }
        const auto capture = detector_.capture_one();
        const auto index = static_cast<std::uint32_t>(dark.attempts.size());
        if (!capture) {
            dark.attempts.emplace_back(
                schema::binary::RejectedAttempt { index, capture.error().reason, 0 });
            ++dark.end.rejected_count;
        } else if (!std::isfinite(capture->ai0_peak_v) || !std::isfinite(capture->ai1_peak_v)
            || capture->ai0_peak_v < config_.ai0_minimum_v
            || capture->ai0_peak_v > config_.ai0_maximum_v
            || capture->ai1_peak_v < config_.ai1_minimum_v
            || capture->ai1_peak_v > config_.ai1_maximum_v) {
            const auto reason = !std::isfinite(capture->ai0_peak_v)
                ? schema::binary::RejectionReason::NonfiniteAi0
                : !std::isfinite(capture->ai1_peak_v)
                ? schema::binary::RejectionReason::NonfiniteAi1
                : capture->ai0_peak_v < config_.ai0_minimum_v
                    || capture->ai0_peak_v > config_.ai0_maximum_v
                ? schema::binary::RejectionReason::Ai0OutOfRange
                : schema::binary::RejectionReason::Ai1OutOfRange;
            dark.attempts.emplace_back(schema::binary::RejectedAttempt { index, reason, 0 });
            ++dark.end.rejected_count;
        } else {
            dark.attempts.emplace_back(
                schema::binary::AcceptedShot { index, capture->ai0_peak_v, capture->ai1_peak_v });
            ++accepted;
            ++dark.end.accepted_count;
        }
        if (dark.end.rejected_count >= config_.capture.maximum_rejected_attempts
            && accepted < config_.capture.target_accepted_readings) {
            outcome.error = "dark shot retry limit";
            dark.end.reason = schema::binary::DarkTerminationReason::ShotRetryLimit;
            break;
        }
    }
    if (dark.end.accepted_count == config_.capture.target_accepted_readings) {
        dark.end.result = schema::binary::DarkResult::Complete;
        dark.end.reason = schema::binary::DarkTerminationReason::NormalCompletion;
    } else {
        dark.end.result = schema::binary::DarkResult::Incomplete;
        if (outcome.error.empty())
            outcome.error = "dark acquisition incomplete";
    }
    return outcome;
}

} // namespace zscan::acquisition
