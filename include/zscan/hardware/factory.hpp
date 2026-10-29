#pragma once

#include "zscan/hardware/interfaces.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace zscan::hardware {

struct HardwareConfiguration {
    std::string stage_host { "192.168.254.254" };
    std::uint16_t stage_port { 5001 };
    int stage_axis { 3 };
    std::string daq_device { "Dev1" };
    std::string daq_ai0 { "ai0" };
    std::string daq_ai1 { "ai1" };
    std::string daq_trigger_terminal { "/Dev1/PFI0" };
    double daq_sample_rate_hz { 1'000'000.0 };
    std::uint32_t daq_pretrigger_samples { 10 };
    std::uint32_t daq_posttrigger_samples { 140 };
    double daq_minimum_v { -10.0 };
    double daq_maximum_v { 10.0 };
    double daq_timeout_s { 5.0 };
};

struct HardwareBundle {
    std::unique_ptr<StageController> stage;
    std::unique_ptr<DetectorAcquisition> detector;
};

[[nodiscard]] std::expected<HardwareBundle, std::string> make_hardware(
    const HardwareConfiguration& configuration);

} // namespace zscan::hardware
