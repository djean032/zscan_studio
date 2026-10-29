#include "zscan/schema/validation_report.hpp"

#include "zscan/schema/storage.hpp"
#include "zscan/schema/toml_io.hpp"

#include <fstream>
#include <iterator>
#include <set>

namespace zscan::schema {

ValidationReport validate_session(const std::filesystem::path& root)
{
    ValidationReport report;
    auto manifest = storage::read_session_manifest(root);
    if (!manifest) {
        report.errors.push_back(manifest.error());
        return report;
    }
    auto add = [&](Result<void> result) {
        if (!result)
            report.errors.push_back(result.error());
    };
    std::set<std::string> ids;
    std::set<std::string> paths;
    for (const auto& scan : manifest->scans) {
        if (!ids.insert(scan.acquisition_id.to_string()).second)
            report.errors.push_back({ ErrorCode::InvalidValue, "duplicate acquisition ID", { },
                scan.acquisition_id.to_string(), "scans" });
        if (!paths.insert(scan.file.path.to_string()).second)
            report.errors.push_back({ ErrorCode::InvalidValue, "duplicate input path", { },
                scan.acquisition_id.to_string(), "scans.file.path" });
        add(validate(scan));
    }
    for (const auto& dark : manifest->darks) {
        if (!ids.insert(dark.record_id.to_string()).second)
            report.errors.push_back({ ErrorCode::InvalidValue, "duplicate dark ID", { },
                dark.record_id.to_string(), "darks" });
        add(validate(dark));
    }
    for (const auto& burst : manifest->energy_bursts) {
        if (!ids.insert(burst.burst_id.to_string()).second)
            report.errors.push_back({ ErrorCode::InvalidValue, "duplicate energy burst ID", { },
                burst.burst_id.to_string(), "energy_bursts" });
        add(validate(burst));
    }
    for (const auto& curve : manifest->curves)
        add(validate(curve));
    auto sample_path = manifest->sample_snapshot.path.resolve_within(root);
    if (!sample_path)
        report.errors.push_back(sample_path.error());
    else {
        std::ifstream input(*sample_path, std::ios::binary);
        auto sample
            = toml_io::parse_sample(std::string((std::istreambuf_iterator<char>(input)), { }));
        if (!sample)
            report.errors.push_back(sample.error());
        else
            add(validate(*sample, ValidationLevel::Draft));
    }
    auto instrument_path = manifest->instrument_snapshot.path.resolve_within(root);
    if (!instrument_path)
        report.errors.push_back(instrument_path.error());
    else {
        std::ifstream input(*instrument_path, std::ios::binary);
        auto instrument
            = toml_io::parse_instrument(std::string((std::istreambuf_iterator<char>(input)), { }));
        if (!instrument)
            report.errors.push_back(instrument.error());
        else
            add(validate(*instrument, ValidationLevel::Draft));
    }
    if (report.errors.empty()) {
        auto loaded = load_production_session(root);
        if (!loaded)
            report.errors.push_back(loaded.error());
        else
            report.warnings = std::move(loaded->warnings);
    }
    return report;
}

} // namespace zscan::schema
