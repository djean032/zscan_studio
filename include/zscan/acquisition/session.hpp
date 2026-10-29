#pragma once

#include "zscan/acquisition/runner.hpp"
#include "zscan/schema/dark_energy.hpp"
#include "zscan/schema/storage.hpp"

namespace zscan::acquisition {

// Creates a NEW directory only; takes ownership of already-entered, validated
// scientific metadata. No catalog values are inferred or rewritten.
[[nodiscard]] schema::Result<schema::SessionManifest> create_session(
    const std::filesystem::path& root, const schema::SampleSnapshot& sample,
    schema::InstrumentSnapshot instrument, std::string session_name = {});

[[nodiscard]] schema::Result<schema::SessionManifest> publish_scan(
    const std::filesystem::path& root, const RunOutcome& outcome, const std::string& cuvette_id);

[[nodiscard]] schema::Result<schema::SessionManifest> publish_dark(
    const std::filesystem::path& root, const schema::binary::DarkFile& dark,
    const schema::DarkConditions& conditions, const std::string& daq_configuration_id);

[[nodiscard]] schema::Result<schema::SessionManifest> publish_energy(
    const std::filesystem::path& root, const schema::binary::EnergyFile& energy);

[[nodiscard]] schema::Result<schema::SessionManifest> publish_curve(
    const std::filesystem::path& root, const schema::Uuid& sample_scan,
    const schema::Uuid& blank_scan, const schema::Uuid& energy_burst,
    const std::string& cuvette_pair_calibration_id);

} // namespace zscan::acquisition
