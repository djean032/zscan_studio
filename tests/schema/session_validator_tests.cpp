#include "zscan/schema/session_validator.hpp"
#include "zscan/schema/storage.hpp"
#include "zscan/schema/toml_io.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::binary;
using namespace zscan::schema::storage;

struct Fixture {
    std::filesystem::path root;
    SessionManifest manifest;
    Fixture()
        : root("/tmp/opencode/zscan_validator_"
              + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
        , manifest { *random_uuid_v4(), *UtcTimestamp::parse("2026-10-25T14:00:00Z"), 1,
            { *SessionPath::parse("sample.toml"), 0, { } },
            { *SessionPath::parse("instrument.toml"), 0, { } } }
    {
        std::filesystem::create_directories(root / "scans");
        std::filesystem::create_directory(root / "darks");
        std::filesystem::create_directory(root / "energy");
    }
    ~Fixture() { std::filesystem::remove_all(root); }

    FileIdentity save(std::string_view path, std::string_view text)
    {
        auto relative = *SessionPath::parse(path);
        std::ofstream output(root / relative.value(), std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        return { relative, text.size(), *sha256(std::span(data, text.size())) };
    }
    FileIdentity save_binary(std::string_view path, std::span<const std::uint8_t> bytes)
    {
        return save(path, { reinterpret_cast<const char*>(bytes.data()), bytes.size() });
    }
};

ScalarWithUncertainty scalar(double value, double std)
{
    return { value, std, "source-1", "evaluation-1" };
}

UtcTimestamp fixture_time(int second)
{
    return *UtcTimestamp::parse(
        std::string("2026-10-25T14:00:") + (second < 10 ? "0" : "") + std::to_string(second) + "Z");
}

void populate(Fixture& f)
{
    const auto t = fixture_time;
    SampleSnapshot sample { "sample-1", "solvent-1", scalar(1e-5, 1e-7), scalar(1.33, 0.001),
        t(0) };
    f.manifest.sample_snapshot = f.save("sample.toml", *toml_io::format_sample(sample));
    InstrumentSnapshot instrument;
    instrument.snapshot_id = *random_uuid_v4();
    instrument.created_at = t(0);
    instrument.source_catalog_id = "catalog-1";
    instrument.source_catalog_revision = "rev-1";
    instrument.laser_characterization_id = "laser-1";
    instrument.wavelength_m = 532e-9;
    instrument.pulse_fwhm_s = 9e-9;
    instrument.waist_m = scalar(14e-6, 1e-7);
    instrument.m2 = scalar(1.2, 0.03);
    instrument.energy_meter_calibration_id = "meter-1";
    instrument.energy_meter_standard_uncertainty_j = 1e-9;
    instrument.energy_meter_uncertainty_provenance_id = "meter-report";
    instrument.entrance_window_refractive_index = scalar(1.46, 0.005);
    instrument.cuvettes
        = { { "sample-cuvette", scalar(0.001, 1e-5) }, { "blank-cuvette", scalar(0.001, 1e-5) } };
    instrument.cuvette_pairs
        = { { "pair-1", "sample-cuvette", "blank-cuvette", "scan-1", scalar(1.05, 0.03) } };
    DaqConfiguration daq;
    daq.configuration_id = "daq-1";
    daq.device_id = "ni-1";
    daq.channels = { DaqChannel { 0, DetectorRole::Reference, TerminalConfiguration::Rse,
                         Coupling::Dc, -0.1, 10. },
        DaqChannel {
            1, DetectorRole::Signal, TerminalConfiguration::Rse, Coupling::Dc, -0.1, 10. } };
    instrument.daq_configurations = { daq };
    instrument.optical_configurations = { { "optical-1", { }, { } } };
    instrument.stage_configurations = { { "stage-1", "esp302", 1, 1e-6, "stage-spec", 2 } };
    instrument.scan_configurations = { { "scan-1", "daq-1", "optical-1", "stage-1" } };
    f.manifest.instrument_snapshot
        = f.save("instrument.toml", *toml_io::format_instrument(instrument));
    const CaptureSettings capture { 1e6, 10, 140, 1, 1, 2, 1 };
    auto create_scan = [&](ScanKind kind, int second, std::string_view name, std::string cuvette) {
        ScanFile scan;
        scan.header = { FileKind::Scan, *random_uuid_v4(), f.manifest.session_id, t(second) };
        scan.config = { kind, 2, { -0.001, 0.001, 2, 0.002 }, capture, 1e-6, -0.1, 10., -0.1, 10.,
            { -0.001, 0.001 } };
        scan.positions = { { 0, -0.001, { { 0, MoveResult::WithinTolerance, -0.001, -0.001 } },
                               { AcceptedShot { 0, 2., 1.5 }, AcceptedShot { 1, 2.1, 1.6 } },
                               PositionResult::Complete },
            { 1, 0.001, { { 0, MoveResult::WithinTolerance, 0.001, 0.001 } },
                { AcceptedShot { 0, 2., 1.5 }, AcceptedShot { 1, 2.1, 1.6 } },
                PositionResult::Complete } };
        scan.end = { ScanResult::Complete, TerminationReason::NormalCompletion, 2, 2, 4, 0 };
        auto bytes = *encode_scan(scan);
        const std::string path = std::string("scans/") + std::string(name) + ".bin";
        f.manifest.scans.push_back(
            { scan.header.record_id, kind, AcquisitionState::Complete, f.save_binary(path, bytes),
                scan.header.timestamp, kind == ScanKind::Sample ? "sample-1" : "", "solvent-1",
                std::move(cuvette), "scan-1", scan.config.grid, capture });
    };
    create_scan(ScanKind::Blank, 20, "blank", "blank-cuvette");
    create_scan(ScanKind::Sample, 22, "sample", "sample-cuvette");
    auto create_dark = [&](int second, std::string_view name) {
        DarkFile dark;
        dark.header = { FileKind::Dark, *random_uuid_v4(), f.manifest.session_id, t(second) };
        dark.config = { capture, -0.1, 10., -0.1, 10. };
        dark.attempts = { AcceptedShot { 0, 0.01, 0.02 }, AcceptedShot { 1, 0.01, 0.02 } };
        dark.end = { DarkResult::Complete, DarkTerminationReason::NormalCompletion, 2, 0 };
        auto bytes = *encode_dark(dark);
        f.manifest.darks.push_back({ dark.header.record_id, AcquisitionState::Complete,
            f.save_binary(std::string("darks/") + std::string(name) + ".bin", bytes),
            dark.header.timestamp, "daq-1", { true, true, true, true }, capture });
    };
    create_dark(10, "before");
    create_dark(24, "after");
    EnergyFile energy { { FileKind::Energy, *random_uuid_v4(), f.manifest.session_id, t(5) },
        { 50e-9, 52e-9 } };
    auto burst = *encode_energy(energy);
    f.manifest.energy_bursts.push_back({ energy.header.record_id, { },
        f.save_binary("energy/burst.bin", burst), energy.header.timestamp, 2, "meter-1" });
    f.manifest.curves.push_back({ *random_uuid_v4(), f.manifest.scans[1].acquisition_id,
        f.manifest.scans[0].acquisition_id, energy.header.record_id, "pair-1" });
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
}
}

TEST_CASE("session production validation has a dedicated Qt-free entry point")
{
    REQUIRE_FALSE(
        zscan::schema::load_production_session("/tmp/opencode/nonexistent-zscan-session"));
}

TEST_CASE("production session selects closest electronic-compatible dark without age cutoff")
{
    Fixture f;
    populate(f);
    auto validated = load_production_session(f.root);
    REQUIRE(validated);
    REQUIRE(validated->curves.size() == 1);
    REQUIRE(validated->curves[0].selected_dark_index == 1); // dark AFTER the pair
    REQUIRE(validated->warnings.size() == 1);
    REQUIRE(validated->warnings[0].location == "workflow");
    REQUIRE(validated->curves[0].cuvette_pair_index == 0);
    REQUIRE(validated->scans[0].config.commanded_positions_m[0] == -0.001);
    REQUIRE(validated->sample.sample_id == "sample-1");
}

TEST_CASE("production validator refuses role mismatch, wrong electronics and altered input")
{
    Fixture f;
    populate(f);
    f.manifest.scans[1].cuvette_id = "blank-cuvette";
    f.manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    REQUIRE(
        load_production_session(f.root).error().location == "curves.cuvette_pair_calibration_id");
    f.manifest.scans[1].cuvette_id = "sample-cuvette";
    f.manifest.manifest_revision = 3;
    f.manifest.darks[1].daq_configuration_id = "wrong-daq";
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    REQUIRE_FALSE(load_production_session(f.root));
    f.manifest.darks[1].daq_configuration_id = "daq-1";
    f.manifest.manifest_revision = 4;
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    std::fstream corrupt(
        f.root / "scans/blank.bin", std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(80);
    corrupt.put('\0');
    corrupt.flush();
    corrupt.close();
    REQUIRE(load_production_session(f.root).error().code == ErrorCode::ChecksumMismatch);
}

TEST_CASE("production validator requires each binary timestamp to equal its manifest timestamp")
{
    Fixture f;
    populate(f);
    f.manifest.darks[0].acquired_at = fixture_time(9);
    f.manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    REQUIRE(load_production_session(f.root).error().location == "darks");
    f.manifest.darks[0].acquired_at = fixture_time(10);
    f.manifest.energy_bursts[0].entered_at = fixture_time(4);
    f.manifest.manifest_revision = 3;
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    REQUIRE(load_production_session(f.root).error().location == "energy_bursts");
    f.manifest.energy_bursts[0].entered_at = fixture_time(5);
    f.manifest.scans[0].acquired_at = fixture_time(19);
    f.manifest.manifest_revision = 4;
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    REQUIRE(load_production_session(f.root).error().location == "scans");
}

TEST_CASE("production validator refuses duplicate and dangling identities and incomplete inputs")
{
    Fixture duplicate;
    populate(duplicate);
    duplicate.manifest.scans[1].acquisition_id = duplicate.manifest.scans[0].acquisition_id;
    duplicate.manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(duplicate.root, duplicate.manifest));
    REQUIRE(load_production_session(duplicate.root).error().location == "input_inventory");

    Fixture dangling;
    populate(dangling);
    dangling.manifest.curves[0].energy_burst_id
        = *Uuid::parse("121e8400-e29b-41d4-a716-446655440000");
    dangling.manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(dangling.root, dangling.manifest));
    REQUIRE(load_production_session(dangling.root).error().location == "curves.energy_burst_id");

    Fixture incomplete;
    populate(incomplete);
    incomplete.manifest.scans[1].state = AcquisitionState::Incomplete;
    incomplete.manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(incomplete.root, incomplete.manifest));
    REQUIRE_FALSE(load_production_session(incomplete.root));
}

TEST_CASE("production validator refuses uncharacterized instrument uncertainty")
{
    Fixture f;
    populate(f);
    std::ifstream input(f.root / "instrument.toml", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto instrument = *toml_io::parse_instrument(text);
    instrument.energy_meter_standard_uncertainty_j = 0;
    f.manifest.instrument_snapshot
        = f.save("instrument.toml", *toml_io::format_instrument(instrument));
    f.manifest.manifest_revision = 2;
    REQUIRE(write_session_manifest_atomic(f.root, f.manifest));
    REQUIRE_FALSE(load_production_session(f.root));
}
