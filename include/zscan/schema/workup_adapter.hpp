#pragma once

#include "zscan/schema/session_validator.hpp"

#include "zfit/workup/dark.hpp"
#include "zfit/workup/energy.hpp"
#include "zfit/workup/scan.hpp"

namespace zscan::schema {

struct PreparedWorkup {
    std::vector<zfit::workup::PreparedCurve> curves;
    std::vector<zfit::workup::EnergyCalibration> energies;
    std::vector<zfit::workup::DarkSummary> darks;
    std::vector<zfit::workup::CuvettePairCalibration> cuvettes;
    zfit::workup::SampleCalibration calibration;
    // [curve][sample then blank position rows flattened separately by caller]
    std::vector<std::vector<std::vector<std::uint32_t>>> sample_retained_attempt_indexes;
    std::vector<std::vector<std::vector<std::uint32_t>>> blank_retained_attempt_indexes;
};

// The session layer owns all file I/O and stage/rejection audit data; the
// adapter passes accepted pairs, commanded grids, and calibration values only.
[[nodiscard]] Result<PreparedWorkup> prepare_workup(
    const ValidatedSession& session, zfit::workup::DarkDriftLimits drift_limits);

} // namespace zscan::schema
