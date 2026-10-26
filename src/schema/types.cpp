#include "zscan/schema/types.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <system_error>

namespace zscan::schema {
namespace {

    SchemaError error(ErrorCode code, std::string message)
    {
        return { code, std::move(message), { }, { }, { } };
    }

    Result<void> fail(ErrorCode code, std::string location, std::string message)
    {
        auto diagnostic = error(code, std::move(message));
        diagnostic.location = std::move(location);
        return std::unexpected(std::move(diagnostic));
    }

    Result<void> context(Result<void> result, std::string_view prefix, std::string_view id = { })
    {
        if (!result) {
            auto diagnostic = std::move(result.error());
            diagnostic.location = diagnostic.location.empty()
                ? std::string(prefix)
                : std::string(prefix) + "." + diagnostic.location;
            if (!id.empty())
                diagnostic.record_id = id;
            return std::unexpected(std::move(diagnostic));
        }
        return { };
    }

    bool valid_uuid(const Uuid& id)
    {
        return (id.bytes[6] >> 4) == 4 && (id.bytes[8] & 0xc0) == 0x80;
    }

    bool valid_timestamp(const UtcTimestamp& time)
    {
        if (time.nanosecond >= 1'000'000'000)
            return false;
        // A parsed timestamp always has a four-digit positive year.
        const auto day = std::chrono::floor<std::chrono::days>(time.seconds);
        const auto year = int(std::chrono::year_month_day { day }.year());
        return year >= 1 && year <= 9999;
    }

    bool valid_state(AcquisitionState state)
    {
        switch (state) {
        case AcquisitionState::InProgress:
        case AcquisitionState::Complete:
        case AcquisitionState::Incomplete:
        case AcquisitionState::Interrupted:
            return true;
        }
        return false;
    }

    Result<void> validate_file(const FileIdentity& file, bool digest_required)
    {
        if (digest_required && !file.sha256) {
            return fail(
                ErrorCode::MissingField, "sha256", "immutable file requires a SHA-256 digest");
        }
        if (!digest_required && file.sha256) {
            return fail(
                ErrorCode::InvalidValue, "sha256", "in-progress file must not claim a seal");
        }
        return { };
    }

    Result<void> validate_scalar(const ScalarWithUncertainty& scalar, ValidationLevel level)
    {
        if (level == ValidationLevel::Production)
            return validate(scalar);
        if (!std::isfinite(scalar.value) || !std::isfinite(scalar.standard_uncertainty)
            || scalar.standard_uncertainty < 0) {
            return fail(
                ErrorCode::InvalidValue, "value", "draft calibration must have finite values");
        }
        if (scalar.conversion)
            return validate(scalar);
        return { };
    }

    int hex_value(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    }

    std::string hex_bytes(const auto& bytes)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 15]);
        }
        return result;
    }

    bool digits_at(std::string_view text, std::size_t pos, std::size_t count)
    {
        return pos + count <= text.size()
            && std::all_of(text.begin() + pos, text.begin() + pos + count,
                [](char c) { return c >= '0' && c <= '9'; });
    }

    int decimal_at(std::string_view text, std::size_t pos, std::size_t count)
    {
        int result = 0;
        for (std::size_t i = pos; i < pos + count; ++i)
            result = result * 10 + text[i] - '0';
        return result;
    }

    bool is_child_of(const std::filesystem::path& child, const std::filesystem::path& root)
    {
        const auto [root_end, child_end]
            = std::mismatch(root.begin(), root.end(), child.begin(), child.end());
        return root_end == root.end() && child_end != child.end();
    }

} // namespace

SchemaError with_source(SchemaError diagnostic, std::filesystem::path source_path)
{
    diagnostic.source_path = std::move(source_path);
    return diagnostic;
}

Result<Uuid> Uuid::parse(std::string_view text)
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-'
        || text[14] != '4'
        || (text[19] != '8' && text[19] != '9' && text[19] != 'a' && text[19] != 'b')) {
        return std::unexpected(
            error(ErrorCode::InvalidUuid, "expected canonical lowercase UUID v4"));
    }
    Uuid result;
    std::size_t index = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            ++i;
            continue;
        }
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::unexpected(
                error(ErrorCode::InvalidUuid, "expected canonical lowercase UUID v4"));
        }
        result.bytes[index++] = static_cast<std::uint8_t>(hi * 16 + lo);
        i += 2;
    }
    return result;
}

std::string Uuid::to_string() const
{
    auto hex = hex_bytes(bytes);
    hex.insert(20, "-");
    hex.insert(16, "-");
    hex.insert(12, "-");
    hex.insert(8, "-");
    return hex;
}

Result<UtcTimestamp> UtcTimestamp::parse(std::string_view text)
{
    constexpr auto invalid = "expected RFC 3339 UTC time YYYY-MM-DDTHH:MM:SS[.fraction]Z";
    if (text.size() < 20 || !digits_at(text, 0, 4) || text[4] != '-' || !digits_at(text, 5, 2)
        || text[7] != '-' || !digits_at(text, 8, 2) || text[10] != 'T' || !digits_at(text, 11, 2)
        || text[13] != ':' || !digits_at(text, 14, 2) || text[16] != ':' || !digits_at(text, 17, 2)
        || text.back() != 'Z') {
        return std::unexpected(error(ErrorCode::InvalidTimestamp, invalid));
    }
    std::uint32_t nanos = 0;
    if (text.size() != 20) {
        const auto digits = text.size() - 21;
        if (text[19] != '.' || digits == 0 || digits > 9 || !digits_at(text, 20, digits)) {
            return std::unexpected(error(ErrorCode::InvalidTimestamp, invalid));
        }
        for (std::size_t i = 20; i < 20 + digits; ++i)
            nanos = nanos * 10 + text[i] - '0';
        for (std::size_t i = digits; i < 9; ++i)
            nanos *= 10;
    }
    const int y = decimal_at(text, 0, 4);
    const int m = decimal_at(text, 5, 2);
    const int d = decimal_at(text, 8, 2);
    const int h = decimal_at(text, 11, 2);
    const int min = decimal_at(text, 14, 2);
    const int sec = decimal_at(text, 17, 2);
    const std::chrono::year_month_day day { std::chrono::year { y },
        std::chrono::month { static_cast<unsigned>(m) },
        std::chrono::day { static_cast<unsigned>(d) } };
    if (y == 0 || !day.ok() || h > 23 || min > 59 || sec > 59) {
        return std::unexpected(
            error(ErrorCode::InvalidTimestamp, "invalid UTC calendar date or time"));
    }
    return UtcTimestamp { std::chrono::sys_days { day } + std::chrono::hours { h }
            + std::chrono::minutes { min } + std::chrono::seconds { sec },
        nanos };
}

std::string UtcTimestamp::to_string() const
{
    const auto day = std::chrono::floor<std::chrono::days>(seconds);
    const std::chrono::year_month_day date { day };
    const auto since_midnight = seconds - day;
    const auto hours = std::chrono::duration_cast<std::chrono::hours>(since_midnight);
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(since_midnight - hours);
    const auto secs
        = std::chrono::duration_cast<std::chrono::seconds>(since_midnight - hours - minutes);
    auto text = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", int(date.year()),
        unsigned(date.month()), unsigned(date.day()), hours.count(), minutes.count(), secs.count());
    if (nanosecond != 0) {
        auto fraction = std::format("{:09}", nanosecond);
        fraction.erase(fraction.find_last_not_of('0') + 1);
        text += "." + fraction;
    }
    return text + "Z";
}

Result<Sha256Digest> Sha256Digest::parse(std::string_view text)
{
    if (text.size() != 64) {
        return std::unexpected(
            error(ErrorCode::InvalidDigest, "expected 64 lowercase SHA-256 hex digits"));
    }
    Sha256Digest result;
    for (std::size_t i = 0; i < result.bytes.size(); ++i) {
        const int hi = hex_value(text[2 * i]);
        const int lo = hex_value(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::unexpected(
                error(ErrorCode::InvalidDigest, "expected 64 lowercase SHA-256 hex digits"));
        }
        result.bytes[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return result;
}

std::string Sha256Digest::to_string() const { return hex_bytes(bytes); }

Result<SessionPath> SessionPath::parse(std::string_view text)
{
    const auto invalid = [] {
        return std::unexpected(
            error(ErrorCode::UnsafePath, "expected normalized nonempty relative session path"));
    };
    if (text.empty() || text.front() == '/' || text.find('\\') != text.npos
        || text.find('\0') != text.npos || text.back() == '/' || text.find(':') != text.npos) {
        return invalid();
    }
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('/', start);
        const auto part = text.substr(start, end == text.npos ? text.npos : end - start);
        if (part.empty() || part == "." || part == "..")
            return invalid();
        if (end == text.npos)
            break;
        start = end + 1;
    }
    const std::filesystem::path path { std::string(text) };
    if (path.has_root_path() || path.generic_string() != text)
        return invalid();
    return SessionPath { path };
}

Result<std::filesystem::path> SessionPath::resolve_within(
    const std::filesystem::path& session_root) const
{
    std::error_code ec;
    const auto root = std::filesystem::canonical(session_root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) {
        return std::unexpected(
            error(ErrorCode::FilesystemError, "session root must be an existing directory"));
    }
    const auto resolved = std::filesystem::weakly_canonical(root / path_, ec);
    if (ec)
        return std::unexpected(error(ErrorCode::FilesystemError, "cannot resolve session path"));
    if (!is_child_of(resolved, root)) {
        return std::unexpected(
            error(ErrorCode::UnsafePath, "session path escapes session root through a symlink"));
    }
    return resolved;
}

Result<void> validate(const ScanGrid& grid)
{
    if (!std::isfinite(grid.start_z_m) || !std::isfinite(grid.end_z_m)
        || !std::isfinite(grid.spacing_m) || grid.position_count < 2
        || grid.start_z_m == grid.end_z_m || grid.spacing_m == 0
        || std::signbit(grid.end_z_m - grid.start_z_m) != std::signbit(grid.spacing_m)
        || !std::isfinite(grid.end_z_m - grid.start_z_m)) {
        return fail(ErrorCode::InvalidValue, "start_z_m", "invalid linear scan grid");
    }
    const double expected = (grid.end_z_m - grid.start_z_m) / (grid.position_count - 1);
    const double scale
        = std::max({ std::abs(grid.start_z_m), std::abs(grid.end_z_m), std::abs(expected) });
    if (!std::isfinite(expected)
        || std::abs(expected - grid.spacing_m)
            > 64 * std::numeric_limits<double>::epsilon() * scale) {
        return fail(
            ErrorCode::InvalidValue, "spacing_m", "scan grid spacing disagrees with endpoints");
    }
    return { };
}

Result<void> validate(const CaptureSettings& capture)
{
    if (!std::isfinite(capture.sample_rate_hz) || capture.sample_rate_hz <= 0
        || capture.posttrigger_sample_count == 0 || capture.peak_extraction_rule != 1
        || capture.peak_extraction_rule_version == 0 || capture.target_accepted_readings == 0) {
        return fail(ErrorCode::InvalidValue, "sample_rate_hz",
            "invalid sample rate or capture configuration");
    }
    return { };
}

Result<void> validate(const ScalarWithUncertainty& scalar)
{
    if (!std::isfinite(scalar.value) || !std::isfinite(scalar.standard_uncertainty)
        || scalar.standard_uncertainty <= 0 || scalar.provenance_id.empty()
        || scalar.uncertainty_evaluation_id.empty()) {
        return fail(ErrorCode::InvalidValue, "standard_uncertainty",
            "missing or invalid characterized value or uncertainty");
    }
    if (scalar.conversion) {
        const auto& conversion = *scalar.conversion;
        if (conversion.provenance_id.empty() || !std::isfinite(conversion.source_value)
            || conversion.source_value <= 0 || !std::isfinite(conversion.divisor)
            || conversion.divisor <= 0
            || (conversion.method != "rectangular_half_width"
                && conversion.method != "triangular_half_width"
                && conversion.method != "expanded_uncertainty"))
            return fail(
                ErrorCode::InvalidValue, "conversion", "invalid uncertainty conversion metadata");
        if ((conversion.method == "rectangular_half_width"
                && std::abs(conversion.divisor - std::sqrt(3.0)) > 1e-12)
            || (conversion.method == "triangular_half_width"
                && std::abs(conversion.divisor - std::sqrt(6.0)) > 1e-12))
            return fail(ErrorCode::InvalidValue, "conversion.divisor",
                "bound conversion divisor disagrees with distribution");
        const double computed = conversion.source_value / conversion.divisor;
        if (!std::isfinite(computed)
            || std::abs(computed - scalar.standard_uncertainty)
                > 1e-12 * std::max(computed, scalar.standard_uncertainty))
            return fail(ErrorCode::InvalidValue, "conversion",
                "standard uncertainty disagrees with quoted source value");
    }
    return { };
}

Result<void> validate(const DaqConfiguration& config)
{
    if (config.configuration_id.empty() || config.device_id.empty()
        || (config.trigger_edge != TriggerEdge::Rising
            && config.trigger_edge != TriggerEdge::Falling)
        || config.channels[0].role != DetectorRole::Reference
        || config.channels[1].role != DetectorRole::Signal
        || config.channels[0].channel_index == config.channels[1].channel_index) {
        return fail(ErrorCode::InvalidValue, "channels", "invalid DAQ identity or channel roles");
    }
    for (const auto& channel : config.channels) {
        if ((channel.terminal_configuration != TerminalConfiguration::Rse
                && channel.terminal_configuration != TerminalConfiguration::Nrse
                && channel.terminal_configuration != TerminalConfiguration::Differential
                && channel.terminal_configuration != TerminalConfiguration::PseudoDifferential)
            || (channel.coupling != Coupling::Dc && channel.coupling != Coupling::Ac)
            || !std::isfinite(channel.minimum_v) || !std::isfinite(channel.maximum_v)
            || channel.minimum_v >= channel.maximum_v) {
            return fail(
                ErrorCode::InvalidValue, "channels.voltage_range_v", "invalid DAQ voltage range");
        }
    }
    return { };
}

Result<void> validate(const StageConfiguration& config)
{
    if (config.configuration_id.empty() || config.controller_id.empty() || config.axis == 0
        || !std::isfinite(config.maximum_readback_error_m) || config.maximum_readback_error_m <= 0
        || config.tolerance_provenance_id.empty() || config.maximum_move_attempts != 2) {
        return fail(ErrorCode::InvalidValue, "maximum_readback_error_m",
            "invalid stage acceptance configuration");
    }
    return { };
}

Result<void> validate(const OpticalFilter& filter)
{
    if (filter.filter_id.empty())
        return fail(ErrorCode::MissingField, "filter_id", "filter needs an ID");
    if (!std::isfinite(filter.nominal_optical_density) || filter.nominal_optical_density < 0)
        return fail(ErrorCode::InvalidValue, "nominal_optical_density",
            "OD must be finite and nonnegative");
    if (!std::isfinite(filter.wavelength_m) || filter.wavelength_m <= 0)
        return fail(ErrorCode::InvalidValue, "wavelength_m", "filter wavelength must be positive");
    if (filter.provenance_id.empty())
        return fail(ErrorCode::MissingField, "provenance_id", "filter needs provenance");
    return { };
}

Result<void> validate(const OpticalConfiguration& config)
{
    if (config.configuration_id.empty())
        return fail(
            ErrorCode::MissingField, "configuration_id", "optical configuration needs an ID");
    for (const auto& [name, filters] :
        { std::pair { "reference_filter_ids", &config.reference_filter_ids },
            std::pair { "signal_filter_ids", &config.signal_filter_ids } }) {
        std::set<std::string> seen;
        for (const auto& id : *filters) {
            if (id.empty() || !seen.insert(id).second)
                return fail(ErrorCode::InvalidValue, name,
                    "filter IDs must be nonempty and unique per channel");
        }
    }
    return { };
}

Result<void> validate(const ScanConfiguration& config)
{
    if (config.configuration_id.empty() || config.daq_configuration_id.empty()
        || config.optical_configuration_id.empty() || config.stage_configuration_id.empty())
        return fail(ErrorCode::MissingField, "configuration_id",
            "scan configuration needs all component IDs");
    return { };
}

Result<void> validate(const ScanRecord& scan)
{
    const auto id = scan.acquisition_id.to_string();
    if (!valid_uuid(scan.acquisition_id))
        return fail(ErrorCode::InvalidUuid, "acquisition_id", "invalid scan UUID");
    if (!valid_state(scan.state) || (scan.kind != ScanKind::Blank && scan.kind != ScanKind::Sample))
        return context(
            fail(ErrorCode::InvalidValue, "kind", "unknown scan kind or lifecycle state"), "scan",
            id);
    if (!valid_timestamp(scan.acquired_at))
        return fail(ErrorCode::InvalidTimestamp, "acquired_at", "invalid scan time");
    if (auto result
        = context(validate_file(scan.file, scan.state != AcquisitionState::InProgress), "file", id);
        !result)
        return result;
    if (scan.state == AcquisitionState::Interrupted) {
        if (!scan.valid_prefix_byte_size || !scan.valid_frame_count
            || *scan.valid_prefix_byte_size < 80
            || *scan.valid_prefix_byte_size > scan.file.byte_size)
            return context(fail(ErrorCode::InvalidValue, "valid_prefix_byte_size",
                               "interrupted scan requires a bounded CRC-valid prefix"),
                "scan", id);
    } else if (scan.valid_prefix_byte_size || scan.valid_frame_count)
        return context(fail(ErrorCode::InvalidValue, "valid_prefix_byte_size",
                           "only interrupted scans record valid-prefix metadata"),
            "scan", id);
    if (scan.solvent_id.empty() || scan.cuvette_id.empty() || scan.scan_configuration_id.empty())
        return context(fail(ErrorCode::MissingField, "scan_configuration_id",
                           "scan needs solvent, cuvette and configuration IDs"),
            "scan", id);
    if ((scan.kind == ScanKind::Blank && !scan.sample_id.empty())
        || (scan.kind == ScanKind::Sample && scan.sample_id.empty()))
        return context(fail(ErrorCode::InvalidValue, "sample_id",
                           "only sample scans carry a molecular sample ID"),
            "scan", id);
    if (auto result = context(validate(scan.grid), "grid", id); !result)
        return result;
    return context(validate(scan.capture), "capture", id);
}

Result<void> validate(const DarkRecord& dark)
{
    const auto id = dark.record_id.to_string();
    if (!valid_uuid(dark.record_id))
        return fail(ErrorCode::InvalidUuid, "record_id", "invalid dark UUID");
    if (!valid_state(dark.state))
        return context(
            fail(ErrorCode::InvalidValue, "state", "unknown dark lifecycle state"), "dark", id);
    if (!valid_timestamp(dark.acquired_at))
        return fail(ErrorCode::InvalidTimestamp, "acquired_at", "invalid dark time");
    if (dark.daq_configuration_id.empty())
        return context(fail(ErrorCode::MissingField, "daq_configuration_id",
                           "dark needs an electronic configuration"),
            "dark", id);
    if (auto result
        = context(validate_file(dark.file, dark.state != AcquisitionState::InProgress), "file", id);
        !result)
        return result;
    return context(validate(dark.capture), "capture", id);
}

Result<void> validate(const EnergyBurstRecord& burst)
{
    const auto id = burst.burst_id.to_string();
    if (!valid_uuid(burst.burst_id))
        return fail(ErrorCode::InvalidUuid, "burst_id", "invalid energy burst UUID");
    if (burst.revision_of
        && (!valid_uuid(*burst.revision_of) || *burst.revision_of == burst.burst_id))
        return context(
            fail(ErrorCode::InvalidValue, "revision_of", "revision must name another valid burst"),
            "energy_burst", id);
    if (!valid_timestamp(burst.entered_at))
        return fail(ErrorCode::InvalidTimestamp, "entered_at", "invalid entry time");
    if (auto result = context(validate_file(burst.file, true), "file", id); !result)
        return result;
    if (burst.reading_count < 2)
        return context(fail(ErrorCode::InvalidValue, "reading_count",
                           "at least two energy readings are required"),
            "energy_burst", id);
    if (burst.energy_meter_calibration_id.empty())
        return context(fail(ErrorCode::MissingField, "energy_meter_calibration_id",
                           "burst needs meter calibration ID"),
            "energy_burst", id);
    return { };
}

Result<void> validate(const CurveMapping& curve)
{
    if (!valid_uuid(curve.curve_id) || !valid_uuid(curve.sample_acquisition_id)
        || !valid_uuid(curve.blank_acquisition_id) || !valid_uuid(curve.energy_burst_id))
        return fail(ErrorCode::InvalidUuid, "curve_id", "curve IDs must be UUID v4 values");
    if (curve.sample_acquisition_id == curve.blank_acquisition_id)
        return context(fail(ErrorCode::InvalidValue, "blank_acquisition_id",
                           "sample and blank must be distinct"),
            "curve", curve.curve_id.to_string());
    if (curve.cuvette_pair_calibration_id.empty())
        return context(fail(ErrorCode::MissingField, "cuvette_pair_calibration_id",
                           "curve needs a pair calibration"),
            "curve", curve.curve_id.to_string());
    return { };
}

Result<void> validate(const SampleSnapshot& sample, ValidationLevel level)
{
    if (sample.sample_id.empty() || sample.solvent_id.empty())
        return fail(ErrorCode::MissingField, "sample_id", "sample and solvent need IDs");
    if (auto result
        = context(validate_scalar(sample.concentration_mol_per_l, level), "concentration");
        !result)
        return result;
    if (auto result = context(
            validate_scalar(sample.solvent_refractive_index, level), "solvent_refractive_index");
        !result)
        return result;
    if (sample.concentration_mol_per_l.value <= 0 || sample.solvent_refractive_index.value <= 0)
        return fail(ErrorCode::InvalidValue, "concentration",
            "concentration and refractive index must be positive");
    return { };
}

Result<void> validate(const InstrumentSnapshot& instrument, ValidationLevel level)
{
    if (!valid_uuid(instrument.snapshot_id))
        return fail(ErrorCode::InvalidUuid, "snapshot_id", "invalid snapshot UUID");
    if (instrument.source_catalog_id.empty() || instrument.laser_characterization_id.empty()
        || instrument.energy_meter_calibration_id.empty())
        return fail(ErrorCode::MissingField, "source_catalog_id",
            "catalog, laser and meter IDs are required");
    if (!std::isfinite(instrument.wavelength_m) || instrument.wavelength_m <= 0
        || !std::isfinite(instrument.pulse_fwhm_s) || instrument.pulse_fwhm_s <= 0)
        return fail(
            ErrorCode::InvalidValue, "wavelength_m", "laser wavelength and FWHM must be positive");
    if (auto result = context(validate_scalar(instrument.waist_m, level), "waist_m"); !result)
        return result;
    if (auto result = context(validate_scalar(instrument.m2, level), "m2"); !result)
        return result;
    if (auto result = context(validate_scalar(instrument.entrance_window_refractive_index, level),
            "entrance_window_refractive_index");
        !result)
        return result;
    if (instrument.waist_m.value <= 0 || instrument.m2.value <= 0
        || instrument.entrance_window_refractive_index.value <= 0)
        return fail(
            ErrorCode::InvalidValue, "waist_m", "waist, M² and window index must be positive");
    if (!std::isfinite(instrument.energy_meter_standard_uncertainty_j)
        || instrument.energy_meter_standard_uncertainty_j < 0
        || (level == ValidationLevel::Production
            && (instrument.energy_meter_standard_uncertainty_j == 0
                || instrument.energy_meter_uncertainty_provenance_id.empty())))
        return fail(ErrorCode::InvalidValue, "energy_meter_standard_uncertainty_j",
            "production meter needs positive characterized uncertainty in joules");
    for (const auto& cuvette : instrument.cuvettes) {
        if (cuvette.cuvette_id.empty())
            return fail(ErrorCode::MissingField, "cuvettes.cuvette_id", "cuvette needs an ID");
        if (auto result = context(validate_scalar(cuvette.path_length_m, level),
                "cuvettes.path_length_m", cuvette.cuvette_id);
            !result)
            return result;
        if (cuvette.path_length_m.value <= 0)
            return fail(
                ErrorCode::InvalidValue, "cuvettes.path_length_m", "path length must be positive");
    }
    for (const auto& pair : instrument.cuvette_pairs) {
        if (pair.calibration_id.empty() || pair.sample_cuvette_id.empty()
            || pair.blank_cuvette_id.empty() || pair.scan_configuration_id.empty()
            || pair.sample_cuvette_id == pair.blank_cuvette_id)
            return fail(ErrorCode::InvalidValue, "cuvette_pairs",
                "pair requires distinct fixed-role cuvettes and IDs");
        if (auto result = context(validate_scalar(pair.correction_factor, level),
                "cuvette_pairs.correction_factor", pair.calibration_id);
            !result)
            return result;
        if (pair.correction_factor.value <= 0)
            return fail(ErrorCode::InvalidValue, "cuvette_pairs.correction_factor",
                "correction factor must be positive");
    }
    for (const auto& config : instrument.daq_configurations)
        if (auto result = context(validate(config), "daq_configurations", config.configuration_id);
            !result)
            return result;
    for (const auto& config : instrument.optical_filters)
        if (auto result = context(validate(config), "optical_filters", config.filter_id); !result)
            return result;
    for (const auto& config : instrument.optical_configurations)
        if (auto result
            = context(validate(config), "optical_configurations", config.configuration_id);
            !result)
            return result;
    for (const auto& config : instrument.stage_configurations)
        if (auto result
            = context(validate(config), "stage_configurations", config.configuration_id);
            !result)
            return result;
    for (const auto& config : instrument.scan_configurations)
        if (auto result = context(validate(config), "scan_configurations", config.configuration_id);
            !result)
            return result;
    // Uniqueness and references between catalog records are checked in Phase 7.
    return { };
}

Result<void> validate(const DerivedRunReference& run)
{
    if (!valid_uuid(run.run_id))
        return fail(ErrorCode::InvalidUuid, "run_id", "invalid run UUID");
    if (!valid_timestamp(run.created_at))
        return fail(ErrorCode::InvalidTimestamp, "created_at", "invalid run time");
    return context(validate_file(run.run_manifest, true), "run_manifest", run.run_id.to_string());
}

Result<void> validate(const SessionManifest& session)
{
    if (!valid_uuid(session.session_id))
        return fail(ErrorCode::InvalidUuid, "session_id", "invalid session UUID");
    if (!valid_timestamp(session.created_at))
        return fail(ErrorCode::InvalidTimestamp, "created_at", "invalid session time");
    if (session.manifest_revision == 0)
        return fail(
            ErrorCode::InvalidValue, "manifest_revision", "manifest revision starts at one");
    if (auto result = context(validate_file(session.sample_snapshot, true), "sample_snapshot");
        !result)
        return result;
    if (auto result
        = context(validate_file(session.instrument_snapshot, true), "instrument_snapshot");
        !result)
        return result;
    for (const auto& scan : session.scans)
        if (auto result = context(validate(scan), "scans", scan.acquisition_id.to_string());
            !result)
            return result;
    for (const auto& dark : session.darks)
        if (auto result = context(validate(dark), "darks", dark.record_id.to_string()); !result)
            return result;
    for (const auto& burst : session.energy_bursts)
        if (auto result = context(validate(burst), "energy_bursts", burst.burst_id.to_string());
            !result)
            return result;
    for (const auto& curve : session.curves)
        if (auto result = context(validate(curve), "curves", curve.curve_id.to_string()); !result)
            return result;
    for (const auto& run : session.derived_runs)
        if (auto result = context(validate(run), "derived_runs", run.run_id.to_string()); !result)
            return result;
    return { };
}

} // namespace zscan::schema
