#pragma once

#include "zscan/hardware/interfaces.hpp"

#include <deque>

namespace zscan::hardware {

class MockStage final : public StageController {
public:
    std::deque<Result<StagePosition>> responses;
    Result<StagePosition> move_and_readback(double commanded_z_m) override;
};

class MockDetector final : public DetectorAcquisition {
public:
    std::deque<Result<DetectorCapture>> responses;
    Result<DetectorCapture> capture_one() override;
};

} // namespace zscan::hardware
