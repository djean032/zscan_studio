#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "zscan/schema/types.hpp"

namespace {
using namespace zscan::schema;

template <typename T> void rejects(std::string_view text, ErrorCode expected)
{
    const auto result = T::parse(text);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == expected);
    REQUIRE_FALSE(result.error().message.empty());
}

Uuid uuid(std::string_view text = "550e8400-e29b-41d4-a716-446655440000")
{
    return *Uuid::parse(text);
}

UtcTimestamp timestamp() { return *UtcTimestamp::parse("2026-10-25T14:03:00Z"); }

FileIdentity file(std::string_view path, bool sealed = true)
{
    return { *SessionPath::parse(path), 100,
        sealed ? std::optional { *Sha256Digest::parse(std::string(64, 'a')) } : std::nullopt };
}

CaptureSettings capture() { return { 1e6, 10, 140, 1, 1, 100, 25 }; }

ScalarWithUncertainty scalar(double value = 1.0, double uncertainty = 0.01)
{
    return { value, uncertainty, "characterization-1", "evaluation-1" };
}
} // namespace

TEST_CASE("canonical UUID v4 round trips as wire bytes")
{
    const auto value = Uuid::parse("550e8400-e29b-41d4-a716-446655440000");
    REQUIRE(value);
    REQUIRE(value->to_string() == "550e8400-e29b-41d4-a716-446655440000");
    REQUIRE(value->bytes[6] == 0x41);
    REQUIRE(value->bytes[8] == 0xa7);
    REQUIRE(*Uuid::parse(value->to_string()) == *value);
    for (std::string_view invalid : { "", "550e8400-e29b-11d4-a716-446655440000",
             "550e8400-e29b-41d4-7716-446655440000", "550E8400-e29b-41d4-a716-446655440000",
             "550e8400-e29b-41d4-a716-44665544000g", "550e8400-e29b-41d4-a716-44665544000-" }) {
        rejects<Uuid>(invalid, ErrorCode::InvalidUuid);
    }
}

TEST_CASE("UTC timestamps round trip and reject invalid dates and precision")
{
    for (std::string_view text : { "2026-10-25T14:03:00Z", "2026-02-28T23:59:59.123456789Z",
             "2000-02-29T00:00:00.01Z", "9999-12-31T23:59:59Z" }) {
        const auto parsed = UtcTimestamp::parse(text);
        REQUIRE(parsed);
        REQUIRE(parsed->to_string() == text);
        REQUIRE(*UtcTimestamp::parse(parsed->to_string()) == *parsed);
    }
    REQUIRE(
        UtcTimestamp::parse("2026-10-25T14:03:00.1000Z")->to_string() == "2026-10-25T14:03:00.1Z");
    for (std::string_view invalid :
        { "2026-02-29T00:00:00Z", "0000-01-01T00:00:00Z", "2026-13-01T00:00:00Z",
            "2026-10-25T24:00:00Z", "2026-10-25T00:00:60Z", "2026-10-25T14:03:00+00:00",
            "2026-10-25T14:03:00.Z", "2026-10-25T14:03:00.1234567890Z" }) {
        rejects<UtcTimestamp>(invalid, ErrorCode::InvalidTimestamp);
    }
}

TEST_CASE("SHA-256 digest accepts only lowercase hex")
{
    const std::string hex(64, 'a');
    const auto digest = Sha256Digest::parse(hex);
    REQUIRE(digest);
    REQUIRE(digest->to_string() == hex);
    REQUIRE(digest->bytes[0] == 0xaa);
    rejects<Sha256Digest>(std::string(63, 'a'), ErrorCode::InvalidDigest);
    rejects<Sha256Digest>(std::string(64, 'A'), ErrorCode::InvalidDigest);
    rejects<Sha256Digest>(std::string(64, 'g'), ErrorCode::InvalidDigest);
}

TEST_CASE("session paths refuse traversal and unsafe spellings")
{
    const auto path = SessionPath::parse("scans/scan.bin");
    REQUIRE(path);
    REQUIRE(path->to_string() == "scans/scan.bin");
    for (std::string_view invalid : { "", "/etc/passwd", "../outside", "scans/../outside",
             "scans/./scan.bin", "scans//scan.bin", "scans/", "C:/escape", "scans\\x.bin" }) {
        rejects<SessionPath>(invalid, ErrorCode::UnsafePath);
    }
    rejects<SessionPath>(std::string_view("safe\0unsafe", 11), ErrorCode::UnsafePath);
}

TEST_CASE("session path resolution rejects symlink escapes and permits internal paths")
{
    const auto root = std::filesystem::temp_directory_path()
        / ("zscan_schema_path_test_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto outside = root.parent_path() / (root.filename().string() + "_outside");
    std::filesystem::create_directory(root);
    std::filesystem::create_directory(outside);
    struct Cleanup {
        std::filesystem::path root, outside;
        ~Cleanup()
        {
            std::filesystem::remove_all(root);
            std::filesystem::remove_all(outside);
        }
    } cleanup { root, outside };
    std::filesystem::create_directory(root / "scans");
    std::filesystem::create_directory_symlink(outside, root / "escape");
    std::filesystem::create_directory_symlink(root / "scans", root / "inside");

    REQUIRE(SessionPath::parse("scans/new.bin")->resolve_within(root)
        == std::filesystem::canonical(root) / "scans/new.bin");
    REQUIRE(SessionPath::parse("inside/new.bin")->resolve_within(root)
        == std::filesystem::canonical(root) / "scans/new.bin");
    const auto escaped = SessionPath::parse("escape/new.bin")->resolve_within(root);
    REQUIRE_FALSE(escaped);
    REQUIRE(escaped.error().code == ErrorCode::UnsafePath);
    REQUIRE_FALSE(SessionPath::parse("scans/new.bin")->resolve_within(outside / "missing"));
}

TEST_CASE("structural numeric validators reject invalid scan and calibration inputs")
{
    ScanGrid grid { -0.01, 0.01, 101, 0.0002 };
    REQUIRE(validate(grid));
    grid.spacing_m = -0.0002;
    REQUIRE_FALSE(validate(grid));
    grid.spacing_m = 0.0002;
    grid.position_count = 1;
    REQUIRE_FALSE(validate(grid));
    grid.position_count = 101;
    grid.end_z_m = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(validate(grid));
    REQUIRE_FALSE(validate(ScanGrid { -1e-16, 1e-16, 2, 1e-15 }));

    CaptureSettings capture { 1e6, 10, 140, 1, 1, 100, 25 };
    REQUIRE(validate(capture));
    capture.sample_rate_hz = 0;
    REQUIRE_FALSE(validate(capture));

    ScalarWithUncertainty scalar { 1.0, 0.01, "report-1", "analysis-1" };
    REQUIRE(validate(scalar));
    scalar.standard_uncertainty = 0;
    REQUIRE_FALSE(validate(scalar));
    scalar.standard_uncertainty = 0.01;
    scalar.value = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(validate(scalar));
}

TEST_CASE("electronic configuration is distinct from optical and stage settings")
{
    DaqConfiguration daq;
    daq.configuration_id = "daq-1";
    daq.device_id = "ni-1";
    daq.channels = { DaqChannel { 0, DetectorRole::Reference, TerminalConfiguration::Rse,
                         Coupling::Dc, -0.1, 10.0 },
        DaqChannel {
            1, DetectorRole::Signal, TerminalConfiguration::Rse, Coupling::Dc, -0.1, 10.0 } };
    REQUIRE(validate(daq));
    daq.channels[1].role = DetectorRole::Reference;
    REQUIRE_FALSE(validate(daq));
    daq.channels[1].role = DetectorRole::Signal;
    daq.channels[1].minimum_v = 11.0;
    REQUIRE_FALSE(validate(daq));

    StageConfiguration stage { "stage-1", "esp302-1", 1, 1e-6, "stage-spec-1", 2 };
    REQUIRE(validate(stage));
    stage.maximum_readback_error_m = 0;
    REQUIRE_FALSE(validate(stage));
}

TEST_CASE("scan and dark record validation reports ID and nested field")
{
    ScanRecord scan { uuid(), ScanKind::Sample, AcquisitionState::Complete, file("scans/scan.bin"),
        timestamp(), "sample-1", "solvent-1", "cuvette-1", "scan-config-1",
        { -0.01, 0.01, 101, 0.0002 }, capture() };
    REQUIRE(validate(scan));
    scan.grid.spacing_m = 0.1;
    auto diagnostic = validate(scan);
    REQUIRE_FALSE(diagnostic);
    REQUIRE(diagnostic.error().code == ErrorCode::InvalidValue);
    REQUIRE(diagnostic.error().record_id == scan.acquisition_id.to_string());
    REQUIRE(diagnostic.error().location == "grid.spacing_m");
    auto sourced = with_source(diagnostic.error(), "session.toml");
    REQUIRE(sourced.source_path == "session.toml");
    REQUIRE(sourced.record_id == scan.acquisition_id.to_string());
    scan.grid.spacing_m = 0.0002;
    scan.kind = ScanKind::Blank;
    REQUIRE(validate(scan).error().location == "scan.sample_id");
    scan.sample_id.clear();
    REQUIRE(validate(scan));
    scan.file.sha256.reset();
    REQUIRE(validate(scan).error().location == "file.sha256");
    scan.state = AcquisitionState::InProgress;
    REQUIRE(validate(scan));
    scan.kind = static_cast<ScanKind>(99);
    REQUIRE(validate(scan).error().location == "scan.kind");

    DarkRecord dark { uuid(), AcquisitionState::Complete, file("darks/dark.bin"), timestamp(),
        "daq-1", { true, true, true, true }, capture() };
    REQUIRE(validate(dark));
    dark.daq_configuration_id.clear();
    REQUIRE(validate(dark).error().location == "dark.daq_configuration_id");
}

TEST_CASE("energy, curve, and run records validate their local identity")
{
    const auto other = uuid("33e47923-4466-4ec8-b957-3e4c39ad4516");
    EnergyBurstRecord burst { uuid(), { }, file("energy/burst.bin"), timestamp(), 20, "meter-1" };
    REQUIRE(validate(burst));
    burst.revision_of = burst.burst_id;
    REQUIRE(validate(burst).error().location == "energy_burst.revision_of");
    burst.revision_of = other;
    burst.reading_count = 1;
    REQUIRE(validate(burst).error().location == "energy_burst.reading_count");
    burst.reading_count = 20;
    REQUIRE(validate(burst));

    CurveMapping curve { uuid(), other, uuid(), burst.burst_id, "pair-cal-1" };
    REQUIRE(validate(curve));
    curve.blank_acquisition_id = other;
    REQUIRE(validate(curve).error().location == "curve.blank_acquisition_id");

    DerivedRunReference run { uuid(), file("derived/run/run.toml"), timestamp() };
    REQUIRE(validate(run));
    run.run_manifest.sha256.reset();
    REQUIRE(validate(run).error().location == "run_manifest.sha256");
}

TEST_CASE("sample and instrument drafts can omit uncertainty but cannot support production")
{
    SampleSnapshot sample { "sample-1", "solvent-1", scalar(0.001), scalar(1.33) };
    REQUIRE(validate(sample));
    sample.solvent_refractive_index.standard_uncertainty = 0;
    REQUIRE(validate(sample, ValidationLevel::Draft));
    const auto production_error = validate(sample);
    REQUIRE_FALSE(production_error);
    REQUIRE(production_error.error().location == "solvent_refractive_index.standard_uncertainty");
    sample.solvent_refractive_index.standard_uncertainty = 0.001;

    InstrumentSnapshot instrument;
    instrument.snapshot_id = uuid();
    instrument.source_catalog_id = "catalog-1";
    instrument.laser_characterization_id = "laser-char-1";
    instrument.energy_meter_calibration_id = "meter-1";
    instrument.wavelength_m = 532e-9;
    instrument.pulse_fwhm_s = 7e-9;
    instrument.waist_m = scalar(1e-5, 1e-7);
    instrument.m2 = scalar(1.2, 0.01);
    instrument.entrance_window_refractive_index = scalar(1.46, 0.001);
    instrument.cuvettes.push_back({ "cuvette-s", scalar(0.001, 1e-6) });
    instrument.cuvette_pairs.push_back(
        { "pair-1", "cuvette-s", "cuvette-b", "scan-1", scalar(1.01, 0.01) });
    instrument.energy_meter_standard_uncertainty_j = 0;
    REQUIRE(validate(instrument, ValidationLevel::Draft));
    REQUIRE(validate(instrument).error().location == "energy_meter_standard_uncertainty_j");
    instrument.energy_meter_standard_uncertainty_j = 1e-9;
    instrument.energy_meter_uncertainty_provenance_id = "meter-report-1";
    REQUIRE(validate(instrument));
    instrument.cuvette_pairs[0].blank_cuvette_id = "cuvette-s";
    REQUIRE(validate(instrument).error().location == "cuvette_pairs");
}

TEST_CASE("manifest structural errors retain array location and record ID")
{
    SessionManifest session { uuid(), timestamp(), 1, file("sample.toml"),
        file("instrument.toml") };
    REQUIRE(validate(session));
    session.scans.push_back({ uuid(), ScanKind::Sample, AcquisitionState::Complete,
        file("scans/scan.bin"), timestamp(), "sample-1", "solvent-1", "cuvette-1", "scan-config-1",
        { -0.01, 0.01, 101, 0.0002 }, capture() });
    REQUIRE(validate(session));
    session.scans[0].capture.sample_rate_hz = std::numeric_limits<double>::infinity();
    auto result = validate(session);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().location == "scans.capture.sample_rate_hz");
    REQUIRE(result.error().record_id == session.scans[0].acquisition_id.to_string());
    session.scans[0].capture.sample_rate_hz = 1e6;
    session.manifest_revision = 0;
    REQUIRE(validate(session).error().location == "manifest_revision");
}
