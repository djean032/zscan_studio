#include "zscan/schema/session_validator.hpp"

#include "zscan/schema/storage.hpp"
#include "zscan/schema/toml_io.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <set>
#include <string>

namespace zscan::schema {
namespace {

    template <typename T>
    Result<T> refuse(
        std::string location, std::string message, ErrorCode code = ErrorCode::InvalidValue)
    {
        return std::unexpected(
            SchemaError { code, std::move(message), { }, { }, std::move(location) });
    }

    bool bits_equal(double a, double b)
    {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    }

    bool same_capture(const CaptureSettings& a, const CaptureSettings& b)
    {
        return bits_equal(a.sample_rate_hz, b.sample_rate_hz)
            && a.pretrigger_sample_count == b.pretrigger_sample_count
            && a.posttrigger_sample_count == b.posttrigger_sample_count
            && a.peak_extraction_rule == b.peak_extraction_rule
            && a.peak_extraction_rule_version == b.peak_extraction_rule_version
            && a.target_accepted_readings == b.target_accepted_readings
            && a.maximum_rejected_attempts == b.maximum_rejected_attempts;
    }

    bool same_electronic_capture(const CaptureSettings& a, const CaptureSettings& b)
    {
        return bits_equal(a.sample_rate_hz, b.sample_rate_hz)
            && a.pretrigger_sample_count == b.pretrigger_sample_count
            && a.posttrigger_sample_count == b.posttrigger_sample_count
            && a.peak_extraction_rule == b.peak_extraction_rule
            && a.peak_extraction_rule_version == b.peak_extraction_rule_version;
    }

    bool same_grid(const ScanGrid& a, const ScanGrid& b)
    {
        return a.position_count == b.position_count && bits_equal(a.start_z_m, b.start_z_m)
            && bits_equal(a.end_z_m, b.end_z_m) && bits_equal(a.spacing_m, b.spacing_m);
    }

    template <typename T>
    const T* by_id(const std::vector<T>& records, std::string_view id, auto&& identify)
    {
        auto found = std::find_if(
            records.begin(), records.end(), [&](const T& item) { return identify(item) == id; });
        return found == records.end() ? nullptr : &*found;
    }

    template <typename T>
    Result<T> load_record(
        const std::filesystem::path& root, const FileIdentity& file, auto&& decoder)
    {
        if (!file.sha256)
            return refuse<T>(
                file.path.to_string(), "unsealed raw input cannot be used in production");
        auto data = storage::read_sealed_file(root, file.path, { file.byte_size, *file.sha256 });
        if (!data)
            return std::unexpected(data.error());
        return decoder(*data);
    }

    std::chrono::sys_time<std::chrono::nanoseconds> as_time(UtcTimestamp value)
    {
        return std::chrono::time_point_cast<std::chrono::nanoseconds>(value.seconds)
            + std::chrono::nanoseconds(value.nanosecond);
    }

    long double distance(std::chrono::sys_time<std::chrono::nanoseconds> a,
        std::chrono::sys_time<std::chrono::nanoseconds> b)
    {
        const auto lhs = std::chrono::duration<long double>(a.time_since_epoch()).count();
        const auto rhs = std::chrono::duration<long double>(b.time_since_epoch()).count();
        return std::abs(lhs - rhs);
    }

} // namespace

Result<ValidatedSession> load_production_session(const std::filesystem::path& root)
{
    auto manifest = storage::read_session_manifest(root);
    if (!manifest)
        return std::unexpected(manifest.error());
    ValidatedSession session { std::move(*manifest) };
    auto sample_bytes
        = load_record<std::vector<std::uint8_t>>(root, session.manifest.sample_snapshot,
            [](const auto& data) -> Result<std::vector<std::uint8_t>> { return data; });
    if (!sample_bytes)
        return std::unexpected(sample_bytes.error());
    auto sample = toml_io::parse_sample(std::string(sample_bytes->begin(), sample_bytes->end()));
    if (!sample)
        return std::unexpected(sample.error());
    session.sample = std::move(*sample);
    if (auto result = validate(session.sample); !result)
        return std::unexpected(result.error());
    auto instrument_bytes
        = load_record<std::vector<std::uint8_t>>(root, session.manifest.instrument_snapshot,
            [](const auto& data) -> Result<std::vector<std::uint8_t>> { return data; });
    if (!instrument_bytes)
        return std::unexpected(instrument_bytes.error());
    auto instrument = toml_io::parse_instrument(
        std::string(instrument_bytes->begin(), instrument_bytes->end()));
    if (!instrument)
        return std::unexpected(instrument.error());
    session.instrument = std::move(*instrument);
    if (auto result = validate(session.instrument); !result)
        return std::unexpected(result.error());

    std::set<std::string> uuids { session.manifest.session_id.to_string(),
        session.instrument.snapshot_id.to_string() };
    if (uuids.size() != 2)
        return refuse<ValidatedSession>("snapshot_id", "instrument and session UUIDs must differ");
    std::set<std::string> paths;
    for (const auto& file :
        { session.manifest.sample_snapshot, session.manifest.instrument_snapshot })
        if (!paths.insert(file.path.to_string()).second)
            return refuse<ValidatedSession>("input_inventory", "duplicate input path");
    const auto inventory = [&](const Uuid& id, const FileIdentity& file) -> Result<void> {
        if (!uuids.insert(id.to_string()).second)
            return refuse<void>("input_inventory", "duplicate generated ID");
        if (!paths.insert(file.path.to_string()).second)
            return refuse<void>("input_inventory", "duplicate input path");
        return { };
    };

    for (const auto& config : session.instrument.scan_configurations) {
        if (!by_id(session.instrument.daq_configurations, config.daq_configuration_id,
                [](const auto& d) { return d.configuration_id; })
            || !by_id(session.instrument.optical_configurations, config.optical_configuration_id,
                [](const auto& o) { return o.configuration_id; })
            || !by_id(session.instrument.stage_configurations, config.stage_configuration_id,
                [](const auto& s) { return s.configuration_id; }))
            return refuse<ValidatedSession>(
                "instrument.scan_configurations", "dangling instrument configuration");
    }
    for (const auto& optical : session.instrument.optical_configurations) {
        for (const auto& id : optical.reference_filter_ids)
            if (!by_id(session.instrument.optical_filters, id,
                    [](const auto& filter) { return filter.filter_id; }))
                return refuse<ValidatedSession>(
                    "instrument.optical_filters", "unknown reference OD filter");
        for (const auto& id : optical.signal_filter_ids)
            if (!by_id(session.instrument.optical_filters, id,
                    [](const auto& filter) { return filter.filter_id; }))
                return refuse<ValidatedSession>(
                    "instrument.optical_filters", "unknown signal OD filter");
    }
    const auto unique_catalog = [&](const auto& records, auto&& id) -> Result<void> {
        std::set<std::string> seen;
        for (const auto& record : records)
            if (!seen.insert(id(record)).second)
                return refuse<void>("instrument", "duplicate catalog ID");
        return { };
    };
    if (auto r
        = unique_catalog(session.instrument.cuvettes, [](const auto& c) { return c.cuvette_id; });
        !r)
        return std::unexpected(r.error());
    if (auto r = unique_catalog(
            session.instrument.cuvette_pairs, [](const auto& c) { return c.calibration_id; });
        !r)
        return std::unexpected(r.error());
    if (auto r = unique_catalog(session.instrument.daq_configurations,
            [](const auto& c) { return c.configuration_id; });
        !r)
        return std::unexpected(r.error());
    if (auto r = unique_catalog(
            session.instrument.optical_filters, [](const auto& c) { return c.filter_id; });
        !r)
        return std::unexpected(r.error());
    if (auto r = unique_catalog(session.instrument.optical_configurations,
            [](const auto& c) { return c.configuration_id; });
        !r)
        return std::unexpected(r.error());
    if (auto r = unique_catalog(session.instrument.stage_configurations,
            [](const auto& c) { return c.configuration_id; });
        !r)
        return std::unexpected(r.error());
    if (auto r = unique_catalog(session.instrument.scan_configurations,
            [](const auto& c) { return c.configuration_id; });
        !r)
        return std::unexpected(r.error());
    for (const auto& pair : session.instrument.cuvette_pairs) {
        if (!by_id(session.instrument.cuvettes, pair.sample_cuvette_id,
                [](const auto& c) { return c.cuvette_id; })
            || !by_id(session.instrument.cuvettes, pair.blank_cuvette_id,
                [](const auto& c) { return c.cuvette_id; })
            || !by_id(session.instrument.scan_configurations, pair.scan_configuration_id,
                [](const auto& c) { return c.configuration_id; }))
            return refuse<ValidatedSession>(
                "instrument.cuvette_pairs", "unresolved pair role or configuration");
    }

    for (const auto& record : session.manifest.scans) {
        if (auto r = inventory(record.acquisition_id, record.file); !r)
            return std::unexpected(r.error());
        if (record.state == AcquisitionState::InProgress)
            return refuse<ValidatedSession>(
                "scans.state", "recover an in-progress scan before production workup");
        auto scan = load_record<binary::ScanFile>(
            root, record.file, [](const auto& data) { return binary::decode_scan(data); });
        if (!scan)
            return std::unexpected(scan.error());
        if (scan->header.record_id != record.acquisition_id
            || scan->header.session_id != session.manifest.session_id
            || scan->header.timestamp != record.acquired_at || scan->config.kind != record.kind
            || !same_grid(scan->config.grid, record.grid)
            || !same_capture(scan->config.capture, record.capture)
            || (scan->end.result == binary::ScanResult::Complete)
                != (record.state == AcquisitionState::Complete))
            return refuse<ValidatedSession>(
                "scans", "scan header, grid, capture or state disagrees with manifest");
        const auto* configuration = by_id(session.instrument.scan_configurations,
            record.scan_configuration_id, [](const auto& c) { return c.configuration_id; });
        if (!configuration)
            return refuse<ValidatedSession>(
                "scans.scan_configuration_id", "unknown scan configuration");
        const auto* stage
            = by_id(session.instrument.stage_configurations, configuration->stage_configuration_id,
                [](const auto& c) { return c.configuration_id; });
        const auto* daq = by_id(session.instrument.daq_configurations,
            configuration->daq_configuration_id, [](const auto& c) { return c.configuration_id; });
        if (!stage || !daq
            || !bits_equal(scan->config.maximum_readback_error_m, stage->maximum_readback_error_m)
            || !bits_equal(scan->config.ai0_minimum_v, daq->channels[0].minimum_v)
            || !bits_equal(scan->config.ai0_maximum_v, daq->channels[0].maximum_v)
            || !bits_equal(scan->config.ai1_minimum_v, daq->channels[1].minimum_v)
            || !bits_equal(scan->config.ai1_maximum_v, daq->channels[1].maximum_v))
            return refuse<ValidatedSession>(
                "scans.capture", "scan electronic or stage settings differ from snapshot");
        session.scans.push_back(std::move(*scan));
    }
    for (const auto& record : session.manifest.darks) {
        if (auto r = inventory(record.record_id, record.file); !r)
            return std::unexpected(r.error());
        if (record.state == AcquisitionState::InProgress)
            return refuse<ValidatedSession>(
                "darks.state", "recover in-progress dark before workup");
        auto dark = load_record<binary::DarkFile>(
            root, record.file, [](const auto& data) { return binary::decode_dark(data); });
        if (!dark)
            return std::unexpected(dark.error());
        if (dark->header.record_id != record.record_id
            || dark->header.session_id != session.manifest.session_id
            || dark->header.timestamp != record.acquired_at
            || !same_capture(dark->config.capture, record.capture)
            || (dark->end.result == binary::DarkResult::Complete)
                != (record.state == AcquisitionState::Complete))
            return refuse<ValidatedSession>(
                "darks", "dark header, capture or state disagrees with manifest");
        const auto* daq = by_id(session.instrument.daq_configurations, record.daq_configuration_id,
            [](const auto& c) { return c.configuration_id; });
        if (!daq || !bits_equal(dark->config.ai0_minimum_v, daq->channels[0].minimum_v)
            || !bits_equal(dark->config.ai0_maximum_v, daq->channels[0].maximum_v)
            || !bits_equal(dark->config.ai1_minimum_v, daq->channels[1].minimum_v)
            || !bits_equal(dark->config.ai1_maximum_v, daq->channels[1].maximum_v))
            return refuse<ValidatedSession>(
                "darks.daq_configuration_id", "dark electronic settings differ from snapshot");
        session.darks.push_back(std::move(*dark));
    }
    for (const auto& record : session.manifest.energy_bursts) {
        if (auto r = inventory(record.burst_id, record.file); !r)
            return std::unexpected(r.error());
        auto energy = load_record<binary::EnergyFile>(
            root, record.file, [](const auto& data) { return binary::decode_energy(data); });
        if (!energy)
            return std::unexpected(energy.error());
        if (energy->header.record_id != record.burst_id
            || energy->header.session_id != session.manifest.session_id
            || energy->header.timestamp != record.entered_at
            || energy->readings_j.size() != record.reading_count
            || record.energy_meter_calibration_id != session.instrument.energy_meter_calibration_id)
            return refuse<ValidatedSession>(
                "energy_bursts", "energy burst or meter calibration mismatch");
        session.energy_bursts.push_back(std::move(*energy));
    }
    for (const auto& record : session.manifest.energy_bursts) {
        std::set<std::string> visited { record.burst_id.to_string() };
        auto parent = record.revision_of;
        while (parent) {
            if (!visited.insert(parent->to_string()).second)
                return refuse<ValidatedSession>(
                    "energy_bursts.revision_of", "cyclic energy revision chain");
            const auto* earlier = by_id(session.manifest.energy_bursts, parent->to_string(),
                [](const auto& burst) { return burst.burst_id.to_string(); });
            if (!earlier)
                return refuse<ValidatedSession>(
                    "energy_bursts.revision_of", "dangling energy revision ID");
            parent = earlier->revision_of;
        }
    }
    for (const auto& record : session.manifest.derived_runs)
        if (auto r = inventory(record.run_id, record.run_manifest); !r)
            return std::unexpected(r.error());
    std::set<std::string> curve_ids;
    std::optional<std::string> session_pair_id;
    for (const auto& curve : session.manifest.curves) {
        if (session_pair_id && *session_pair_id != curve.cuvette_pair_calibration_id)
            return refuse<ValidatedSession>("curves.cuvette_pair_calibration_id",
                "one session must use the same fixed-role cuvette pair across curves");
        session_pair_id = curve.cuvette_pair_calibration_id;
        if (!uuids.insert(curve.curve_id.to_string()).second
            || !curve_ids.insert(curve.curve_id.to_string()).second)
            return refuse<ValidatedSession>("curves.curve_id", "duplicate curve ID");
        const auto scan_index = [&](const Uuid& id) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < session.manifest.scans.size(); ++i)
                if (session.manifest.scans[i].acquisition_id == id)
                    return i;
            return { };
        };
        const auto sample_index = scan_index(curve.sample_acquisition_id);
        const auto blank_index = scan_index(curve.blank_acquisition_id);
        if (!sample_index || !blank_index)
            return refuse<ValidatedSession>("curves", "dangling sample or blank acquisition");
        const auto& sample_record = session.manifest.scans[*sample_index];
        const auto& blank_record = session.manifest.scans[*blank_index];
        const auto& sample_scan = session.scans[*sample_index];
        const auto& blank_scan = session.scans[*blank_index];
        if (sample_record.state != AcquisitionState::Complete
            || blank_record.state != AcquisitionState::Complete
            || sample_record.kind != ScanKind::Sample || blank_record.kind != ScanKind::Blank
            || sample_record.sample_id != session.sample.sample_id
            || sample_record.solvent_id != session.sample.solvent_id
            || sample_record.solvent_id != blank_record.solvent_id
            || sample_record.scan_configuration_id != blank_record.scan_configuration_id
            || !same_capture(sample_scan.config.capture, blank_scan.config.capture)
            || sample_scan.config.commanded_positions_m.size()
                != blank_scan.config.commanded_positions_m.size())
            return refuse<ValidatedSession>("curves", "unmatched or incomplete sample/blank pair");
        for (std::size_t i = 0; i < sample_scan.config.commanded_positions_m.size(); ++i)
            if (!bits_equal(sample_scan.config.commanded_positions_m[i],
                    blank_scan.config.commanded_positions_m[i]))
                return refuse<ValidatedSession>(
                    "curves.grid", "sample/blank commanded grids differ");
        const auto* pair = by_id(session.instrument.cuvette_pairs,
            curve.cuvette_pair_calibration_id, [](const auto& p) { return p.calibration_id; });
        if (!pair || pair->sample_cuvette_id != sample_record.cuvette_id
            || pair->blank_cuvette_id != blank_record.cuvette_id
            || pair->scan_configuration_id != sample_record.scan_configuration_id)
            return refuse<ValidatedSession>(
                "curves.cuvette_pair_calibration_id", "fixed cuvette pair roles mismatch");
        const auto pair_index
            = static_cast<std::size_t>(pair - session.instrument.cuvette_pairs.data());
        std::optional<std::size_t> energy_index;
        for (std::size_t i = 0; i < session.manifest.energy_bursts.size(); ++i)
            if (session.manifest.energy_bursts[i].burst_id == curve.energy_burst_id)
                energy_index = i;
        if (!energy_index)
            return refuse<ValidatedSession>("curves.energy_burst_id", "missing shared meter burst");
        const auto* config = by_id(session.instrument.scan_configurations,
            sample_record.scan_configuration_id, [](const auto& c) { return c.configuration_id; });
        const auto midpoint = as_time(blank_record.acquired_at)
            + (as_time(sample_record.acquired_at) - as_time(blank_record.acquired_at)) / 2;
        std::optional<std::size_t> selected;
        for (std::size_t i = 0; i < session.manifest.darks.size(); ++i) {
            const auto& record = session.manifest.darks[i];
            if (record.state != AcquisitionState::Complete
                || record.daq_configuration_id != config->daq_configuration_id
                || !same_electronic_capture(record.capture, sample_record.capture))
                continue;
            if (!record.conditions.laser_running || !record.conditions.beam_blocked
                || !record.conditions.reference_detector_capped
                || !record.conditions.signal_detector_capped)
                continue;
            const auto candidate = as_time(record.acquired_at);
            if (!selected
                || distance(candidate, midpoint)
                    < distance(as_time(session.manifest.darks[*selected].acquired_at), midpoint)
                || (distance(candidate, midpoint)
                        == distance(
                            as_time(session.manifest.darks[*selected].acquired_at), midpoint)
                    && candidate < as_time(session.manifest.darks[*selected].acquired_at)))
                selected = i;
        }
        if (!selected)
            return refuse<ValidatedSession>(
                "curves.dark", "no compatible completed dark in session");
        const auto entered = as_time(session.manifest.energy_bursts[*energy_index].entered_at);
        const auto dark_time = as_time(session.manifest.darks[*selected].acquired_at);
        const auto blank_time = as_time(blank_record.acquired_at);
        const auto sample_time = as_time(sample_record.acquired_at);
        if (!(entered <= dark_time && dark_time <= blank_time && blank_time <= sample_time))
            session.warnings.push_back({ ErrorCode::InvalidValue,
                "software-action timestamps differ from guided energy → dark → blank → sample "
                "order; not a production refusal",
                { }, curve.curve_id.to_string(), "workflow" });
        session.curves.push_back(
            { *sample_index, *blank_index, *energy_index, *selected, pair_index });
    }
    return session;
}

} // namespace zscan::schema
