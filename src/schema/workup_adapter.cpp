#include "zscan/schema/workup_adapter.hpp"

#include "zfit/workup/entrance_chain.hpp"
#include "zfit/workup/prepare.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>

namespace zscan::schema {
namespace {

    std::chrono::system_clock::time_point time_point(const UtcTimestamp& value)
    {
        // system_clock::duration is not nanoseconds on every platform (MSVC
        // uses a coarser tick). Convert the fractional second explicitly.
        return std::chrono::time_point_cast<std::chrono::system_clock::duration>(value.seconds)
            + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds { value.nanosecond });
    }

    zfit::workup::RawScan to_raw_scan(
        const ValidatedSession& session, std::size_t index, double energy_mean_j)
    {
        const auto& metadata = session.manifest.scans[index];
        const auto& binary = session.scans[index];
        zfit::workup::RawScan scan;
        scan.kind = metadata.kind == ScanKind::Sample ? zfit::workup::ScanKind::Sample
                                                      : zfit::workup::ScanKind::Blank;
        scan.session_id = session.manifest.session_id.to_string();
        scan.sample_id = metadata.sample_id;
        scan.solvent_id = metadata.solvent_id;
        scan.cuvette_id = metadata.cuvette_id;
        scan.acquired_at = time_point(metadata.acquired_at);
        scan.measured_pulse_energy_j = energy_mean_j;
        scan.acquisition_id = metadata.acquisition_id.to_string();
        scan.instrument_configuration_id = metadata.scan_configuration_id;
        scan.num_readings_per_point = binary.config.capture.target_accepted_readings;
        for (const auto& position : binary.positions) {
            zfit::workup::RawScanPoint point;
            point.z_m = binary.config.commanded_positions_m[position.position_index];
            for (const auto& shot : position.attempts)
                if (const auto* accepted = std::get_if<binary::AcceptedShot>(&shot))
                    point.readings.push_back({ accepted->ai0_peak_v, accepted->ai1_peak_v });
            scan.points.push_back(std::move(point));
        }
        return scan;
    }

} // namespace

Result<PreparedWorkup> prepare_workup(
    const ValidatedSession& session, zfit::workup::DarkDriftLimits drift_limits)
{
    try {
        PreparedWorkup result;
        const auto& instrument = session.instrument;
        const auto& sample = session.sample;
        const std::string index_provenance
            = instrument.entrance_window_refractive_index.provenance_id + "+"
            + sample.solvent_refractive_index.provenance_id;
        const auto entrance = zfit::workup::compute_entrance_chain({
            instrument.entrance_window_refractive_index.value,
            sample.solvent_refractive_index.value,
            instrument.entrance_window_refractive_index.standard_uncertainty,
            sample.solvent_refractive_index.standard_uncertainty,
            index_provenance,
        });
        const auto& first_pair
            = session.instrument.cuvette_pairs[session.curves.at(0).cuvette_pair_index];
        const auto path = std::find_if(instrument.cuvettes.begin(), instrument.cuvettes.end(),
            [&](const Cuvette& value) { return value.cuvette_id == first_pair.sample_cuvette_id; });
        if (path == instrument.cuvettes.end())
            throw std::invalid_argument("sample cuvette path length missing");
        result.calibration = {
            .laser_characterization_id = instrument.laser_characterization_id,
            .wavelength_m = instrument.wavelength_m,
            .waist_m = instrument.waist_m.value,
            .waist_standard_error_m = instrument.waist_m.standard_uncertainty,
            .pulse_fwhm_s = instrument.pulse_fwhm_s,
            .m2 = instrument.m2.value,
            .m2_standard_error = instrument.m2.standard_uncertainty,
            .energy_meter_calibration_id = instrument.energy_meter_calibration_id,
            .energy_meter_standard_error_j = instrument.energy_meter_standard_uncertainty_j,
            .entrance_chain_calibration_id = entrance.index_source_id,
            .entrance_chain_transmission = entrance.transmission,
            .entrance_chain_standard_error = entrance.standard_error,
            .concentration_mol_per_l = sample.concentration_mol_per_l.value,
            .concentration_standard_error_mol_per_l
            = sample.concentration_mol_per_l.standard_uncertainty,
            .path_length_m = path->path_length_m.value,
            .path_length_standard_error_m = path->path_length_m.standard_uncertainty,
        };

        for (const auto& curve : session.curves) {
            const auto& mapping = session.manifest.curves[result.curves.size()];
            const auto& burst = session.energy_bursts[curve.energy_burst_index];
            const auto energy = zfit::workup::summarize_energy_measurement(
                burst.readings_j, mapping.energy_burst_id.to_string());
            auto raw_sample = to_raw_scan(session, curve.sample_scan_index, energy.mean_j);
            auto raw_blank = to_raw_scan(session, curve.blank_scan_index, energy.mean_j);
            const auto& pair = session.instrument.cuvette_pairs[curve.cuvette_pair_index];
            zfit::workup::CuvettePairCalibration correction {
                .calibration_id = pair.calibration_id,
                .sample_cuvette_id = pair.sample_cuvette_id,
                .blank_cuvette_id = pair.blank_cuvette_id,
                .instrument_configuration_id = pair.scan_configuration_id,
                .correction_factor = pair.correction_factor.value,
                .standard_error = pair.correction_factor.standard_uncertainty,
            };
            const auto& scan_config
                = session.manifest.scans[curve.sample_scan_index].scan_configuration_id;
            const auto& aggregate = *std::find_if(instrument.scan_configurations.begin(),
                instrument.scan_configurations.end(), [&](const ScanConfiguration& config) {
                    return config.configuration_id == scan_config;
                });
            std::vector<zfit::workup::DarkRecord> darks;
            for (std::size_t i = 0; i < session.manifest.darks.size(); ++i) {
                const auto& record = session.manifest.darks[i];
                if (record.state != AcquisitionState::Complete
                    || record.daq_configuration_id != aggregate.daq_configuration_id
                    || !record.conditions.laser_running || !record.conditions.beam_blocked
                    || !record.conditions.reference_detector_capped
                    || !record.conditions.signal_detector_capped)
                    continue;
                zfit::workup::DarkRecord dark;
                dark.record_id = record.record_id.to_string();
                dark.session_id = session.manifest.session_id.to_string();
                // The pinned zfit_cpp type has one configuration ID. The
                // validator already checked matching electronics; this
                // temporary alias lets workup select a capped dark without
                // claiming that it had an illuminated optical configuration.
                dark.instrument_configuration_id = scan_config;
                dark.acquired_at = time_point(record.acquired_at);
                for (const auto& shot : session.darks[i].attempts)
                    if (const auto* accepted = std::get_if<binary::AcceptedShot>(&shot))
                        dark.readings.push_back({ accepted->ai0_peak_v, accepted->ai1_peak_v });
                darks.push_back(std::move(dark));
            }
            const auto selected
                = zfit::workup::select_dark_for_pair(raw_sample, raw_blank, darks, drift_limits);
            if (selected.selected.record_id
                != session.manifest.darks[curve.selected_dark_index].record_id.to_string())
                throw std::invalid_argument("dark selection differs from validated session");
            result.curves.push_back(
                zfit::workup::prepare_curve(raw_sample, raw_blank, correction, selected));
            const auto retained = [](const binary::ScanFile& scan) {
                std::vector<std::vector<std::uint32_t>> positions;
                for (const auto& position : scan.positions) {
                    std::vector<std::uint32_t> indexes;
                    for (const auto& attempt : position.attempts)
                        if (const auto* accepted = std::get_if<binary::AcceptedShot>(&attempt))
                            indexes.push_back(accepted->attempt_index);
                    positions.push_back(std::move(indexes));
                }
                return positions;
            };
            result.sample_retained_attempt_indexes.push_back(
                retained(session.scans[curve.sample_scan_index]));
            result.blank_retained_attempt_indexes.push_back(
                retained(session.scans[curve.blank_scan_index]));
            result.energies.push_back(energy);
            result.darks.push_back(selected.selected);
            result.cuvettes.push_back(std::move(correction));
        }
        return result;
    } catch (const std::exception& error) {
        return std::unexpected(
            SchemaError { ErrorCode::InvalidValue, error.what(), { }, { }, "workup" });
    }
}

} // namespace zscan::schema
