#include "zscan/acquisition/session.hpp"

#include "zscan/acquisition/persistence.hpp"
#include "zscan/schema/toml_io.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

namespace zscan::acquisition {
namespace {
    struct FileCloser {
        void operator()(std::FILE* file) const
        {
            if (file)
                std::fclose(file);
        }
    };

    template <typename T> schema::Result<T> failure(std::string message)
    {
        return std::unexpected(schema::SchemaError {
            schema::ErrorCode::InvalidValue, std::move(message), { }, { }, "acquisition" });
    }

    schema::Result<schema::FileIdentity> snapshot(
        const std::filesystem::path& root, const char* name, const std::string& contents)
    {
        auto path = schema::SessionPath::parse(name);
        if (!path)
            return std::unexpected(path.error());
        const auto digest = schema::binary::sha256(
            std::span(reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size()));
        if (!digest)
            return std::unexpected(digest.error());
        const auto target = path->resolve_within(root);
        if (!target)
            return std::unexpected(target.error());
        std::unique_ptr<std::FILE, FileCloser> file(std::fopen(target->string().c_str(), "wbx"));
        if (!file || std::fwrite(contents.data(), 1, contents.size(), file.get()) != contents.size()
            || std::fflush(file.get()) != 0)
            return failure<schema::FileIdentity>(
                "cannot create immutable snapshot " + std::string(name));
        return schema::FileIdentity { *path, contents.size(), *digest };
    }

    schema::Result<schema::SessionManifest> load(
        const std::filesystem::path& root, const schema::Uuid& session_id)
    {
        auto manifest = schema::storage::read_session_manifest(root);
        if (!manifest)
            return std::unexpected(manifest.error());
        if (manifest->session_id != session_id)
            return failure<schema::SessionManifest>("binary session ID does not match manifest");
        return manifest;
    }

    schema::Result<schema::SessionManifest> commit(
        const std::filesystem::path& root, schema::SessionManifest manifest)
    {
        ++manifest.manifest_revision;
        if (auto result = schema::storage::write_session_manifest_atomic(root, manifest); !result)
            return std::unexpected(result.error());
        return manifest;
    }

    schema::Result<std::string> read_snapshot(
        const std::filesystem::path& root, const schema::FileIdentity& identity)
    {
        auto bytes = schema::storage::read_sealed_file(
            root, identity.path, { identity.byte_size, *identity.sha256 });
        if (!bytes)
            return std::unexpected(bytes.error());
        return std::string(bytes->begin(), bytes->end());
    }

} // namespace

schema::Result<schema::SessionManifest> create_session(const std::filesystem::path& root,
    const schema::SampleSnapshot& sample, schema::InstrumentSnapshot instrument,
    std::string session_name)
{
    if (auto valid = schema::validate(sample, schema::ValidationLevel::Production); !valid)
        return std::unexpected(valid.error());
    if (auto valid = schema::validate(instrument, schema::ValidationLevel::Production); !valid)
        return std::unexpected(valid.error());
    auto sample_text = schema::toml_io::format_sample(sample);
    if (!sample_text)
        return std::unexpected(sample_text.error());
    auto instrument_text = schema::toml_io::format_instrument(instrument);
    if (!instrument_text)
        return std::unexpected(instrument_text.error());
    auto uuid = schema::binary::random_uuid_v4();
    if (!uuid)
        return std::unexpected(uuid.error());
    if (!std::filesystem::create_directory(root))
        return failure<schema::SessionManifest>("new session directory must not already exist");
    struct NewDirectoryGuard {
        std::filesystem::path path;
        bool committed { false };
        ~NewDirectoryGuard()
        {
            if (!committed) {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        }
    } guard { root };
    auto sample_file = snapshot(root, "sample.toml", *sample_text);
    if (!sample_file)
        return std::unexpected(sample_file.error());
    auto instrument_file = snapshot(root, "instrument.toml", *instrument_text);
    if (!instrument_file)
        return std::unexpected(instrument_file.error());
    schema::SessionManifest manifest { *uuid, sample.created_at, 1, *sample_file,
        *instrument_file };
    if (!session_name.empty()) {
        toml::table extensions;
        extensions.insert("name", session_name);
        std::ostringstream output;
        output << extensions;
        manifest.extensions_toml = output.str();
    }
    if (auto result = schema::storage::write_session_manifest_atomic(root, manifest); !result)
        return std::unexpected(result.error());
    guard.committed = true;
    return manifest;
}

schema::Result<schema::SessionManifest> publish_scan(
    const std::filesystem::path& root, const RunOutcome& outcome, const std::string& cuvette_id)
{
    const auto& scan = outcome.scan;
    if (!outcome.error.empty() || scan.end.result != schema::binary::ScanResult::Complete)
        return failure<schema::SessionManifest>(
            "incomplete scan discarded; restart with a new acquisition ID");
    auto manifest = load(root, scan.header.session_id);
    if (!manifest)
        return std::unexpected(manifest.error());
    auto sample_text = read_snapshot(root, manifest->sample_snapshot);
    if (!sample_text)
        return std::unexpected(sample_text.error());
    auto instrument_text = read_snapshot(root, manifest->instrument_snapshot);
    if (!instrument_text)
        return std::unexpected(instrument_text.error());
    auto sample = schema::toml_io::parse_sample(*sample_text);
    if (!sample)
        return std::unexpected(sample.error());
    auto instrument = schema::toml_io::parse_instrument(*instrument_text);
    if (!instrument)
        return std::unexpected(instrument.error());
    if (instrument->scan_configurations.empty() || instrument->cuvettes.empty())
        return failure<schema::SessionManifest>("session has no scan configuration or cuvettes");
    const auto found = std::find_if(instrument->cuvettes.begin(), instrument->cuvettes.end(),
        [&](const auto& item) { return item.cuvette_id == cuvette_id; });
    if (found == instrument->cuvettes.end())
        return failure<schema::SessionManifest>("unknown cuvette ID");
    const auto path
        = schema::SessionPath::parse("scans/" + scan.header.record_id.to_string() + ".bin");
    if (!path)
        return std::unexpected(path.error());
    std::error_code ec;
    std::filesystem::create_directory(root / "scans", ec);
    if (ec)
        return failure<schema::SessionManifest>("cannot create scans directory");
    const auto seal = persist_scan(root, *path, outcome);
    if (!seal)
        return std::unexpected(seal.error());
    const auto& config = scan.config;
    manifest->scans.push_back({ scan.header.record_id, config.kind,
        schema::AcquisitionState::Complete, { *path, seal->byte_size, seal->sha256 },
        scan.header.timestamp, config.kind == schema::ScanKind::Sample ? sample->sample_id : "",
        sample->solvent_id, cuvette_id, instrument->scan_configurations.front().configuration_id,
        config.grid, config.capture });
    return commit(root, std::move(*manifest));
}

schema::Result<schema::SessionManifest> publish_dark(const std::filesystem::path& root,
    const schema::binary::DarkFile& dark, const schema::DarkConditions& conditions,
    const std::string& daq_configuration_id)
{
    if (dark.end.result != schema::binary::DarkResult::Complete)
        return failure<schema::SessionManifest>(
            "incomplete dark discarded; restart with a new record ID");
    if (auto valid = schema::binary::validate_dark(dark); !valid)
        return std::unexpected(valid.error());
    auto manifest = load(root, dark.header.session_id);
    if (!manifest)
        return std::unexpected(manifest.error());
    auto instrument_text = read_snapshot(root, manifest->instrument_snapshot);
    if (!instrument_text)
        return std::unexpected(instrument_text.error());
    auto instrument = schema::toml_io::parse_instrument(*instrument_text);
    if (!instrument)
        return std::unexpected(instrument.error());
    if (std::none_of(instrument->daq_configurations.begin(), instrument->daq_configurations.end(),
            [&](const auto& daq) { return daq.configuration_id == daq_configuration_id; }))
        return failure<schema::SessionManifest>("unknown DAQ configuration ID");
    auto encoded = schema::binary::encode_dark(dark);
    if (!encoded)
        return std::unexpected(encoded.error());
    auto path = schema::SessionPath::parse("darks/" + dark.header.record_id.to_string() + ".bin");
    if (!path)
        return std::unexpected(path.error());
    std::error_code ec;
    std::filesystem::create_directory(root / "darks", ec);
    if (ec)
        return failure<schema::SessionManifest>("cannot create darks directory");
    const auto digest = schema::binary::sha256(*encoded);
    if (!digest)
        return std::unexpected(digest.error());
    const auto target = path->resolve_within(root);
    if (!target)
        return std::unexpected(target.error());
    std::unique_ptr<std::FILE, FileCloser> file(std::fopen(target->string().c_str(), "wbx"));
    if (!file || std::fwrite(encoded->data(), 1, encoded->size(), file.get()) != encoded->size()
        || std::fflush(file.get()) != 0)
        return failure<schema::SessionManifest>("cannot write immutable dark");
    file.reset();
    auto verified = schema::storage::read_sealed_file(root, *path, { encoded->size(), *digest });
    if (!verified)
        return std::unexpected(verified.error());
    if (auto decoded = schema::binary::decode_dark(*verified); !decoded)
        return std::unexpected(decoded.error());
    manifest->darks.push_back({ dark.header.record_id, schema::AcquisitionState::Complete,
        { *path, encoded->size(), *digest }, dark.header.timestamp, daq_configuration_id,
        conditions, dark.config.capture });
    return commit(root, std::move(*manifest));
}

schema::Result<schema::SessionManifest> publish_energy(
    const std::filesystem::path& root, const schema::binary::EnergyFile& energy)
{
    auto manifest = load(root, energy.header.session_id);
    if (!manifest)
        return std::unexpected(manifest.error());
    auto instrument_text = read_snapshot(root, manifest->instrument_snapshot);
    if (!instrument_text)
        return std::unexpected(instrument_text.error());
    auto instrument = schema::toml_io::parse_instrument(*instrument_text);
    if (!instrument)
        return std::unexpected(instrument.error());
    auto path
        = schema::SessionPath::parse("energy/" + energy.header.record_id.to_string() + ".bin");
    if (!path)
        return std::unexpected(path.error());
    std::error_code ec;
    std::filesystem::create_directory(root / "energy", ec);
    if (ec)
        return failure<schema::SessionManifest>("cannot create energy directory");
    auto seal = schema::storage::write_energy_file_atomic(root, *path, energy);
    if (!seal)
        return std::unexpected(seal.error());
    manifest->energy_bursts.push_back(
        { energy.header.record_id, { }, { *path, seal->byte_size, seal->sha256 },
            energy.header.timestamp, static_cast<std::uint32_t>(energy.readings_j.size()),
            instrument->energy_meter_calibration_id });
    return commit(root, std::move(*manifest));
}

schema::Result<schema::SessionManifest> publish_curve(const std::filesystem::path& root,
    const schema::Uuid& sample_scan, const schema::Uuid& blank_scan,
    const schema::Uuid& energy_burst, const std::string& pair_id)
{
    auto manifest = schema::storage::read_session_manifest(root);
    if (!manifest)
        return std::unexpected(manifest.error());
    auto has_scan = [&](const schema::Uuid& id, schema::ScanKind kind) {
        return std::any_of(manifest->scans.begin(), manifest->scans.end(), [&](const auto& scan) {
            return scan.acquisition_id == id && scan.kind == kind
                && scan.state == schema::AcquisitionState::Complete;
        });
    };
    if (!has_scan(sample_scan, schema::ScanKind::Sample)
        || !has_scan(blank_scan, schema::ScanKind::Blank)
        || std::none_of(manifest->energy_bursts.begin(), manifest->energy_bursts.end(),
            [&](const auto& energy) { return energy.burst_id == energy_burst; })
        || pair_id.empty())
        return failure<schema::SessionManifest>(
            "curve requires complete sample/blank scans and energy");
    auto id = schema::binary::random_uuid_v4();
    if (!id)
        return std::unexpected(id.error());
    manifest->curves.push_back({ *id, sample_scan, blank_scan, energy_burst, pair_id });
    return commit(root, std::move(*manifest));
}

} // namespace zscan::acquisition
