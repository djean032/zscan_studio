#pragma once

#include "zscan/schema/binary.hpp"

#include <variant>

namespace zscan::schema::binary {

enum class MoveResult : std::uint8_t { WithinTolerance = 0, OutsideTolerance = 1 };
enum class RejectionReason : std::uint16_t {
    TriggerTimeout = 1,
    DaqError = 2,
    MissingAi0 = 3,
    MissingAi1 = 4,
    NonfiniteAi0 = 5,
    NonfiniteAi1 = 6,
    Ai0OutOfRange = 7,
    Ai1OutOfRange = 8,
    MultipleChannelFailures = 9,
    Other = 255,
};
enum class PositionResult : std::uint16_t {
    Complete = 0,
    StagePositionError = 1,
    ShotRetryLimit = 2,
    OperatorCancelled = 3,
    AcquisitionError = 4,
};
enum class ScanResult : std::uint16_t { Complete = 0, Incomplete = 1 };
enum class TerminationReason : std::uint16_t {
    NormalCompletion = 0,
    OperatorCancelled = 1,
    StagePositionError = 2,
    ShotRetryLimit = 3,
    DaqError = 4,
    ControllerError = 5,
    ApplicationError = 6,
    Other = 255,
};

struct ScanConfig {
    ScanKind kind { ScanKind::Blank };
    std::uint8_t maximum_move_attempts { 2 };
    ScanGrid grid;
    CaptureSettings capture;
    double maximum_readback_error_m { };
    double ai0_minimum_v { };
    double ai0_maximum_v { };
    double ai1_minimum_v { };
    double ai1_maximum_v { };
    std::vector<double> commanded_positions_m;
};

struct MoveCheck {
    std::uint16_t move_attempt_index { };
    MoveResult result { MoveResult::WithinTolerance };
    double commanded_z_m { };
    double readback_z_m { };
};

struct AcceptedShot {
    std::uint32_t attempt_index { };
    double ai0_peak_v { };
    double ai1_peak_v { };
};

struct RejectedAttempt {
    std::uint32_t attempt_index { };
    RejectionReason reason { RejectionReason::Other };
    std::int32_t vendor_error_code { }; // Zero means unavailable.
};

using ShotAttempt = std::variant<AcceptedShot, RejectedAttempt>;

struct ScanPosition {
    std::uint32_t position_index { };
    double commanded_z_m { };
    std::vector<MoveCheck> move_checks;
    std::vector<ShotAttempt> attempts;
    PositionResult result { PositionResult::Complete };
};

struct ScanEnd {
    ScanResult result { ScanResult::Complete };
    TerminationReason reason { TerminationReason::NormalCompletion };
    std::uint32_t positions_started { };
    std::uint32_t positions_completed { };
    std::uint64_t total_accepted_shots { };
    std::uint64_t total_rejected_attempts { };
};

struct ScanFile {
    Header header;
    ScanConfig config;
    std::vector<ScanPosition> positions;
    ScanEnd end;
};

// Semantic checks shared by writer and reader; no audit-only file can be used
// as a production scan unless end.result == ScanResult::Complete.
[[nodiscard]] Result<void> validate_scan(const ScanFile& scan);

// Encodes a sealed scan in memory. Phase 5 will add incremental on-disk writes
// and interrupted-prefix recovery using the same frame payload rules.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_scan(
    const ScanFile& scan, ResourceLimits limits = { });
[[nodiscard]] Result<ScanFile> decode_scan(
    std::span<const std::uint8_t> bytes, ResourceLimits limits = { });

} // namespace zscan::schema::binary
