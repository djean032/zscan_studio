#include "zscan/acquisition/dark_runner.hpp"
#include "zscan/acquisition/session.hpp"
#include "zscan/hardware/mock.hpp"
#include "zscan/schema/toml_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace zscan;

std::string text(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

struct Temp {
    std::filesystem::path root = std::filesystem::path("/tmp/opencode")
        / ("zscan-new-session-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temp() { std::filesystem::remove_all(root); }
};
}

TEST_CASE("mock new session persists snapshots dark energy and scan with revisioned manifest")
{
    Temp tmp;
    auto sample = *schema::toml_io::parse_sample(
        text(std::filesystem::path(ZSCAN_SOURCE_DIR) / "examples/sessions/valid/sample.toml"));
    auto instrument = *schema::toml_io::parse_instrument(
        text(std::filesystem::path(ZSCAN_SOURCE_DIR) / "examples/sessions/valid/instrument.toml"));
    instrument.snapshot_id = *schema::binary::random_uuid_v4();
    const auto created = acquisition::create_session(tmp.root, sample, instrument);
    REQUIRE(created);
    REQUIRE(created->manifest_revision == 1);
    REQUIRE(created->scans.empty());
    REQUIRE(std::filesystem::exists(tmp.root / "sample.toml"));
    REQUIRE(std::filesystem::exists(tmp.root / "instrument.toml"));
    const auto sample_bytes = text(tmp.root / "sample.toml");
    REQUIRE(created->sample_snapshot.sha256
        == *schema::binary::sha256(std::span(
            reinterpret_cast<const std::uint8_t*>(sample_bytes.data()), sample_bytes.size())));

    schema::CaptureSettings capture { 1e6, 10, 140, 1, 1, 2, 1 };
    hardware::MockDetector dark_detector;
    dark_detector.responses.push_back(hardware::DetectorCapture { 0.01, 0.02 });
    dark_detector.responses.push_back(hardware::DetectorCapture { 0.02, 0.03 });
    acquisition::DarkRunner dark_runner({ capture, -10, 10, -10, 10 }, created->session_id,
        *schema::binary::random_uuid_v4(), dark_detector);
    const auto dark = dark_runner.run();
    REQUIRE(dark.error.empty());
    REQUIRE(schema::binary::validate_dark(dark.dark));
    const auto after_dark = acquisition::publish_dark(tmp.root, dark.dark,
        { true, true, true, true }, instrument.daq_configurations.front().configuration_id);
    REQUIRE(after_dark);
    REQUIRE(after_dark->manifest_revision == 2);

    schema::binary::EnergyFile energy { { schema::binary::FileKind::Energy,
                                            *schema::binary::random_uuid_v4(), created->session_id,
                                            dark.dark.header.timestamp },
        { 50e-9, 52e-9 } };
    const auto after_energy = acquisition::publish_energy(tmp.root, energy);
    REQUIRE(after_energy);
    REQUIRE(after_energy->manifest_revision == 3);

    acquisition::RunConfiguration config;
    config.session_id = created->session_id;
    config.acquisition_id = *schema::binary::random_uuid_v4();
    config.positions_m = { -0.001, 0.001 };
    config.grid = { -0.001, 0.001, 2, 0.002 };
    config.capture = capture;
    config.maximum_move_attempts = 2;
    config.maximum_readback_error_m = 1e-6;
    hardware::MockStage stage;
    stage.responses.push_back(hardware::StagePosition { -0.001, -0.001 });
    stage.responses.push_back(hardware::StagePosition { 0.001, 0.001 });
    hardware::MockDetector detector;
    for (int i = 0; i < 4; ++i)
        detector.responses.push_back(hardware::DetectorCapture { 1.0, 2.0 });
    acquisition::Runner runner(config, stage, detector);
    const auto outcome = runner.run();
    REQUIRE(outcome.error.empty());
    REQUIRE(schema::binary::validate_scan(outcome.scan));
    const auto after_scan
        = acquisition::publish_scan(tmp.root, outcome, instrument.cuvettes.front().cuvette_id);
    REQUIRE(after_scan);
    REQUIRE(after_scan->manifest_revision == 4);
    REQUIRE(after_scan->scans.size() == 1);
    REQUIRE(schema::storage::read_session_manifest(tmp.root)->manifest_revision == 4);
    REQUIRE_FALSE(
        acquisition::publish_scan(tmp.root, outcome, instrument.cuvettes.front().cuvette_id));
    REQUIRE(schema::storage::read_session_manifest(tmp.root)->manifest_revision == 4);
    REQUIRE_FALSE(acquisition::create_session(tmp.root, sample, instrument));

    auto blank = outcome;
    blank.scan.header.record_id = *schema::binary::random_uuid_v4();
    blank.scan.config.kind = schema::ScanKind::Blank;
    auto saved_blank
        = acquisition::publish_scan(tmp.root, blank, instrument.cuvettes.back().cuvette_id);
    REQUIRE(saved_blank);
    auto curve = acquisition::publish_curve(tmp.root, outcome.scan.header.record_id,
        blank.scan.header.record_id, energy.header.record_id,
        instrument.cuvette_pairs.front().calibration_id);
    REQUIRE(curve);
    REQUIRE(curve->curves.size() == 1);
    REQUIRE(curve->manifest_revision == 6);
    REQUIRE_FALSE(acquisition::publish_curve(tmp.root, outcome.scan.header.record_id,
        *schema::binary::random_uuid_v4(), energy.header.record_id, "pair-1"));
}

TEST_CASE("failed mock dark and scan never alter the manifest")
{
    Temp tmp;
    auto sample = *schema::toml_io::parse_sample(
        text(std::filesystem::path(ZSCAN_SOURCE_DIR) / "examples/sessions/valid/sample.toml"));
    auto instrument = *schema::toml_io::parse_instrument(
        text(std::filesystem::path(ZSCAN_SOURCE_DIR) / "examples/sessions/valid/instrument.toml"));
    instrument.snapshot_id = *schema::binary::random_uuid_v4();
    const auto created = acquisition::create_session(tmp.root, sample, instrument);
    REQUIRE(created);
    hardware::MockDetector detector;
    detector.responses.push_back(std::unexpected(
        hardware::HardwareError { "timeout", schema::binary::RejectionReason::TriggerTimeout }));
    acquisition::DarkRunner runner({ { 1e6, 10, 140, 1, 1, 2, 1 }, -10, 10, -10, 10 },
        created->session_id, *schema::binary::random_uuid_v4(), detector);
    auto dark = runner.run();
    REQUIRE_FALSE(dark.error.empty());
    REQUIRE_FALSE(acquisition::publish_dark(tmp.root, dark.dark, { true, true, true, true },
        instrument.daq_configurations.front().configuration_id));
    REQUIRE_FALSE(std::filesystem::exists(tmp.root / "darks"));
    acquisition::RunOutcome incomplete;
    incomplete.scan.header.session_id = created->session_id;
    incomplete.scan.end.result = schema::binary::ScanResult::Incomplete;
    REQUIRE_FALSE(acquisition::publish_scan(tmp.root, incomplete, "sample-cuvette"));
    REQUIRE_FALSE(std::filesystem::exists(tmp.root / "scans"));
    REQUIRE(schema::storage::read_session_manifest(tmp.root)->manifest_revision == 1);
}
