#include "zscan/hardware/mock.hpp"

namespace zscan::hardware {

Result<StagePosition> MockStage::move_and_readback(double commanded_z_m)
{
    if (responses.empty())
        return std::unexpected(HardwareError { "mock stage response queue exhausted" });
    auto result = std::move(responses.front());
    responses.pop_front();
    if (result && result->commanded_z_m == 0.0) result->commanded_z_m = commanded_z_m;
    return result;
}

Result<DetectorCapture> MockDetector::capture_one()
{
    if (responses.empty())
        return std::unexpected(HardwareError { "mock detector response queue exhausted" });
    auto result = std::move(responses.front());
    responses.pop_front();
    return result;
}

} // namespace zscan::hardware
