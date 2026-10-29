#include "zscan/hardware/factory.hpp"

#include "zscan/hardware/esp302.hpp"

#include <utility>

#ifdef ZSCAN_ENABLE_NIDAQMX
#include "zscan/hardware/nidaqmx.hpp"
#endif

namespace zscan::hardware {

std::expected<HardwareBundle, std::string> make_hardware(
    const HardwareConfiguration& configuration)
{
    if (configuration.stage_host.empty() || configuration.stage_port == 0
        || configuration.stage_axis < 1)
        return std::unexpected("invalid ESP302 hardware configuration");
    if (configuration.daq_device.empty() || configuration.daq_ai0.empty()
        || configuration.daq_ai1.empty() || configuration.daq_trigger_terminal.empty())
        return std::unexpected("invalid NI-DAQmx channel configuration");
    if (configuration.daq_sample_rate_hz <= 0
        || configuration.daq_pretrigger_samples == 0
        || configuration.daq_posttrigger_samples == 0
        || configuration.daq_minimum_v >= configuration.daq_maximum_v
        || configuration.daq_timeout_s <= 0)
        return std::unexpected("invalid NI-DAQmx timing or voltage configuration");

    HardwareBundle result;
    result.stage = std::make_unique<Esp302Stage>(QString::fromStdString(configuration.stage_host),
        configuration.stage_port, configuration.stage_axis);
#ifdef ZSCAN_ENABLE_NIDAQMX
    NidaqmxSettings settings;
    settings.device = configuration.daq_device;
    settings.ai0 = configuration.daq_ai0;
    settings.ai1 = configuration.daq_ai1;
    settings.trigger_terminal = configuration.daq_trigger_terminal;
    settings.sample_rate_hz = configuration.daq_sample_rate_hz;
    settings.pretrigger_samples = configuration.daq_pretrigger_samples;
    settings.posttrigger_samples = configuration.daq_posttrigger_samples;
    settings.minimum_v = configuration.daq_minimum_v;
    settings.maximum_v = configuration.daq_maximum_v;
    settings.timeout_s = configuration.daq_timeout_s;
    result.detector = std::make_unique<NidaqmxDetector>(std::move(settings));
#else
    return std::unexpected("NI-DAQmx support is not enabled in this build");
#endif
    return result;
}

} // namespace zscan::hardware
