#pragma once

#include "zscan/schema/dark_energy.hpp"

#include <filesystem>

namespace zscan::schema {

struct ValidatedCurve {
    std::size_t sample_scan_index { };
    std::size_t blank_scan_index { };
    std::size_t energy_burst_index { };
    std::size_t selected_dark_index { };
    std::size_t cuvette_pair_index { };
};

struct ValidatedSession {
    SessionManifest manifest;
    SampleSnapshot sample;
    InstrumentSnapshot instrument;
    std::vector<binary::ScanFile> scans;
    std::vector<binary::DarkFile> darks;
    std::vector<binary::EnergyFile> energy_bursts;
    std::vector<ValidatedCurve> curves;
    std::vector<SchemaError> warnings;
};

// No GUI, hardware, or fit dependencies. Validates immutable files, references,
// configuration and calibrated production readiness; selects the closest
// eligible dark to each sample/blank midpoint with earlier-time tie breaking.
[[nodiscard]] Result<ValidatedSession> load_production_session(
    const std::filesystem::path& session_root);

} // namespace zscan::schema
