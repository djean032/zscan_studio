#pragma once

#include "zscan/hardware/interfaces.hpp"

#include <string>

namespace zscan::hardware {

struct NidaqmxSettings {
    std::string device { "Dev1" };
    std::string ai0 { "ai0" };
    std::string ai1 { "ai1" };
    std::string trigger_terminal { "/Dev1/PFI0" };
    double sample_rate_hz { 1'000'000.0 };
    std::uint32_t pretrigger_samples { 10 };
    std::uint32_t posttrigger_samples { 140 };
    double minimum_v { -10.0 };
    double maximum_v { 10.0 };
    double timeout_s { 5.0 };
};

class NidaqmxDetector final : public DetectorAcquisition {
public:
    explicit NidaqmxDetector(NidaqmxSettings settings = {});
    ~NidaqmxDetector() override;

    Result<DetectorCapture> capture_one() override;

private:
    NidaqmxSettings settings_;
};

} // namespace zscan::hardware
