#include "zscan/schema/toml_io.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace zscan::schema::toml_io {
namespace {

    [[noreturn]] void reject(ErrorCode code, std::string location, std::string message)
    {
        throw SchemaError { code, std::move(message), { }, { }, std::move(location) };
    }

    template <typename T> T take(Result<T> result)
    {
        if (!result)
            throw result.error();
        return std::move(*result);
    }

    void take(Result<void> result)
    {
        if (!result)
            throw result.error();
    }

    void keys(const toml::table& table, std::initializer_list<std::string_view> allowed,
        std::string_view location)
    {
        for (const auto& [key, node] : table) {
            const auto name = key.str();
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                reject(ErrorCode::InvalidValue, std::string(location) + "." + std::string(name),
                    "unknown standard TOML field");
            if (name == "extensions" && !node.is_table())
                reject(ErrorCode::InvalidValue, std::string(location) + ".extensions",
                    "extensions must be a TOML table");
        }
    }

    const toml::table& table_at(const toml::table& parent, std::string_view name)
    {
        const auto* result = parent.get(name) ? parent.get(name)->as_table() : nullptr;
        if (!result)
            reject(ErrorCode::MissingField, std::string(name), "missing required TOML table");
        return *result;
    }

    std::string string_at(const toml::table& table, std::string_view name)
    {
        const auto node = table[name];
        if (auto value = node.value<std::string>())
            return *value;
        reject(ErrorCode::MissingField, std::string(name), "missing required TOML string");
    }

    std::int64_t integer_at(const toml::table& table, std::string_view name)
    {
        const auto node = table[name];
        if (auto value = node.value<std::int64_t>())
            return *value;
        reject(ErrorCode::MissingField, std::string(name), "missing required TOML integer");
    }

    double double_at(const toml::table& table, std::string_view name)
    {
        const auto node = table[name];
        if (auto value = node.value<double>())
            return *value;
        reject(ErrorCode::MissingField, std::string(name), "missing required TOML number");
    }

    bool bool_at(const toml::table& table, std::string_view name)
    {
        if (auto value = table[name].value<bool>())
            return *value;
        reject(ErrorCode::MissingField, std::string(name), "missing required TOML boolean");
    }

    std::uint32_t u32_at(const toml::table& table, std::string_view name)
    {
        const auto number = integer_at(table, name);
        if (number < 0 || number > UINT32_MAX)
            reject(ErrorCode::InvalidValue, std::string(name), "integer exceeds uint32 range");
        return static_cast<std::uint32_t>(number);
    }

    std::uint64_t u64_at(const toml::table& table, std::string_view name)
    {
        const auto number = integer_at(table, name);
        if (number < 0)
            reject(ErrorCode::InvalidValue, std::string(name), "negative byte size or revision");
        return static_cast<std::uint64_t>(number);
    }

    std::uint16_t u16_at(const toml::table& table, std::string_view name)
    {
        const auto number = integer_at(table, name);
        if (number < 0 || number > UINT16_MAX)
            reject(ErrorCode::InvalidValue, std::string(name), "integer exceeds uint16 range");
        return static_cast<std::uint16_t>(number);
    }

    UtcTimestamp time_at(const toml::table& table, std::string_view name)
    {
        return take(UtcTimestamp::parse(string_at(table, name)));
    }

    Uuid uuid_at(const toml::table& table, std::string_view name)
    {
        return take(Uuid::parse(string_at(table, name)));
    }

    void schema_version(const toml::table& table)
    {
        if (integer_at(table, "schema_version") != 1)
            reject(
                ErrorCode::UnsupportedVersion, "schema_version", "expected TOML schema version 1");
    }

    toml::table parse_document(std::string_view text)
    {
        try {
            return toml::parse(text);
        } catch (const toml::parse_error& error) {
            reject(ErrorCode::InvalidValue, "toml", std::string(error.description()));
        }
    }

    template <typename T, typename F> Result<T> parse_guard(F&& function)
    {
        try {
            return function();
        } catch (const SchemaError& error) {
            return std::unexpected(error);
        } catch (const toml::parse_error& error) {
            return std::unexpected(SchemaError {
                ErrorCode::InvalidValue, std::string(error.description()), { }, { }, "toml" });
        }
    }

    template <typename F> Result<std::string> format_guard(F&& function)
    {
        try {
            auto result = function();
            std::ostringstream output;
            output << result;
            return output.str();
        } catch (const SchemaError& error) {
            return std::unexpected(error);
        }
    }

    ScalarWithUncertainty scalar_at(const toml::table& parent, std::string_view key)
    {
        const auto& table = table_at(parent, key);
        keys(table,
            { "value", "standard_uncertainty", "provenance_id", "uncertainty_evaluation_id",
                "conversion" },
            key);
        ScalarWithUncertainty scalar { double_at(table, "value"),
            double_at(table, "standard_uncertainty"), string_at(table, "provenance_id"),
            string_at(table, "uncertainty_evaluation_id") };
        if (table.contains("conversion")) {
            const auto& conversion = table_at(table, "conversion");
            keys(
                conversion, { "method", "source_value", "divisor", "provenance_id" }, "conversion");
            scalar.conversion = ScalarWithUncertainty::Conversion { string_at(conversion, "method"),
                double_at(conversion, "source_value"), double_at(conversion, "divisor"),
                string_at(conversion, "provenance_id") };
        }
        return scalar;
    }

    void check_input_size(std::uint64_t size, std::string_view location);

    toml::table scalar_table(const ScalarWithUncertainty& scalar)
    {
        toml::table table;
        table.insert("value", scalar.value);
        table.insert("standard_uncertainty", scalar.standard_uncertainty);
        table.insert("provenance_id", scalar.provenance_id);
        table.insert("uncertainty_evaluation_id", scalar.uncertainty_evaluation_id);
        if (scalar.conversion) {
            toml::table conversion;
            conversion.insert("method", scalar.conversion->method);
            conversion.insert("source_value", scalar.conversion->source_value);
            conversion.insert("divisor", scalar.conversion->divisor);
            conversion.insert("provenance_id", scalar.conversion->provenance_id);
            table.insert("conversion", std::move(conversion));
        }
        return table;
    }

    FileIdentity file_at(const toml::table& table)
    {
        keys(table, { "path", "byte_size", "sha256" }, "file");
        FileIdentity identity { take(SessionPath::parse(string_at(table, "path"))),
            u64_at(table, "byte_size"), { } };
        if (table.contains("sha256"))
            identity.sha256 = take(Sha256Digest::parse(string_at(table, "sha256")));
        return identity;
    }

    toml::table file_table(const FileIdentity& file)
    {
        toml::table table;
        table.insert("path", file.path.to_string());
        check_input_size(file.byte_size, "file.byte_size");
        table.insert("byte_size", static_cast<std::int64_t>(file.byte_size));
        if (file.sha256)
            table.insert("sha256", file.sha256->to_string());
        return table;
    }

    CaptureSettings capture_at(const toml::table& table)
    {
        keys(table,
            { "sample_rate_hz", "pretrigger_sample_count", "posttrigger_sample_count",
                "peak_extraction_rule", "peak_extraction_rule_version", "target_accepted_readings",
                "maximum_rejected_attempts" },
            "capture");
        return { double_at(table, "sample_rate_hz"), u32_at(table, "pretrigger_sample_count"),
            u32_at(table, "posttrigger_sample_count"), u16_at(table, "peak_extraction_rule"),
            u16_at(table, "peak_extraction_rule_version"),
            u32_at(table, "target_accepted_readings"), u32_at(table, "maximum_rejected_attempts") };
    }

    toml::table capture_table(const CaptureSettings& capture)
    {
        toml::table table;
        table.insert("sample_rate_hz", capture.sample_rate_hz);
        table.insert("pretrigger_sample_count", capture.pretrigger_sample_count);
        table.insert("posttrigger_sample_count", capture.posttrigger_sample_count);
        table.insert("peak_extraction_rule", capture.peak_extraction_rule);
        table.insert("peak_extraction_rule_version", capture.peak_extraction_rule_version);
        table.insert("target_accepted_readings", capture.target_accepted_readings);
        table.insert("maximum_rejected_attempts", capture.maximum_rejected_attempts);
        return table;
    }

    template <typename T> std::string enum_text(T value);

    template <> std::string enum_text(ScanKind value)
    {
        return value == ScanKind::Sample ? "sample" : "blank";
    }

    template <> std::string enum_text(AcquisitionState value)
    {
        switch (value) {
        case AcquisitionState::InProgress:
            return "in_progress";
        case AcquisitionState::Complete:
            return "complete";
        case AcquisitionState::Incomplete:
            return "incomplete";
        case AcquisitionState::Interrupted:
            return "interrupted";
        }
        reject(ErrorCode::InvalidValue, "state", "invalid lifecycle state");
    }

    AcquisitionState state_at(const toml::table& table)
    {
        const auto value = string_at(table, "state");
        if (value == "in_progress")
            return AcquisitionState::InProgress;
        if (value == "complete")
            return AcquisitionState::Complete;
        if (value == "incomplete")
            return AcquisitionState::Incomplete;
        if (value == "interrupted")
            return AcquisitionState::Interrupted;
        reject(ErrorCode::InvalidValue, "state", "unknown lifecycle state");
    }

    ScanKind scan_kind_at(const toml::table& table)
    {
        const auto value = string_at(table, "kind");
        if (value == "blank")
            return ScanKind::Blank;
        if (value == "sample")
            return ScanKind::Sample;
        reject(ErrorCode::InvalidValue, "kind", "unknown scan kind");
    }

    const toml::array& array_at(const toml::table& table, std::string_view name)
    {
        const auto* result = table.get(name) ? table.get(name)->as_array() : nullptr;
        if (!result)
            reject(ErrorCode::MissingField, std::string(name), "missing required TOML array");
        return *result;
    }

    template <typename F>
    void each_table(const toml::table& source, std::string_view name, F&& process)
    {
        for (const auto& node : array_at(source, name)) {
            const auto* table = node.as_table();
            if (!table)
                reject(ErrorCode::InvalidValue, std::string(name), "expected table array");
            process(*table);
        }
    }

    void check_input_size(std::uint64_t size, std::string_view location)
    {
        if (size > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            reject(
                ErrorCode::InvalidValue, std::string(location), "TOML integer exceeds int64 range");
    }

    std::string extensions_at(const toml::table& root)
    {
        if (!root.contains("extensions"))
            return { };
        std::ostringstream output;
        output << table_at(root, "extensions");
        return output.str();
    }

    void insert_extensions(toml::table& root, const std::string& text)
    {
        if (!text.empty())
            root.insert("extensions", parse_document(text));
    }

} // namespace

Result<SampleSnapshot> parse_sample(std::string_view text)
{
    return parse_guard<SampleSnapshot>([&] {
        const auto root = parse_document(text);
        keys(root,
            { "schema_version", "sample_id", "solvent_id", "created_at", "concentration_mol_per_l",
                "solvent_refractive_index", "extensions" },
            "sample");
        schema_version(root);
        SampleSnapshot sample { string_at(root, "sample_id"), string_at(root, "solvent_id"),
            scalar_at(root, "concentration_mol_per_l"), scalar_at(root, "solvent_refractive_index"),
            time_at(root, "created_at"), extensions_at(root) };
        take(validate(sample, ValidationLevel::Draft));
        return sample;
    });
}

Result<std::string> format_sample(const SampleSnapshot& sample)
{
    return format_guard([&] {
        take(validate(sample, ValidationLevel::Draft));
        toml::table root;
        root.insert("schema_version", 1);
        root.insert("sample_id", sample.sample_id);
        root.insert("solvent_id", sample.solvent_id);
        root.insert("created_at", sample.created_at.to_string());
        root.insert("concentration_mol_per_l", scalar_table(sample.concentration_mol_per_l));
        root.insert("solvent_refractive_index", scalar_table(sample.solvent_refractive_index));
        insert_extensions(root, sample.extensions_toml);
        return root;
    });
}

Result<InstrumentSnapshot> parse_instrument(std::string_view text)
{
    return parse_guard<InstrumentSnapshot>([&] {
        const auto root = parse_document(text);
        keys(root,
            { "schema_version", "snapshot_id", "created_at", "source_catalog_id",
                "source_catalog_revision", "laser_characterization_id", "wavelength_m",
                "pulse_fwhm_s", "waist_m", "m2", "energy_meter_calibration_id",
                "energy_meter_standard_uncertainty_j", "energy_meter_uncertainty_provenance_id",
                "entrance_window_refractive_index", "cuvettes", "cuvette_pairs",
                "daq_configurations", "optical_filters", "optical_configurations",
                "stage_configurations", "scan_configurations", "extensions" },
            "instrument");
        schema_version(root);
        InstrumentSnapshot instrument;
        instrument.snapshot_id = uuid_at(root, "snapshot_id");
        instrument.created_at = time_at(root, "created_at");
        instrument.extensions_toml = extensions_at(root);
        instrument.source_catalog_id = string_at(root, "source_catalog_id");
        instrument.source_catalog_revision = string_at(root, "source_catalog_revision");
        instrument.laser_characterization_id = string_at(root, "laser_characterization_id");
        instrument.wavelength_m = double_at(root, "wavelength_m");
        instrument.pulse_fwhm_s = double_at(root, "pulse_fwhm_s");
        instrument.waist_m = scalar_at(root, "waist_m");
        instrument.m2 = scalar_at(root, "m2");
        instrument.energy_meter_calibration_id = string_at(root, "energy_meter_calibration_id");
        instrument.energy_meter_standard_uncertainty_j
            = double_at(root, "energy_meter_standard_uncertainty_j");
        instrument.energy_meter_uncertainty_provenance_id
            = string_at(root, "energy_meter_uncertainty_provenance_id");
        instrument.entrance_window_refractive_index
            = scalar_at(root, "entrance_window_refractive_index");
        each_table(root, "cuvettes", [&](const toml::table& table) {
            keys(table, { "cuvette_id", "path_length_m" }, "cuvettes");
            instrument.cuvettes.push_back(
                { string_at(table, "cuvette_id"), scalar_at(table, "path_length_m") });
        });
        each_table(root, "cuvette_pairs", [&](const toml::table& table) {
            keys(table,
                { "calibration_id", "sample_cuvette_id", "blank_cuvette_id",
                    "scan_configuration_id", "correction_factor" },
                "cuvette_pairs");
            instrument.cuvette_pairs.push_back({ string_at(table, "calibration_id"),
                string_at(table, "sample_cuvette_id"), string_at(table, "blank_cuvette_id"),
                string_at(table, "scan_configuration_id"), scalar_at(table, "correction_factor") });
        });
        each_table(root, "daq_configurations", [&](const toml::table& table) {
            keys(table,
                { "configuration_id", "device_id", "trigger_terminal_index", "trigger_edge",
                    "channels" },
                "daq_configurations");
            DaqConfiguration config;
            config.configuration_id = string_at(table, "configuration_id");
            config.device_id = string_at(table, "device_id");
            config.trigger_terminal_index = u32_at(table, "trigger_terminal_index");
            const auto edge = string_at(table, "trigger_edge");
            if (edge != "rising" && edge != "falling")
                reject(ErrorCode::InvalidValue, "trigger_edge", "unknown trigger edge");
            config.trigger_edge = edge == "rising" ? TriggerEdge::Rising : TriggerEdge::Falling;
            const auto& channels = array_at(table, "channels");
            if (channels.size() != 2)
                reject(ErrorCode::InvalidValue, "channels",
                    "DAQ needs exactly two role-specific channels");
            for (std::size_t i = 0; i < 2; ++i) {
                const auto* channel = channels[i].as_table();
                if (!channel)
                    reject(ErrorCode::InvalidValue, "channels", "DAQ channel must be a table");
                keys(*channel,
                    { "channel_index", "role", "terminal_configuration", "coupling", "minimum_v",
                        "maximum_v" },
                    "channels");
                auto& out = config.channels[i];
                out.channel_index = u32_at(*channel, "channel_index");
                const auto role = string_at(*channel, "role");
                if (role != "reference" && role != "signal")
                    reject(ErrorCode::InvalidValue, "role", "unknown detector role");
                out.role = role == "reference" ? DetectorRole::Reference : DetectorRole::Signal;
                const auto terminal = string_at(*channel, "terminal_configuration");
                if (terminal == "rse")
                    out.terminal_configuration = TerminalConfiguration::Rse;
                else if (terminal == "nrse")
                    out.terminal_configuration = TerminalConfiguration::Nrse;
                else if (terminal == "differential")
                    out.terminal_configuration = TerminalConfiguration::Differential;
                else if (terminal == "pseudo_differential")
                    out.terminal_configuration = TerminalConfiguration::PseudoDifferential;
                else
                    reject(ErrorCode::InvalidValue, "terminal_configuration",
                        "unknown DAQ terminal mode");
                const auto coupling = string_at(*channel, "coupling");
                if (coupling != "dc" && coupling != "ac")
                    reject(ErrorCode::InvalidValue, "coupling", "unknown DAQ coupling");
                out.coupling = coupling == "dc" ? Coupling::Dc : Coupling::Ac;
                out.minimum_v = double_at(*channel, "minimum_v");
                out.maximum_v = double_at(*channel, "maximum_v");
            }
            instrument.daq_configurations.push_back(std::move(config));
        });
        each_table(root, "optical_filters", [&](const toml::table& table) {
            keys(table, { "filter_id", "nominal_optical_density", "wavelength_m", "provenance_id" },
                "optical_filters");
            instrument.optical_filters.push_back(
                { string_at(table, "filter_id"), double_at(table, "nominal_optical_density"),
                    double_at(table, "wavelength_m"), string_at(table, "provenance_id") });
        });
        const auto string_array = [](const toml::table& table, std::string_view name) {
            std::vector<std::string> values;
            for (const auto& node : array_at(table, name)) {
                const auto* text = node.as_string();
                if (!text)
                    reject(ErrorCode::InvalidValue, std::string(name), "expected string array");
                values.push_back(std::string(text->get()));
            }
            return values;
        };
        each_table(root, "optical_configurations", [&](const toml::table& table) {
            keys(table, { "configuration_id", "reference_filter_ids", "signal_filter_ids" },
                "optical_configurations");
            instrument.optical_configurations.push_back(
                { string_at(table, "configuration_id"), string_array(table, "reference_filter_ids"),
                    string_array(table, "signal_filter_ids") });
        });
        each_table(root, "stage_configurations", [&](const toml::table& table) {
            keys(table,
                { "configuration_id", "controller_id", "axis", "maximum_readback_error_m",
                    "tolerance_provenance_id", "maximum_move_attempts" },
                "stage_configurations");
            const auto attempts = u32_at(table, "maximum_move_attempts");
            if (attempts > UINT8_MAX)
                reject(ErrorCode::InvalidValue, "maximum_move_attempts", "move count too large");
            instrument.stage_configurations.push_back({ string_at(table, "configuration_id"),
                string_at(table, "controller_id"), u32_at(table, "axis"),
                double_at(table, "maximum_readback_error_m"),
                string_at(table, "tolerance_provenance_id"), static_cast<std::uint8_t>(attempts) });
        });
        each_table(root, "scan_configurations", [&](const toml::table& table) {
            keys(table,
                { "configuration_id", "daq_configuration_id", "optical_configuration_id",
                    "stage_configuration_id" },
                "scan_configurations");
            instrument.scan_configurations.push_back(
                { string_at(table, "configuration_id"), string_at(table, "daq_configuration_id"),
                    string_at(table, "optical_configuration_id"),
                    string_at(table, "stage_configuration_id") });
        });
        take(validate(instrument, ValidationLevel::Draft));
        return instrument;
    });
}

Result<std::string> format_instrument(const InstrumentSnapshot& instrument)
{
    return format_guard([&] {
        take(validate(instrument, ValidationLevel::Draft));
        toml::table root;
        root.insert("schema_version", 1);
        root.insert("snapshot_id", instrument.snapshot_id.to_string());
        root.insert("created_at", instrument.created_at.to_string());
        root.insert("source_catalog_id", instrument.source_catalog_id);
        root.insert("source_catalog_revision", instrument.source_catalog_revision);
        root.insert("laser_characterization_id", instrument.laser_characterization_id);
        root.insert("wavelength_m", instrument.wavelength_m);
        root.insert("pulse_fwhm_s", instrument.pulse_fwhm_s);
        root.insert("waist_m", scalar_table(instrument.waist_m));
        root.insert("m2", scalar_table(instrument.m2));
        root.insert("energy_meter_calibration_id", instrument.energy_meter_calibration_id);
        root.insert(
            "energy_meter_standard_uncertainty_j", instrument.energy_meter_standard_uncertainty_j);
        root.insert("energy_meter_uncertainty_provenance_id",
            instrument.energy_meter_uncertainty_provenance_id);
        root.insert("entrance_window_refractive_index",
            scalar_table(instrument.entrance_window_refractive_index));
        toml::array cuvettes;
        for (const auto& cuvette : instrument.cuvettes) {
            toml::table table;
            table.insert("cuvette_id", cuvette.cuvette_id);
            table.insert("path_length_m", scalar_table(cuvette.path_length_m));
            cuvettes.push_back(std::move(table));
        }
        root.insert("cuvettes", std::move(cuvettes));
        toml::array pairs;
        for (const auto& pair : instrument.cuvette_pairs) {
            toml::table table;
            table.insert("calibration_id", pair.calibration_id);
            table.insert("sample_cuvette_id", pair.sample_cuvette_id);
            table.insert("blank_cuvette_id", pair.blank_cuvette_id);
            table.insert("scan_configuration_id", pair.scan_configuration_id);
            table.insert("correction_factor", scalar_table(pair.correction_factor));
            pairs.push_back(std::move(table));
        }
        root.insert("cuvette_pairs", std::move(pairs));
        toml::array daqs;
        const auto terminal_text = [](TerminalConfiguration value) -> std::string_view {
            switch (value) {
            case TerminalConfiguration::Rse:
                return "rse";
            case TerminalConfiguration::Nrse:
                return "nrse";
            case TerminalConfiguration::Differential:
                return "differential";
            case TerminalConfiguration::PseudoDifferential:
                return "pseudo_differential";
            }
            reject(ErrorCode::InvalidValue, "terminal_configuration", "invalid terminal mode");
        };
        for (const auto& daq : instrument.daq_configurations) {
            toml::table table;
            table.insert("configuration_id", daq.configuration_id);
            table.insert("device_id", daq.device_id);
            table.insert("trigger_terminal_index", daq.trigger_terminal_index);
            table.insert(
                "trigger_edge", daq.trigger_edge == TriggerEdge::Rising ? "rising" : "falling");
            toml::array channels;
            for (const auto& channel : daq.channels) {
                toml::table entry;
                entry.insert("channel_index", channel.channel_index);
                entry.insert(
                    "role", channel.role == DetectorRole::Reference ? "reference" : "signal");
                entry.insert(
                    "terminal_configuration", terminal_text(channel.terminal_configuration));
                entry.insert("coupling", channel.coupling == Coupling::Dc ? "dc" : "ac");
                entry.insert("minimum_v", channel.minimum_v);
                entry.insert("maximum_v", channel.maximum_v);
                channels.push_back(std::move(entry));
            }
            table.insert("channels", std::move(channels));
            daqs.push_back(std::move(table));
        }
        root.insert("daq_configurations", std::move(daqs));
        toml::array filters;
        for (const auto& filter : instrument.optical_filters) {
            toml::table table;
            table.insert("filter_id", filter.filter_id);
            table.insert("nominal_optical_density", filter.nominal_optical_density);
            table.insert("wavelength_m", filter.wavelength_m);
            table.insert("provenance_id", filter.provenance_id);
            filters.push_back(std::move(table));
        }
        root.insert("optical_filters", std::move(filters));
        const auto to_array = [](const std::vector<std::string>& values) {
            toml::array array;
            for (const auto& value : values)
                array.push_back(value);
            return array;
        };
        toml::array optics;
        for (const auto& optical : instrument.optical_configurations) {
            toml::table table;
            table.insert("configuration_id", optical.configuration_id);
            table.insert("reference_filter_ids", to_array(optical.reference_filter_ids));
            table.insert("signal_filter_ids", to_array(optical.signal_filter_ids));
            optics.push_back(std::move(table));
        }
        root.insert("optical_configurations", std::move(optics));
        toml::array stages;
        for (const auto& stage : instrument.stage_configurations) {
            toml::table table;
            table.insert("configuration_id", stage.configuration_id);
            table.insert("controller_id", stage.controller_id);
            table.insert("axis", stage.axis);
            table.insert("maximum_readback_error_m", stage.maximum_readback_error_m);
            table.insert("tolerance_provenance_id", stage.tolerance_provenance_id);
            table.insert("maximum_move_attempts", stage.maximum_move_attempts);
            stages.push_back(std::move(table));
        }
        root.insert("stage_configurations", std::move(stages));
        toml::array scans;
        for (const auto& scan : instrument.scan_configurations) {
            toml::table table;
            table.insert("configuration_id", scan.configuration_id);
            table.insert("daq_configuration_id", scan.daq_configuration_id);
            table.insert("optical_configuration_id", scan.optical_configuration_id);
            table.insert("stage_configuration_id", scan.stage_configuration_id);
            scans.push_back(std::move(table));
        }
        root.insert("scan_configurations", std::move(scans));
        insert_extensions(root, instrument.extensions_toml);
        return root;
    });
}

Result<SessionManifest> parse_session(std::string_view text)
{
    return parse_guard<SessionManifest>([&] {
        const auto root = parse_document(text);
        keys(root,
            { "schema_version", "session_id", "created_at", "manifest_revision", "sample_snapshot",
                "instrument_snapshot", "scans", "darks", "energy_bursts", "curves", "derived_runs",
                "extensions" },
            "session");
        schema_version(root);
        SessionManifest session { uuid_at(root, "session_id"), time_at(root, "created_at"),
            u64_at(root, "manifest_revision"), file_at(table_at(root, "sample_snapshot")),
            file_at(table_at(root, "instrument_snapshot")) };
        session.extensions_toml = extensions_at(root);
        each_table(root, "scans", [&](const toml::table& table) {
            keys(table,
                { "acquisition_id", "kind", "state", "file", "acquired_at", "sample_id",
                    "solvent_id", "cuvette_id", "scan_configuration_id", "grid", "capture",
                    "valid_prefix_byte_size", "valid_frame_count" },
                "scans");
            ScanRecord scan { uuid_at(table, "acquisition_id"), scan_kind_at(table),
                state_at(table), file_at(table_at(table, "file")), time_at(table, "acquired_at"),
                table.contains("sample_id") ? string_at(table, "sample_id") : std::string { },
                string_at(table, "solvent_id"), string_at(table, "cuvette_id"),
                string_at(table, "scan_configuration_id") };
            const auto& grid = table_at(table, "grid");
            keys(grid, { "start_z_m", "end_z_m", "position_count", "spacing_m" }, "grid");
            scan.grid = { double_at(grid, "start_z_m"), double_at(grid, "end_z_m"),
                u32_at(grid, "position_count"), double_at(grid, "spacing_m") };
            scan.capture = capture_at(table_at(table, "capture"));
            if (table.contains("valid_prefix_byte_size"))
                scan.valid_prefix_byte_size = u64_at(table, "valid_prefix_byte_size");
            if (table.contains("valid_frame_count"))
                scan.valid_frame_count = u64_at(table, "valid_frame_count");
            session.scans.push_back(std::move(scan));
        });
        each_table(root, "darks", [&](const toml::table& table) {
            keys(table,
                { "record_id", "state", "file", "acquired_at", "daq_configuration_id", "conditions",
                    "capture" },
                "darks");
            DarkRecord dark { uuid_at(table, "record_id"), state_at(table),
                file_at(table_at(table, "file")), time_at(table, "acquired_at"),
                string_at(table, "daq_configuration_id") };
            const auto& conditions = table_at(table, "conditions");
            keys(conditions,
                { "laser_running", "beam_blocked", "reference_detector_capped",
                    "signal_detector_capped" },
                "conditions");
            dark.conditions
                = { bool_at(conditions, "laser_running"), bool_at(conditions, "beam_blocked"),
                      bool_at(conditions, "reference_detector_capped"),
                      bool_at(conditions, "signal_detector_capped") };
            dark.capture = capture_at(table_at(table, "capture"));
            session.darks.push_back(std::move(dark));
        });
        each_table(root, "energy_bursts", [&](const toml::table& table) {
            keys(table,
                { "burst_id", "revision_of", "file", "entered_at", "reading_count",
                    "energy_meter_calibration_id" },
                "energy_bursts");
            EnergyBurstRecord burst { uuid_at(table, "burst_id"), { },
                file_at(table_at(table, "file")), time_at(table, "entered_at"),
                u32_at(table, "reading_count"), string_at(table, "energy_meter_calibration_id") };
            if (table.contains("revision_of"))
                burst.revision_of = uuid_at(table, "revision_of");
            session.energy_bursts.push_back(std::move(burst));
        });
        each_table(root, "curves", [&](const toml::table& table) {
            keys(table,
                { "curve_id", "sample_acquisition_id", "blank_acquisition_id", "energy_burst_id",
                    "cuvette_pair_calibration_id" },
                "curves");
            session.curves.push_back(
                { uuid_at(table, "curve_id"), uuid_at(table, "sample_acquisition_id"),
                    uuid_at(table, "blank_acquisition_id"), uuid_at(table, "energy_burst_id"),
                    string_at(table, "cuvette_pair_calibration_id") });
        });
        each_table(root, "derived_runs", [&](const toml::table& table) {
            keys(table, { "run_id", "run_manifest", "created_at" }, "derived_runs");
            session.derived_runs.push_back({ uuid_at(table, "run_id"),
                file_at(table_at(table, "run_manifest")), time_at(table, "created_at") });
        });
        take(validate(session));
        return session;
    });
}

Result<std::string> format_session(const SessionManifest& session)
{
    return format_guard([&] {
        take(validate(session));
        check_input_size(session.manifest_revision, "manifest_revision");
        toml::table root;
        root.insert("schema_version", 1);
        root.insert("session_id", session.session_id.to_string());
        root.insert("created_at", session.created_at.to_string());
        root.insert("manifest_revision", static_cast<std::int64_t>(session.manifest_revision));
        root.insert("sample_snapshot", file_table(session.sample_snapshot));
        root.insert("instrument_snapshot", file_table(session.instrument_snapshot));
        toml::array scans;
        for (const auto& scan : session.scans) {
            toml::table table;
            table.insert("acquisition_id", scan.acquisition_id.to_string());
            table.insert("kind", enum_text(scan.kind));
            table.insert("state", enum_text(scan.state));
            table.insert("file", file_table(scan.file));
            table.insert("acquired_at", scan.acquired_at.to_string());
            if (!scan.sample_id.empty())
                table.insert("sample_id", scan.sample_id);
            table.insert("solvent_id", scan.solvent_id);
            table.insert("cuvette_id", scan.cuvette_id);
            table.insert("scan_configuration_id", scan.scan_configuration_id);
            toml::table grid;
            grid.insert("start_z_m", scan.grid.start_z_m);
            grid.insert("end_z_m", scan.grid.end_z_m);
            grid.insert("position_count", scan.grid.position_count);
            grid.insert("spacing_m", scan.grid.spacing_m);
            table.insert("grid", std::move(grid));
            table.insert("capture", capture_table(scan.capture));
            if (scan.valid_prefix_byte_size) {
                check_input_size(*scan.valid_prefix_byte_size, "valid_prefix_byte_size");
                table.insert("valid_prefix_byte_size",
                    static_cast<std::int64_t>(*scan.valid_prefix_byte_size));
                check_input_size(*scan.valid_frame_count, "valid_frame_count");
                table.insert(
                    "valid_frame_count", static_cast<std::int64_t>(*scan.valid_frame_count));
            }
            scans.push_back(std::move(table));
        }
        root.insert("scans", std::move(scans));
        toml::array darks;
        for (const auto& dark : session.darks) {
            toml::table table;
            table.insert("record_id", dark.record_id.to_string());
            table.insert("state", enum_text(dark.state));
            table.insert("file", file_table(dark.file));
            table.insert("acquired_at", dark.acquired_at.to_string());
            table.insert("daq_configuration_id", dark.daq_configuration_id);
            toml::table conditions;
            conditions.insert("laser_running", dark.conditions.laser_running);
            conditions.insert("beam_blocked", dark.conditions.beam_blocked);
            conditions.insert(
                "reference_detector_capped", dark.conditions.reference_detector_capped);
            conditions.insert("signal_detector_capped", dark.conditions.signal_detector_capped);
            table.insert("conditions", std::move(conditions));
            table.insert("capture", capture_table(dark.capture));
            darks.push_back(std::move(table));
        }
        root.insert("darks", std::move(darks));
        toml::array bursts;
        for (const auto& burst : session.energy_bursts) {
            toml::table table;
            table.insert("burst_id", burst.burst_id.to_string());
            if (burst.revision_of)
                table.insert("revision_of", burst.revision_of->to_string());
            table.insert("file", file_table(burst.file));
            table.insert("entered_at", burst.entered_at.to_string());
            table.insert("reading_count", burst.reading_count);
            table.insert("energy_meter_calibration_id", burst.energy_meter_calibration_id);
            bursts.push_back(std::move(table));
        }
        root.insert("energy_bursts", std::move(bursts));
        toml::array curves;
        for (const auto& curve : session.curves) {
            toml::table table;
            table.insert("curve_id", curve.curve_id.to_string());
            table.insert("sample_acquisition_id", curve.sample_acquisition_id.to_string());
            table.insert("blank_acquisition_id", curve.blank_acquisition_id.to_string());
            table.insert("energy_burst_id", curve.energy_burst_id.to_string());
            table.insert("cuvette_pair_calibration_id", curve.cuvette_pair_calibration_id);
            curves.push_back(std::move(table));
        }
        root.insert("curves", std::move(curves));
        toml::array runs;
        for (const auto& run : session.derived_runs) {
            toml::table table;
            table.insert("run_id", run.run_id.to_string());
            table.insert("run_manifest", file_table(run.run_manifest));
            table.insert("created_at", run.created_at.to_string());
            runs.push_back(std::move(table));
        }
        root.insert("derived_runs", std::move(runs));
        insert_extensions(root, session.extensions_toml);
        return root;
    });
}

} // namespace zscan::schema::toml_io
