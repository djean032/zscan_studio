#include "zscan/schema/toml_io.hpp"

#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::toml_io;

std::string example(std::string_view name)
{
    std::ifstream input(std::string(ZSCAN_SOURCE_DIR) + "/examples/schema/" + std::string(name));
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}
}

TEST_CASE("published schema examples parse as complete v1 documents")
{
    REQUIRE(parse_sample(example("sample.toml")));
    const auto instrument = parse_instrument(example("instrument.toml"));
    const auto diagnostic = instrument.has_value()
        ? std::string("ok")
        : instrument.error().location + ": " + instrument.error().message;
    INFO(diagnostic);
    REQUIRE(instrument.has_value());
    REQUIRE(parse_session(example("session.toml")));
    REQUIRE(parse_fit_request(example("fit_request.toml")));
    REQUIRE(parse_fit_result(example("fit_result.toml")));
    REQUIRE(parse_run(example("run.toml")));
}

TEST_CASE("sample snapshot TOML round-trips explicit uncertainties and conversion sources")
{
    const auto source = R"(
schema_version = 1
sample_id = "sample-1"
solvent_id = "solvent-1"
created_at = "2026-10-25T14:03:00Z"

[concentration_mol_per_l]
value = 1e-5
standard_uncertainty = 2e-7
provenance_id = "prep-report"
uncertainty_evaluation_id = "prep-evaluation"

[solvent_refractive_index]
value = 1.33
standard_uncertainty = 0.00005773502691896258
provenance_id = "supplier-datasheet"
uncertainty_evaluation_id = "index-evaluation"

[solvent_refractive_index.conversion]
method = "rectangular_half_width"
source_value = 0.0001
divisor = 1.7320508075688772
provenance_id = "supplier-datasheet"
)";
    auto sample = parse_sample(source);
    REQUIRE(sample);
    REQUIRE(sample->sample_id == "sample-1");
    REQUIRE(sample->solvent_refractive_index.conversion);
    REQUIRE(validate(*sample));
    auto text = format_sample(*sample);
    REQUIRE(text);
    REQUIRE(parse_sample(*text)->solvent_refractive_index.conversion->method
        == "rectangular_half_width");
    REQUIRE(parse_sample(*text)->solvent_refractive_index.standard_uncertainty
        == sample->solvent_refractive_index.standard_uncertainty);
    const auto extended = std::string(source) + "\n[extensions.operator_notes]\ncode = 42\n";
    auto extended_sample = parse_sample(extended);
    REQUIRE(extended_sample);
    REQUIRE(format_sample(*extended_sample));
    REQUIRE(parse_sample(*format_sample(*extended_sample))->extensions_toml
        == extended_sample->extensions_toml);
    auto contradiction = std::string(source);
    const auto bound = contradiction.find("source_value = 0.0001");
    REQUIRE(bound != std::string::npos);
    contradiction.replace(
        bound, std::string("source_value = 0.0001").size(), "source_value = 0.01");
    REQUIRE_FALSE(parse_sample(contradiction));
}

TEST_CASE("instrument snapshot TOML round-trips dedicated pair and split configurations")
{
    InstrumentSnapshot instrument;
    instrument.snapshot_id = *Uuid::parse("550e8400-e29b-41d4-a716-446655440000");
    instrument.created_at = *UtcTimestamp::parse("2026-10-25T14:03:00Z");
    instrument.source_catalog_id = "lab-1";
    instrument.source_catalog_revision = "2026-oct";
    instrument.laser_characterization_id = "laser-1";
    instrument.wavelength_m = 532e-9;
    instrument.pulse_fwhm_s = 9e-9;
    const auto scalar = [](double value, double uncertainty) {
        return ScalarWithUncertainty { value, uncertainty, "characterization-1", "evaluation-1" };
    };
    instrument.waist_m = scalar(14.5e-6, 0.7e-6);
    instrument.m2 = scalar(1.2, 0.03);
    instrument.energy_meter_calibration_id = "meter-1";
    instrument.energy_meter_standard_uncertainty_j = 1.5e-9;
    instrument.energy_meter_uncertainty_provenance_id = "meter-report";
    instrument.entrance_window_refractive_index = scalar(1.46, 0.005);
    instrument.cuvettes
        = { { "sample-cuvette", scalar(0.001, 1e-5) }, { "blank-cuvette", scalar(0.001, 1e-5) } };
    instrument.cuvette_pairs = { { "pair-cal-1", "sample-cuvette", "blank-cuvette", "scan-config-1",
        scalar(1.05, 0.03) } };
    DaqConfiguration daq;
    daq.configuration_id = "daq-config-1";
    daq.device_id = "ni-1";
    daq.channels = { DaqChannel { 0, DetectorRole::Reference, TerminalConfiguration::Rse,
                         Coupling::Dc, -0.1, 10.0 },
        DaqChannel {
            1, DetectorRole::Signal, TerminalConfiguration::Rse, Coupling::Dc, -0.1, 10.0 } };
    instrument.daq_configurations.push_back(daq);
    instrument.optical_filters = { { "od-reference", 1.0, 532e-9, "filter-report" } };
    instrument.optical_configurations = { { "optical-config-1", { "od-reference" }, { } } };
    instrument.stage_configurations
        = { { "stage-config-1", "esp302-1", 1, 1e-6, "stage-spec", 2 } };
    instrument.scan_configurations
        = { { "scan-config-1", "daq-config-1", "optical-config-1", "stage-config-1" } };
    const auto text = format_instrument(instrument);
    REQUIRE(text);
    const auto parsed = parse_instrument(*text);
    REQUIRE(parsed);
    REQUIRE(parsed->cuvette_pairs[0].sample_cuvette_id == "sample-cuvette");
    REQUIRE(parsed->daq_configurations[0].channels[1].role == DetectorRole::Signal);
    REQUIRE(parsed->optical_configurations[0].reference_filter_ids[0] == "od-reference");
    REQUIRE(validate(*parsed));
    REQUIRE(*format_instrument(*parsed) == *text);
    auto modified = *text + "\nextra_unexpected = true\n";
    REQUIRE_FALSE(parse_instrument(modified));
    instrument.energy_meter_standard_uncertainty_j = 0;
    const auto draft = format_instrument(instrument);
    REQUIRE(draft);
    REQUIRE(validate(*parse_instrument(*draft), ValidationLevel::Draft));
    REQUIRE_FALSE(validate(*parse_instrument(*draft)));
}

TEST_CASE(
    "session TOML preserves explicit ordered curve mappings and leaves dark selection to workup")
{
    const auto id = [](std::string_view value) { return *Uuid::parse(value); };
    const auto file = [](std::string_view name) {
        return FileIdentity { *SessionPath::parse(name), 10,
            *Sha256Digest::parse(std::string(64, 'a')) };
    };
    const auto time = *UtcTimestamp::parse("2026-10-25T14:03:00Z");
    SessionManifest session { id("550e8400-e29b-41d4-a716-446655440000"), time, 1,
        file("sample.toml"), file("instrument.toml") };
    const auto capture = CaptureSettings { 1e6, 10, 140, 1, 1, 2, 1 };
    const auto blank = id("33e47923-4466-4ec8-b957-3e4c39ad4516");
    const auto sample = id("171e8400-e29b-41d4-a716-446655440000");
    const auto dark = id("22e47923-4466-4ec8-b957-3e4c39ad4516");
    const auto burst = id("441e8400-e29b-41d4-a716-446655440000");
    const auto grid = ScanGrid { -0.001, 0.001, 2, 0.002 };
    session.scans.push_back(
        { blank, ScanKind::Blank, AcquisitionState::Complete, file("scans/blank.bin"), time, "",
            "solvent-1", "blank-cuvette", "scan-config-1", grid, capture });
    session.scans.push_back(
        { sample, ScanKind::Sample, AcquisitionState::Complete, file("scans/sample.bin"), time,
            "sample-1", "solvent-1", "sample-cuvette", "scan-config-1", grid, capture });
    session.darks.push_back({ dark, AcquisitionState::Complete, file("darks/dark.bin"), time,
        "daq-config-1", { true, true, true, true }, capture });
    session.energy_bursts.push_back({ burst, { }, file("energy/burst.bin"), time, 2, "meter-1" });
    session.curves.push_back(
        { id("771e8400-e29b-41d4-a716-446655440000"), sample, blank, burst, "pair-cal-1" });
    const auto formatted = format_session(session);
    REQUIRE(formatted);
    REQUIRE(formatted->find("dark_record_id") == std::string::npos);
    const auto restored = parse_session(*formatted);
    REQUIRE(restored);
    REQUIRE(restored->scans[0].kind == ScanKind::Blank);
    REQUIRE(restored->scans[0].sample_id.empty());
    REQUIRE(restored->curves[0].energy_burst_id == burst);
    REQUIRE(restored->darks[0].daq_configuration_id == "daq-config-1");
    REQUIRE(*format_session(*restored) == *formatted);
    auto bad = *formatted + "\nunknown = 42\n";
    REQUIRE_FALSE(parse_session(bad));
    bad = *formatted;
    const auto position = bad.find("daq_configuration_id");
    REQUIRE(position != std::string::npos);
    bad.insert(position, "optical_configuration_id = \"wrong\"\n");
    REQUIRE_FALSE(parse_session(bad)); // A dark cannot claim an OD/optical config.
    bad = *formatted;
    const auto count = bad.find("manifest_revision = 1");
    REQUIRE(count != std::string::npos);
    bad.replace(count, std::string("manifest_revision = 1").size(), "manifest_revision = -1");
    REQUIRE_FALSE(parse_session(bad));
}
