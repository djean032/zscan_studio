#include "zscan/hardware/mock.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("hardware mocks consume scripted stage and detector responses")
{
    zscan::hardware::MockStage stage;
    stage.responses.push_back(zscan::hardware::StagePosition { 0.0, 0.001 });
    const auto moved = stage.move_and_readback(0.001);
    REQUIRE(moved);
    REQUIRE(moved->commanded_z_m == 0.001);

    zscan::hardware::MockDetector detector;
    detector.responses.push_back(zscan::hardware::DetectorCapture { 1.2, 2.3 });
    const auto capture = detector.capture_one();
    REQUIRE(capture);
    REQUIRE(capture->ai1_peak_v == 2.3);
    REQUIRE_FALSE(detector.capture_one());
}
