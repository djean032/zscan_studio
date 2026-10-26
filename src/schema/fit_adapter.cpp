#include "zscan/schema/fit_adapter.hpp"

#include <exception>
#include <numeric>

namespace zscan::schema {

Result<zfit::FitRequest> make_fit_request(const PreparedWorkup& workup)
{
    try {
        return zfit::make_fit_request(
            workup.curves, workup.energies, workup.darks, workup.cuvettes, workup.calibration);
    } catch (const std::exception& error) {
        return std::unexpected(
            SchemaError { ErrorCode::InvalidValue, error.what(), { }, { }, "make_fit_request" });
    }
}

Result<binary::DerivedArrayFile> make_prepared_arrays(
    const PreparedWorkup& workup, const zfit::FitRequest& request, binary::Header header)
{
    if (header.kind != binary::FileKind::DerivedArrays
        || workup.curves.size() != request.curves.size()
        || workup.curves.size() != workup.energies.size()
        || workup.curves.size() != workup.sample_retained_attempt_indexes.size()
        || workup.curves.size() != workup.blank_retained_attempt_indexes.size())
        return std::unexpected(SchemaError { ErrorCode::InvalidValue,
            "prepared array inputs are misaligned", { }, { }, "prepared_arrays" });
    binary::DerivedArrayFile file { header };
    const auto retained = [](binary::ArrayRole role, std::uint32_t curve,
                              const auto& positions) -> Result<binary::DenseArray> {
        if (positions.empty())
            return std::unexpected(SchemaError { ErrorCode::InvalidValue,
                "retained reading identity is empty", { }, { }, "retained_attempt_indexes" });
        const auto columns = positions.front().size();
        binary::DenseArray array { role, curve, positions.size(), columns, { } };
        for (const auto& point : positions) {
            if (point.size() != columns)
                return std::unexpected(SchemaError { ErrorCode::InvalidValue,
                    "retained count varies within scan", { }, { }, "retained_attempt_indexes" });
            for (auto index : point)
                array.values_row_major.push_back(index);
        }
        return array;
    };
    for (std::size_t i = 0; i < workup.curves.size(); ++i) {
        const auto& curve = workup.curves[i];
        file.arrays.push_back({ binary::ArrayRole::ScanPositions, static_cast<std::uint32_t>(i), 1,
            curve.scan_positions_m.size(), curve.scan_positions_m });
        file.arrays.push_back({ binary::ArrayRole::Transmission, static_cast<std::uint32_t>(i), 1,
            curve.transmission.size(), curve.transmission });
        file.arrays.push_back(
            { binary::ArrayRole::TransmissionStandardErrors, static_cast<std::uint32_t>(i), 1,
                curve.transmission_standard_errors.size(), curve.transmission_standard_errors });
        std::vector<double> sensitivities;
        for (const auto& value : curve.dark_sensitivities) {
            sensitivities.push_back(value[0]);
            sensitivities.push_back(value[1]);
        }
        file.arrays.push_back({ binary::ArrayRole::DarkSensitivities, static_cast<std::uint32_t>(i),
            curve.dark_sensitivities.size(), 2, std::move(sensitivities) });
        auto sample = retained(binary::ArrayRole::SampleRetainedAttemptIndexes,
            static_cast<std::uint32_t>(i), workup.sample_retained_attempt_indexes[i]);
        if (!sample)
            return std::unexpected(sample.error());
        file.arrays.push_back(std::move(*sample));
        auto blank = retained(binary::ArrayRole::BlankRetainedAttemptIndexes,
            static_cast<std::uint32_t>(i), workup.blank_retained_attempt_indexes[i]);
        if (!blank)
            return std::unexpected(blank.error());
        file.arrays.push_back(std::move(*blank));
        file.arrays.push_back({ binary::ArrayRole::MeterMeanSem, static_cast<std::uint32_t>(i), 1,
            2, { workup.energies[i].mean_j, workup.energies[i].standard_error_j } });
    }
    file.arrays.push_back(
        { binary::ArrayRole::KappaValueUncertainty, binary::global_array_index, 1, 2,
            { workup.calibration.entrance_chain_transmission,
                workup.calibration.entrance_chain_standard_error } });
    const auto total = std::accumulate(request.curves.begin(), request.curves.end(),
        std::size_t { }, [](std::size_t value, const zfit::EnergyCurve& curve) {
            return value + curve.scan_positions_m.size();
        });
    file.arrays.push_back({ binary::ArrayRole::JointMeasurementCovariance,
        binary::global_array_index, total, total, request.joint_measurement_covariance_row_major });
    if (auto valid = binary::validate_arrays(file); !valid)
        return std::unexpected(valid.error());
    return file;
}

Result<binary::DerivedArrayFile> make_fit_result_arrays(
    const zfit::FitResult& result, binary::Header header)
{
    if (header.kind != binary::FileKind::DerivedArrays)
        return std::unexpected(SchemaError { ErrorCode::InvalidValue,
            "fit arrays require derived-array header", { }, { }, "fit_arrays" });
    binary::DerivedArrayFile file { header };
    for (std::size_t i = 0; i < result.curves.size(); ++i)
        file.arrays.push_back({ binary::ArrayRole::PredictedTransmission,
            static_cast<std::uint32_t>(i), 1, result.curves[i].predicted_transmission.size(),
            result.curves[i].predicted_transmission });
    if (result.uncertainty) {
        const auto n = result.uncertainty->parameter_order.size();
        if (!result.uncertainty->covariance_row_major.empty())
            file.arrays.push_back({ binary::ArrayRole::ConditionalParameterCovariance,
                binary::global_array_index, n, n, result.uncertainty->covariance_row_major });
        if (!result.uncertainty->total_covariance_row_major.empty())
            file.arrays.push_back({ binary::ArrayRole::TotalParameterCovariance,
                binary::global_array_index, n, n, result.uncertainty->total_covariance_row_major });
        if (!result.uncertainty->calibration_variance_contributions.empty()) {
            std::vector<double> contributions;
            for (const auto& row : result.uncertainty->calibration_variance_contributions)
                contributions.insert(contributions.end(), row.begin(), row.end());
            file.arrays.push_back(
                { binary::ArrayRole::CalibrationVarianceContributions, binary::global_array_index,
                    result.uncertainty->calibration_variance_contributions.size(),
                    zfit::FitUncertainty::calibration_group_count, std::move(contributions) });
        }
    }
    if (auto valid = binary::validate_arrays(file); !valid)
        return std::unexpected(valid.error());
    return file;
}

} // namespace zscan::schema
