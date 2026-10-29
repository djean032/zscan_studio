#pragma once

#include "zscan/schema/scan.hpp"

#include <expected>
#include <string>

namespace zscan::hardware {

struct HardwareError {
    std::string message;
    schema::binary::RejectionReason reason { schema::binary::RejectionReason::Other };
};

template <typename T> using Result = std::expected<T, HardwareError>;

struct StagePosition {
    double commanded_z_m { };
    double readback_z_m { };
};

class StageController {
public:
    virtual ~StageController() = default;
    virtual Result<StagePosition> move_and_readback(double commanded_z_m) = 0;
};

struct DetectorCapture {
    double ai0_peak_v { };
    double ai1_peak_v { };
};

class DetectorAcquisition {
public:
    virtual ~DetectorAcquisition() = default;
    virtual Result<DetectorCapture> capture_one() = 0;
};

} // namespace zscan::hardware
