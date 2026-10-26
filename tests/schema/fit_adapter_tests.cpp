#include "zscan/schema/fit_adapter.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;

PreparedWorkup fixture()
{
    PreparedWorkup workup;
    workup.curves.push_back({
        .session_id = "session-1",
        .sample_id = "sample-1",
        .sample_acquisition_id = "sample-scan",
        .blank_acquisition_id = "blank-scan",
        .dark_record_id = "dark-1",
        .cuvette_calibration_id = "pair-1",
        .scan_positions_m = { -0.001, 0.001 },
        .transmission = { 0.8, 0.9 },
        .transmission_standard_errors = { 0.01, 0.01 },
        .dark_sensitivities = { { 0.1, -0.2 }, { 0.1, -0.2 } },
        .dark_drift_checked = true,
        .measured_pulse_energy_j = 51e-9,
    });
    workup.energies.push_back({ 51e-9, 1e-9, 2, "burst-1" });
    workup.darks.push_back(
        { "dark-1", "session-1", "scan-config-1", { }, 0.01, 0.02, 1e-6, 1e-6, 0.0 });
    workup.cuvettes.push_back(
        { "pair-1", "sample-cuvette", "blank-cuvette", "scan-config-1", 1.05, 0.03 });
    workup.calibration = {
        .laser_characterization_id = "laser-1",
        .wavelength_m = 532e-9,
        .waist_m = 14e-6,
        .waist_standard_error_m = 1e-7,
        .pulse_fwhm_s = 9e-9,
        .m2 = 1.2,
        .m2_standard_error = 0.03,
        .energy_meter_calibration_id = "meter-1",
        .energy_meter_standard_error_j = 1e-9,
        .entrance_chain_calibration_id = "window+solvent",
        .entrance_chain_transmission = 0.95,
        .entrance_chain_standard_error = 0.005,
        .concentration_mol_per_l = 1e-5,
        .concentration_standard_error_mol_per_l = 1e-7,
        .path_length_m = 0.001,
        .path_length_standard_error_m = 1e-5,
    };
    workup.sample_retained_attempt_indexes = { { { 0, 2 }, { 0, 1 } } };
    workup.blank_retained_attempt_indexes = { { { 0, 1 }, { 0, 1 } } };
    return workup;
}

TEST_CASE("prepared arrays persist workup values, retained identities and exact covariance")
{
    const auto workup = fixture();
    const auto request = *make_fit_request(workup);
    binary::Header header { binary::FileKind::DerivedArrays,
        *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T15:00:00Z") };
    const auto arrays = make_prepared_arrays(workup, request, header);
    REQUIRE(arrays);
    const auto bytes = binary::encode_arrays(*arrays);
    REQUIRE(bytes);
    const auto replay = binary::decode_arrays(*bytes);
    REQUIRE(replay);
    const auto covariance
        = std::find_if(replay->arrays.begin(), replay->arrays.end(), [](const auto& array) {
              return array.role == binary::ArrayRole::JointMeasurementCovariance;
          });
    REQUIRE(covariance != replay->arrays.end());
    REQUIRE(covariance->values_row_major == request.joint_measurement_covariance_row_major);
    const auto retained
        = std::find_if(replay->arrays.begin(), replay->arrays.end(), [](const auto& array) {
              return array.role == binary::ArrayRole::SampleRetainedAttemptIndexes;
          });
    REQUIRE(retained->values_row_major == std::vector<double> { 0, 2, 0, 1 });
}
}

TEST_CASE("schema workup values build the production joint fit request")
{
    const auto request = make_fit_request(fixture());
    REQUIRE(request);
    REQUIRE(request->curves.size() == 1);
    REQUIRE(request->curves[0].scan_positions_m == std::vector<double> { -0.001, 0.001 });
    REQUIRE(request->curves[0].observed_transmission == std::vector<double> { 0.8, 0.9 });
    REQUIRE(request->curves[0].laser.pulse_energy_j == Catch::Approx(0.95 * 51e-9));
    REQUIRE(request->joint_measurement_covariance_row_major.size() == 4);
    REQUIRE(request->calibration.pulse_energy_stds_j[0] == Catch::Approx(0.95e-9));
    REQUIRE(request->calibration.entrance_chain_std == Catch::Approx(0.005 / 0.95));
    REQUIRE(request->fitted_parameters.empty());
}

TEST_CASE("fit bridge errors remain structured at schema boundary")
{
    auto workup = fixture();
    workup.calibration.energy_meter_standard_error_j = 0;
    const auto result = make_fit_request(workup);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().location == "make_fit_request");
}

TEST_CASE("fit result arrays retain predictions, covariances and eight variance groups")
{
    zfit::FitResult result;
    result.curves = { { { 0.8, 0.9 } } };
    zfit::FitUncertainty uncertainty;
    uncertainty.parameter_order = { zfit::FitParameter::Sigma_G };
    uncertainty.covariance_row_major = { 1e-38 };
    uncertainty.total_covariance_row_major = { 2e-38 };
    uncertainty.calibration_variance_contributions = { { 1., 2., 3., 4., 5., 6., 7., 8. } };
    result.uncertainty = uncertainty;
    binary::Header header { binary::FileKind::DerivedArrays,
        *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T15:00:00Z") };
    const auto arrays = make_fit_result_arrays(result, header);
    REQUIRE(arrays);
    const auto replay = binary::decode_arrays(*binary::encode_arrays(*arrays));
    REQUIRE(replay);
    REQUIRE(replay->arrays.size() == 4);
    REQUIRE(replay->arrays.back().columns == 8);
    REQUIRE(replay->arrays.back().values_row_major[7] == 8.);
}
