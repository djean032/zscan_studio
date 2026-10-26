#pragma once

#include "zscan/schema/types.hpp"

#include <array>

namespace zscan::schema {

inline constexpr std::array<std::string_view, 8> model_parameter_names { "sigma_g", "sigma_s",
    "sigma_t", "tau10", "tau21", "tau13", "tau30", "tau43" };

struct ModelParameterValues {
    std::array<double, 8> values { };
};

struct SolverSettings {
    std::uint64_t nz { 64 };
    double population_rtol { 1e-6 };
    double quadrature_rtol { 1e-7 };
    double atol { 1e-10 };
    std::uint64_t max_steps { 100000 };
    std::uint64_t waveform_knots { 129 };
    double pulse_start_fwhm { -4 };
    double pulse_end_fwhm { 4 };
};

struct OptimizerSettings {
    std::int64_t max_evaluations { 300 };
    double ftol { 1e-6 };
    double xtol { 1e-6 };
    double gtol { 1e-6 };
    double epsfcn { 1e-6 };
};

struct FixedParameterPrior {
    std::string parameter;
    double standard_uncertainty { };
    std::string provenance_id;
};

struct FitRequestDocument {
    Uuid run_id;
    std::vector<Uuid> curve_order;
    ModelParameterValues initial_parameters;
    std::vector<std::string> fitted_parameters;
    SolverSettings solver;
    OptimizerSettings optimizer;
    std::vector<FixedParameterPrior> fixed_parameter_priors;
    FileIdentity prepared_curves;
    FileIdentity joint_covariance;
};

struct ParameterResult {
    std::string parameter;
    double value { };
    std::optional<double> conditional_standard_error;
    std::optional<double> total_standard_error;
    std::array<double, 8> calibration_variance_contributions { };
};

struct FitResultDocument {
    Uuid run_id;
    bool converged { };
    std::int64_t status { };
    std::int64_t function_evaluations { };
    double residual_norm { };
    double initial_residual_norm { };
    double max_abs_residual { };
    std::optional<double> chi_squared;
    std::optional<std::uint64_t> jacobian_rank;
    std::vector<ParameterResult> parameters;
    std::optional<FileIdentity> fit_arrays;
};

enum class DerivedRunState { Complete, Failed };

struct RunInput {
    std::string kind;
    std::string record_id;
    FileIdentity file;
};

struct DerivedRunDocument {
    Uuid run_id;
    Uuid session_id;
    UtcTimestamp created_at;
    DerivedRunState state { DerivedRunState::Complete };
    std::string zscan_revision;
    std::string zfit_revision;
    bool working_tree_modified { };
    std::vector<RunInput> inputs;
    std::vector<Uuid> selected_dark_ids;
    double maximum_ai0_drift_v { };
    double maximum_ai1_drift_v { };
    FileIdentity fit_request;
    std::optional<FileIdentity> fit_result;
    std::vector<Uuid> supersedes_run_ids;
};

[[nodiscard]] Result<void> validate(const FitRequestDocument& request);
[[nodiscard]] Result<void> validate(const FitResultDocument& result);
[[nodiscard]] Result<void> validate(const DerivedRunDocument& run);

} // namespace zscan::schema

namespace zscan::schema::toml_io {
[[nodiscard]] Result<FitRequestDocument> parse_fit_request(std::string_view text);
[[nodiscard]] Result<std::string> format_fit_request(const FitRequestDocument& request);
[[nodiscard]] Result<FitResultDocument> parse_fit_result(std::string_view text);
[[nodiscard]] Result<std::string> format_fit_result(const FitResultDocument& result);
[[nodiscard]] Result<DerivedRunDocument> parse_run(std::string_view text);
[[nodiscard]] Result<std::string> format_run(const DerivedRunDocument& run);
} // namespace zscan::schema::toml_io
