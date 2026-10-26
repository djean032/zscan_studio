#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zscan::schema {

enum class ErrorCode {
    InvalidUuid,
    InvalidTimestamp,
    InvalidDigest,
    UnsafePath,
    FilesystemError,
    InvalidValue,
    MissingField,
    InvalidBinary,
    TruncatedBinary,
    ChecksumMismatch,
    UnsupportedVersion,
    ResourceLimit,
    CryptoFailure,
};

struct SchemaError {
    ErrorCode code;
    std::string message;
    std::filesystem::path source_path;
    std::string record_id;
    std::string location; // TOML field or binary frame location, when available.
};

// Attach the source file when an I/O layer has one; value validators do not.
[[nodiscard]] SchemaError with_source(SchemaError diagnostic, std::filesystem::path source_path);

template <typename T> using Result = std::expected<T, SchemaError>;

struct Uuid {
    std::array<std::uint8_t, 16> bytes { };
    friend bool operator==(const Uuid&, const Uuid&) = default;

    [[nodiscard]] static Result<Uuid> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;
};

// Canonical RFC 3339 UTC timestamps use Z and up to nine fractional digits.
// No local time, timezone inference or leap-second normalization is performed.
struct UtcTimestamp {
    std::chrono::sys_seconds seconds { };
    std::uint32_t nanosecond { 0 }; // 0 .. 999,999,999
    friend bool operator==(const UtcTimestamp&, const UtcTimestamp&) = default;

    [[nodiscard]] static Result<UtcTimestamp> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;
};

struct Sha256Digest {
    std::array<std::uint8_t, 32> bytes { };
    friend bool operator==(const Sha256Digest&, const Sha256Digest&) = default;

    [[nodiscard]] static Result<Sha256Digest> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;
};

// This type is constructible only from normalized relative POSIX-style paths.
class SessionPath {
public:
    [[nodiscard]] static Result<SessionPath> parse(std::string_view text);
    [[nodiscard]] const std::filesystem::path& value() const noexcept { return path_; }
    [[nodiscard]] std::string to_string() const { return path_.generic_string(); }
    // Resolves existing symlinks, including an existing ancestor of a not-yet-created file.
    [[nodiscard]] Result<std::filesystem::path> resolve_within(
        const std::filesystem::path& session_root) const;

private:
    explicit SessionPath(std::filesystem::path path)
        : path_(std::move(path))
    {
    }
    std::filesystem::path path_;
};

enum class AcquisitionState { InProgress, Complete, Incomplete, Interrupted };
enum class ScanKind { Blank, Sample };
enum class ValidationLevel { Draft, Production };

struct FileIdentity {
    SessionPath path;
    std::uint64_t byte_size { 0 };
    std::optional<Sha256Digest> sha256; // Missing while an acquisition is in progress.
};

struct ScanGrid {
    double start_z_m { };
    double end_z_m { };
    std::uint32_t position_count { };
    double spacing_m { };
};

struct CaptureSettings {
    double sample_rate_hz { };
    std::uint32_t pretrigger_sample_count { };
    std::uint32_t posttrigger_sample_count { };
    std::uint16_t peak_extraction_rule { }; // 1 = maximum post-trigger voltage.
    std::uint16_t peak_extraction_rule_version { };
    std::uint32_t target_accepted_readings { };
    std::uint32_t maximum_rejected_attempts { };
};

struct ScanRecord {
    Uuid acquisition_id;
    ScanKind kind { ScanKind::Blank };
    AcquisitionState state { AcquisitionState::InProgress };
    FileIdentity file;
    UtcTimestamp acquired_at;
    std::string sample_id; // Empty for a blank.
    std::string solvent_id;
    std::string cuvette_id;
    std::string scan_configuration_id;
    ScanGrid grid;
    CaptureSettings capture;
    std::optional<std::uint64_t> valid_prefix_byte_size;
    std::optional<std::uint64_t> valid_frame_count;
};

struct DarkConditions {
    bool laser_running { };
    bool beam_blocked { };
    bool reference_detector_capped { };
    bool signal_detector_capped { };
};

struct DarkRecord {
    Uuid record_id;
    AcquisitionState state { AcquisitionState::InProgress };
    FileIdentity file;
    UtcTimestamp acquired_at;
    std::string daq_configuration_id;
    DarkConditions conditions;
    CaptureSettings capture;
};

struct EnergyBurstRecord {
    Uuid burst_id;
    std::optional<Uuid> revision_of;
    FileIdentity file;
    UtcTimestamp entered_at;
    std::uint32_t reading_count { };
    std::string energy_meter_calibration_id;
};

struct CurveMapping {
    Uuid curve_id;
    Uuid sample_acquisition_id;
    Uuid blank_acquisition_id;
    Uuid energy_burst_id;
    std::string cuvette_pair_calibration_id;
    // The selected dark is derived by workup and is not part of this mapping.
};

struct ScalarWithUncertainty {
    double value { };
    double standard_uncertainty { };
    std::string provenance_id;
    std::string uncertainty_evaluation_id;
    // Present only when a quoted bound/expanded value was converted to 1 sigma.
    struct Conversion {
        std::string method; // rectangular_half_width, triangular_half_width, expanded_uncertainty
        double source_value { }; // Same units as value and standard_uncertainty.
        double divisor { }; // sqrt(3), sqrt(6), or explicit coverage factor.
        std::string provenance_id;
    };
    std::optional<Conversion> conversion;
};

struct SampleSnapshot {
    std::string sample_id;
    std::string solvent_id;
    ScalarWithUncertainty concentration_mol_per_l;
    ScalarWithUncertainty solvent_refractive_index;
    UtcTimestamp created_at;
    std::string extensions_toml; // Only the contents of [extensions], never authoritative inputs.
};

struct Cuvette {
    std::string cuvette_id;
    ScalarWithUncertainty path_length_m;
};

struct CuvettePair {
    std::string calibration_id;
    std::string sample_cuvette_id;
    std::string blank_cuvette_id;
    std::string scan_configuration_id;
    ScalarWithUncertainty correction_factor;
};

enum class DetectorRole { Reference, Signal };
enum class TriggerEdge { Rising, Falling };
enum class TerminalConfiguration { Rse, Nrse, Differential, PseudoDifferential };
enum class Coupling { Dc, Ac };

struct DaqChannel {
    std::uint32_t channel_index { };
    DetectorRole role { DetectorRole::Reference };
    TerminalConfiguration terminal_configuration { TerminalConfiguration::Rse };
    Coupling coupling { Coupling::Dc };
    double minimum_v { };
    double maximum_v { };
};

struct DaqConfiguration {
    std::string configuration_id;
    std::string device_id;
    std::uint32_t trigger_terminal_index { };
    TriggerEdge trigger_edge { TriggerEdge::Rising };
    // The ordering is ai0/reference followed by ai1/signal.
    std::array<DaqChannel, 2> channels;
};

struct OpticalFilter {
    std::string filter_id;
    double nominal_optical_density { };
    double wavelength_m { };
    std::string provenance_id;
};

struct OpticalConfiguration {
    std::string configuration_id;
    std::vector<std::string> reference_filter_ids;
    std::vector<std::string> signal_filter_ids;
};

struct StageConfiguration {
    std::string configuration_id;
    std::string controller_id;
    std::uint32_t axis { };
    double maximum_readback_error_m { }; // Pass/fail limit, not a standard uncertainty.
    std::string tolerance_provenance_id;
    std::uint8_t maximum_move_attempts { 2 };
};

struct ScanConfiguration {
    std::string configuration_id;
    std::string daq_configuration_id;
    std::string optical_configuration_id;
    std::string stage_configuration_id;
};

struct InstrumentSnapshot {
    Uuid snapshot_id;
    std::string source_catalog_id;
    std::string source_catalog_revision;
    std::string laser_characterization_id;
    double wavelength_m { };
    double pulse_fwhm_s { };
    ScalarWithUncertainty waist_m;
    ScalarWithUncertainty m2;
    std::string energy_meter_calibration_id;
    double energy_meter_standard_uncertainty_j { };
    std::string energy_meter_uncertainty_provenance_id;
    ScalarWithUncertainty entrance_window_refractive_index;
    std::vector<Cuvette> cuvettes;
    std::vector<CuvettePair> cuvette_pairs;
    std::vector<DaqConfiguration> daq_configurations;
    std::vector<OpticalFilter> optical_filters;
    std::vector<OpticalConfiguration> optical_configurations;
    std::vector<StageConfiguration> stage_configurations;
    std::vector<ScanConfiguration> scan_configurations;
    UtcTimestamp created_at;
    std::string extensions_toml;
};

struct DerivedRunReference {
    Uuid run_id;
    FileIdentity run_manifest;
    UtcTimestamp created_at;
};

struct SessionManifest {
    Uuid session_id;
    UtcTimestamp created_at;
    std::uint64_t manifest_revision { };
    FileIdentity sample_snapshot;
    FileIdentity instrument_snapshot;
    std::vector<ScanRecord> scans;
    std::vector<DarkRecord> darks;
    std::vector<EnergyBurstRecord> energy_bursts;
    std::vector<CurveMapping> curves; // Order is scientifically significant.
    std::vector<DerivedRunReference> derived_runs;
    std::string extensions_toml;
};

// Structural checks only. Complete production/cross-file validation is Phase 7.
[[nodiscard]] Result<void> validate(const ScanGrid& grid);
[[nodiscard]] Result<void> validate(const CaptureSettings& capture);
[[nodiscard]] Result<void> validate(const ScalarWithUncertainty& scalar);
[[nodiscard]] Result<void> validate(const DaqConfiguration& config);
[[nodiscard]] Result<void> validate(const StageConfiguration& config);
[[nodiscard]] Result<void> validate(const OpticalFilter& filter);
[[nodiscard]] Result<void> validate(const OpticalConfiguration& config);
[[nodiscard]] Result<void> validate(const ScanConfiguration& config);
[[nodiscard]] Result<void> validate(const ScanRecord& scan);
[[nodiscard]] Result<void> validate(const DarkRecord& dark);
[[nodiscard]] Result<void> validate(const EnergyBurstRecord& burst);
[[nodiscard]] Result<void> validate(const CurveMapping& curve);
[[nodiscard]] Result<void> validate(
    const SampleSnapshot& sample, ValidationLevel level = ValidationLevel::Production);
[[nodiscard]] Result<void> validate(
    const InstrumentSnapshot& instrument, ValidationLevel level = ValidationLevel::Production);
[[nodiscard]] Result<void> validate(const DerivedRunReference& run);
[[nodiscard]] Result<void> validate(const SessionManifest& session);

} // namespace zscan::schema
