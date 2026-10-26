#pragma once

#include "zscan/schema/scan.hpp"

namespace zscan::schema::binary {

enum class DarkResult : std::uint16_t { Complete = 0, Incomplete = 1 };
enum class DarkTerminationReason : std::uint16_t {
    NormalCompletion = 0,
    OperatorCancelled = 1,
    ShotRetryLimit = 2,
    DaqError = 3,
    ApplicationError = 4,
    Other = 255,
};

struct DarkConfig {
    CaptureSettings capture;
    double ai0_minimum_v { };
    double ai0_maximum_v { };
    double ai1_minimum_v { };
    double ai1_maximum_v { };
};

struct DarkEnd {
    DarkResult result { DarkResult::Complete };
    DarkTerminationReason reason { DarkTerminationReason::NormalCompletion };
    std::uint32_t accepted_count { };
    std::uint32_t rejected_count { };
};

// Dark files intentionally have no optical, OD, cuvette, stage, position or
// scan-configuration fields. DAQ identity and physical dark conditions live in
// the session manifest, which must be cross-validated later.
struct DarkFile {
    Header header;
    DarkConfig config;
    std::vector<ShotAttempt> attempts;
    DarkEnd end;
};

struct EnergyFile {
    Header header;
    std::vector<double> readings_j; // Typed values, in entry order, without kappa.
};

[[nodiscard]] Result<void> validate_dark(const DarkFile& dark);
[[nodiscard]] Result<void> validate_energy(const EnergyFile& energy);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_dark(
    const DarkFile& dark, ResourceLimits limits = { });
[[nodiscard]] Result<DarkFile> decode_dark(
    std::span<const std::uint8_t> bytes, ResourceLimits limits = { });
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_energy(
    const EnergyFile& energy, ResourceLimits limits = { });
[[nodiscard]] Result<EnergyFile> decode_energy(
    std::span<const std::uint8_t> bytes, ResourceLimits limits = { });

} // namespace zscan::schema::binary
