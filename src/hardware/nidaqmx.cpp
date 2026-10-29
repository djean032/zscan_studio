#include "zscan/hardware/nidaqmx.hpp"

#include <NIDAQmx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zscan::hardware {
namespace {

HardwareError daq_error(int code, const char* operation)
{
    char buffer[2048] { };
    DAQmxGetExtendedErrorInfo(buffer, sizeof(buffer));
    return { std::string(operation) + " failed (" + std::to_string(code) + "): " + buffer,
        code == DAQmxErrorSamplesNoLongerAvailable ? schema::binary::RejectionReason::DaqError
                                                    : schema::binary::RejectionReason::Other };
}

} // namespace

NidaqmxDetector::NidaqmxDetector(NidaqmxSettings settings)
    : settings_(std::move(settings))
{
}

NidaqmxDetector::~NidaqmxDetector() = default;

Result<DetectorCapture> NidaqmxDetector::capture_one()
{
    if (settings_.pretrigger_samples == 0 || settings_.posttrigger_samples == 0
        || !std::isfinite(settings_.sample_rate_hz) || settings_.sample_rate_hz <= 0
        || settings_.minimum_v >= settings_.maximum_v)
        return std::unexpected(HardwareError { "invalid NI-DAQmx acquisition settings" });
    const auto total = settings_.pretrigger_samples + settings_.posttrigger_samples;
    TaskHandle task = nullptr;
    auto fail = [&](int code, const char* operation) -> Result<DetectorCapture> {
        if (task) DAQmxClearTask(task);
        return std::unexpected(daq_error(code, operation));
    };
    int error = DAQmxCreateTask("zscan_triggered_capture", &task);
    if (error < 0) return fail(error, "DAQmxCreateTask");
    const std::string channels = settings_.device + "/" + settings_.ai0 + "," + settings_.device + "/" + settings_.ai1;
    // Match the previous nidaqmx Python GUI: terminal configuration was left
    // at the device default rather than forcing RSE/differential wiring.
    error = DAQmxCreateAIVoltageChan(task, channels.c_str(), "", DAQmx_Val_Cfg_Default,
        settings_.minimum_v, settings_.maximum_v, DAQmx_Val_Volts, nullptr);
    if (error < 0) return fail(error, "DAQmxCreateAIVoltageChan");
    error = DAQmxCfgSampClkTiming(task, nullptr, settings_.sample_rate_hz,
        DAQmx_Val_Rising, DAQmx_Val_FiniteSamps, total);
    if (error < 0) return fail(error, "DAQmxCfgSampClkTiming");
    error = DAQmxCfgDigEdgeRefTrig(task, settings_.trigger_terminal.c_str(),
        DAQmx_Val_Rising, settings_.pretrigger_samples);
    if (error < 0) return fail(error, "DAQmxCfgDigEdgeRefTrig");
    error = DAQmxStartTask(task);
    if (error < 0) return fail(error, "DAQmxStartTask");
    std::vector<double> samples(static_cast<std::size_t>(total) * 2);
    int read = 0;
    error = DAQmxReadAnalogF64(task, total, settings_.timeout_s, DAQmx_Val_GroupByChannel,
        samples.data(), samples.size(), &read, nullptr);
    if (error < 0) return fail(error, "DAQmxReadAnalogF64");
    if (read != static_cast<int>(total)) return fail(DAQmxErrorSamplesNotYetAvailable, "DAQmxReadAnalogF64 count");
    DAQmxStopTask(task);
    DAQmxClearTask(task);
    const auto ai0_begin = samples.begin() + settings_.pretrigger_samples;
    const auto ai1_begin = samples.begin() + total + settings_.pretrigger_samples;
    const auto ai0_end = samples.begin() + total;
    const auto ai1_end = samples.end();
    const double ai0 = *std::max_element(ai0_begin, ai0_end);
    const double ai1 = *std::max_element(ai1_begin, ai1_end);
    if (!std::isfinite(ai0) || !std::isfinite(ai1))
        return std::unexpected(HardwareError { "nonfinite NI-DAQmx peak", schema::binary::RejectionReason::NonfiniteAi0 });
    return DetectorCapture { ai0, ai1 };
}

} // namespace zscan::hardware
