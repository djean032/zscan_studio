#pragma once

#include "zscan/hardware/interfaces.hpp"
#include "zscan/schema/scan.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zscan::acquisition {

struct RunProgress {
    std::size_t position_index { };
    std::size_t position_count { };
    std::size_t accepted_readings { };
    std::size_t required_readings { };
    std::size_t rejected_attempts { };
};

struct RunConfiguration {
    schema::Uuid session_id;
    schema::Uuid acquisition_id;
    schema::ScanKind kind { schema::ScanKind::Sample };
    std::vector<double> positions_m;
    schema::ScanGrid grid;
    schema::CaptureSettings capture;
    std::uint8_t maximum_move_attempts { 2 };
    double maximum_readback_error_m { 1e-6 };
    double ai0_minimum_v { -10.0 };
    double ai0_maximum_v { 10.0 };
    double ai1_minimum_v { -10.0 };
    double ai1_maximum_v { 10.0 };
    std::function<void(const RunProgress&)> progress_callback;
    std::filesystem::path session_root;
    std::optional<schema::SessionPath> scan_path;
};

struct RunOutcome {
    schema::binary::ScanFile scan;
    std::string error;
};

class Runner final {
public:
    Runner(RunConfiguration configuration, hardware::StageController& stage,
        hardware::DetectorAcquisition& detector);

    [[nodiscard]] RunOutcome run();
    void request_cancel() noexcept { cancel_requested_.store(true); }
    void request_emergency_stop() noexcept { emergency_stop_requested_.store(true); }

private:
    bool stopped() const noexcept;

    RunConfiguration configuration_;
    hardware::StageController& stage_;
    hardware::DetectorAcquisition& detector_;
    std::atomic_bool cancel_requested_ { false };
    std::atomic_bool emergency_stop_requested_ { false };
};

} // namespace zscan::acquisition
