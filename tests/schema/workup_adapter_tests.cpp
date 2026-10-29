#include "zscan/schema/workup_adapter.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::binary;

Uuid id(std::string_view value) { return *Uuid::parse(value); }
UtcTimestamp at(int second)
{
    return *UtcTimestamp::parse(
        std::string("2026-10-25T14:00:") + (second < 10 ? "0" : "") + std::to_string(second) + "Z");
}
FileIdentity file(std::string_view path)
{
    return { *SessionPath::parse(path), 1, *Sha256Digest::parse(std::string(64, 'a')) };
}
ScalarWithUncertainty scalar(double value, double uncertainty)
{
    return { value, uncertainty, "report", "evaluation" };
}

ValidatedSession fixture()
{
    const auto session_id = id("550e8400-e29b-41d4-a716-446655440000");
    const auto blank_id = id("33e47923-4466-4ec8-b957-3e4c39ad4516");
    const auto sample_id = id("771e8400-e29b-41d4-a716-446655440000");
    const auto before_id = id("881e8400-e29b-41d4-a716-446655440000");
    const auto after_id = id("991e8400-e29b-41d4-a716-446655440000");
    const auto burst_id = id("441e8400-e29b-41d4-a716-446655440000");
    const auto capture = CaptureSettings { 1e6, 10, 140, 1, 1, 2, 0 };
    const auto grid = ScanGrid { -0.001, 0.001, 2, 0.002 };
    SessionManifest manifest { session_id, at(0), 1, file("sample.toml"), file("instrument.toml") };
    manifest.scans.push_back(
        { blank_id, ScanKind::Blank, AcquisitionState::Complete, file("scans/blank.bin"), at(20),
            "", "solvent-1", "blank-cuvette", "scan-1", grid, capture });
    manifest.scans.push_back(
        { sample_id, ScanKind::Sample, AcquisitionState::Complete, file("scans/sample.bin"), at(22),
            "sample-1", "solvent-1", "sample-cuvette", "scan-1", grid, capture });
    manifest.darks.push_back({ before_id, AcquisitionState::Complete, file("darks/before.bin"),
        at(10), "daq-1", { true, true, true, true }, capture });
    manifest.darks.push_back({ after_id, AcquisitionState::Complete, file("darks/after.bin"),
        at(24), "daq-1", { true, true, true, true }, capture });
    manifest.energy_bursts.push_back(
        { burst_id, { }, file("energy/burst.bin"), at(5), 2, "meter-1" });
    manifest.curves.push_back(
        { id("aa1e8400-e29b-41d4-a716-446655440000"), sample_id, blank_id, burst_id, "pair-1" });
    SampleSnapshot sample { "sample-1", "solvent-1", scalar(1e-5, 1e-7), scalar(1.33, 0.001),
        at(0) };
    InstrumentSnapshot instrument;
    instrument.laser_characterization_id = "laser-1";
    instrument.energy_meter_calibration_id = "meter-1";
    instrument.wavelength_m = 532e-9;
    instrument.pulse_fwhm_s = 9e-9;
    instrument.waist_m = scalar(14e-6, 1e-7);
    instrument.m2 = scalar(1.2, 0.03);
    instrument.energy_meter_standard_uncertainty_j = 1e-9;
    instrument.entrance_window_refractive_index = scalar(1.46, 0.005);
    instrument.cuvettes = { { "sample-cuvette", scalar(0.001, 1e-5) } };
    instrument.cuvette_pairs
        = { { "pair-1", "sample-cuvette", "blank-cuvette", "scan-1", scalar(1.05, 0.03) } };
    instrument.scan_configurations = { { "scan-1", "daq-1", "optical-1", "stage-1" } };
    auto scan = [&](Uuid uuid, ScanKind kind, int second, double ai1) {
        ScanFile binary;
        binary.header = { FileKind::Scan, uuid, session_id, at(second) };
        binary.config = { kind, 2, grid, capture, 1e-6, -0.1, 10., -0.1, 10., { -0.001, 0.001 } };
        binary.positions
            = { { 0, -0.001, { { 0, MoveResult::WithinTolerance, -0.001, -0.001 } },
                    { AcceptedShot { 0, 2., ai1 }, AcceptedShot { 1, 2.1, ai1 * 1.02 } },
                    PositionResult::Complete },
                  { 1, 0.001, { { 0, MoveResult::WithinTolerance, 0.001, 0.001 } },
                      { AcceptedShot { 0, 2., ai1 }, AcceptedShot { 1, 2.1, ai1 * 1.02 } },
                      PositionResult::Complete } };
        binary.end = { ScanResult::Complete, TerminationReason::NormalCompletion, 2, 2, 4, 0 };
        return binary;
    };
    auto dark = [&](Uuid uuid, int second) {
        DarkFile binary;
        binary.header = { FileKind::Dark, uuid, session_id, at(second) };
        binary.config = { capture, -0.1, 10., -0.1, 10. };
        binary.attempts = { AcceptedShot { 0, 0.01, 0.02 }, AcceptedShot { 1, 0.01, 0.02 } };
        binary.end = { DarkResult::Complete, DarkTerminationReason::NormalCompletion, 2, 0 };
        return binary;
    };
    ValidatedSession result { std::move(manifest), std::move(sample), std::move(instrument) };
    result.scans
        = { scan(blank_id, ScanKind::Blank, 20, 1.6), scan(sample_id, ScanKind::Sample, 22, 1.3) };
    result.darks = { dark(before_id, 10), dark(after_id, 24) };
    result.energy_bursts
        = { { { FileKind::Energy, burst_id, session_id, at(5) }, { 50e-9, 52e-9 } } };
    result.curves = { { 1, 0, 0, 1, 0 } };
    return result;
}
}

TEST_CASE("headless workup adapter preserves dark midpoint and constructs absolute curve")
{
    const auto session = fixture();
    const auto result = prepare_workup(session, { 0.05, 0.05 });
    REQUIRE(result);
    REQUIRE(result->curves.size() == 1);
    REQUIRE(result->curves[0].dark_record_id == session.manifest.darks[1].record_id.to_string());
    REQUIRE(result->curves[0].scan_positions_m == std::vector<double> { -0.001, 0.001 });
    REQUIRE(result->curves[0].transmission[0] > 0);
    REQUIRE(result->curves[0].transmission[0] < 1);
    REQUIRE(result->energies[0].provenance_id
        == session.manifest.energy_bursts[0].burst_id.to_string());
    REQUIRE(result->calibration.path_length_m == 0.001);
    REQUIRE(result->calibration.entrance_chain_transmission > 0);
    REQUIRE(result->calibration.energy_meter_standard_error_j == 1e-9);
}

TEST_CASE("workup converts fractional dark timestamps to the system clock resolution")
{
    auto session = fixture();
    session.manifest.darks[1].acquired_at.nanosecond = 123'456'789;
    const auto prepared = prepare_workup(session, { 0.05, 0.05 });
    REQUIRE(prepared);
    const auto expected = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                              session.manifest.darks[1].acquired_at.seconds)
        + std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds { 123'456'789 });
    REQUIRE(prepared->darks[0].acquired_at == expected);
}
