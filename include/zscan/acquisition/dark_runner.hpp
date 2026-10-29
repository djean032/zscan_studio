#pragma once

#include "zscan/hardware/interfaces.hpp"
#include "zscan/schema/dark_energy.hpp"

#include <atomic>
#include <string>

namespace zscan::acquisition {

struct DarkOutcome {
    schema::binary::DarkFile dark;
    std::string error;
};

class DarkRunner final {
public:
    DarkRunner(schema::binary::DarkConfig config, schema::Uuid session_id, schema::Uuid record_id,
        hardware::DetectorAcquisition& detector);
    [[nodiscard]] DarkOutcome run();
    void request_cancel() noexcept { cancelled_.store(true); }

private:
    schema::binary::DarkConfig config_;
    schema::Uuid session_id_;
    schema::Uuid record_id_;
    hardware::DetectorAcquisition& detector_;
    std::atomic_bool cancelled_ { false };
};

} // namespace zscan::acquisition
