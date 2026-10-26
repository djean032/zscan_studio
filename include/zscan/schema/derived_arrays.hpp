#pragma once

#include "zscan/schema/binary.hpp"

namespace zscan::schema::binary {

enum class ArrayRole : std::uint16_t {
    ScanPositions = 1,
    Transmission = 2,
    TransmissionStandardErrors = 3,
    DarkSensitivities = 4,
    SampleRetainedAttemptIndexes = 5,
    BlankRetainedAttemptIndexes = 6,
    MeterMeanSem = 7,
    KappaValueUncertainty = 8,
    JointMeasurementCovariance = 9,
    PredictedTransmission = 10,
    ConditionalParameterCovariance = 11,
    TotalParameterCovariance = 12,
    CalibrationVarianceContributions = 13,
};

inline constexpr std::uint32_t global_array_index = UINT32_MAX;

struct DenseArray {
    ArrayRole role { ArrayRole::ScanPositions };
    std::uint32_t curve_index { global_array_index };
    std::uint64_t rows { };
    std::uint64_t columns { };
    std::vector<double> values_row_major;
};

struct DerivedArrayFile {
    Header header;
    std::vector<DenseArray> arrays;
};

[[nodiscard]] Result<void> validate_arrays(const DerivedArrayFile& file);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_arrays(
    const DerivedArrayFile& file, ResourceLimits limits = { 256 * 1024 * 1024, 100'000 });
[[nodiscard]] Result<DerivedArrayFile> decode_arrays(
    std::span<const std::uint8_t> bytes, ResourceLimits limits = { 256 * 1024 * 1024, 100'000 });

} // namespace zscan::schema::binary
