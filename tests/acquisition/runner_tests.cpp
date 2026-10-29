#include "zscan/acquisition/runner.hpp"
#include "zscan/acquisition/persistence.hpp"
#include "zscan/hardware/mock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
zscan::schema::Uuid id(const char* value)
{
    return *zscan::schema::Uuid::parse(value);
}

zscan::acquisition::RunConfiguration configuration()
{
    zscan::acquisition::RunConfiguration result;
    result.session_id = id("11111111-1111-4111-8111-111111111111");
    result.acquisition_id = id("22222222-2222-4222-8222-222222222222");
    result.positions_m = { 0.0, 0.001 };
    result.grid = { 0.0, 0.001, 2, 0.001 };
    result.capture = { 1e6, 10, 140, 1, 1, 2, 2 };
    result.maximum_move_attempts = 1;
    result.maximum_readback_error_m = 1e-6;
    return result;
}
}

TEST_CASE("acquisition runner completes positions with accepted detector readings")
{
    zscan::hardware::MockStage stage;
    stage.responses.push_back(zscan::hardware::StagePosition { 0.0, 0.0 });
    stage.responses.push_back(zscan::hardware::StagePosition { 0.001, 0.001 });
    zscan::hardware::MockDetector detector;
    detector.responses.push_back(zscan::hardware::DetectorCapture { 1.0, 2.0 });
    detector.responses.push_back(zscan::hardware::DetectorCapture { 1.1, 2.1 });
    detector.responses.push_back(zscan::hardware::DetectorCapture { 1.2, 2.2 });
    detector.responses.push_back(zscan::hardware::DetectorCapture { 1.3, 2.3 });

    zscan::acquisition::Runner runner(configuration(), stage, detector);
    const auto outcome = runner.run();
    REQUIRE(outcome.error.empty());
    REQUIRE(outcome.scan.end.result == zscan::schema::binary::ScanResult::Complete);
    REQUIRE(outcome.scan.end.positions_completed == 2);
    REQUIRE(outcome.scan.end.total_accepted_shots == 4);
}

TEST_CASE("acquisition runner seals a failed position as incomplete")
{
    auto config = configuration();
    config.capture.maximum_rejected_attempts = 2;
    zscan::hardware::MockStage stage;
    stage.responses.push_back(zscan::hardware::StagePosition { 0.0, 0.0 });
    zscan::hardware::MockDetector detector;
    detector.responses.push_back(std::unexpected(zscan::hardware::HardwareError {
        "trigger timeout", zscan::schema::binary::RejectionReason::TriggerTimeout }));
    detector.responses.push_back(std::unexpected(zscan::hardware::HardwareError {
        "trigger timeout", zscan::schema::binary::RejectionReason::TriggerTimeout }));

    zscan::acquisition::Runner runner(config, stage, detector);
    const auto outcome = runner.run();
    REQUIRE_FALSE(outcome.error.empty());
    REQUIRE(outcome.scan.end.result == zscan::schema::binary::ScanResult::Incomplete);
    REQUIRE(outcome.scan.end.reason == zscan::schema::binary::TerminationReason::ShotRetryLimit);
    REQUIRE(outcome.scan.positions.size() == 1);
}

TEST_CASE("acquisition runner responds to cancellation")
{
    auto config = configuration();
    zscan::hardware::MockStage stage;
    zscan::hardware::MockDetector detector;
    zscan::acquisition::Runner runner(config, stage, detector);
    runner.request_cancel();
    const auto outcome = runner.run();
    REQUIRE(outcome.scan.end.result == zscan::schema::binary::ScanResult::Incomplete);
    REQUIRE(outcome.scan.end.reason == zscan::schema::binary::TerminationReason::OperatorCancelled);
}
