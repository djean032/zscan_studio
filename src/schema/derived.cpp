#include "zscan/schema/derived.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace zscan::schema {
namespace {
    Result<void> bad(std::string location, std::string message)
    {
        return std::unexpected(SchemaError {
            ErrorCode::InvalidValue, std::move(message), { }, { }, std::move(location) });
    }
    bool known_parameter(std::string_view value)
    {
        return std::find(model_parameter_names.begin(), model_parameter_names.end(), value)
            != model_parameter_names.end();
    }
    bool positive(double value) { return std::isfinite(value) && value > 0; }
    bool file_sealed(const FileIdentity& file)
    {
        return file.byte_size > 0 && file.sha256.has_value();
    }
}

Result<void> validate(const FitRequestDocument& request)
{
    if (!Uuid::parse(request.run_id.to_string()) || request.curve_order.empty()
        || !file_sealed(request.prepared_curves) || !file_sealed(request.joint_covariance))
        return bad("fit_request", "request needs run ID, curves and sealed numerical inputs");
    std::set<std::string> fitted;
    for (double value : request.initial_parameters.values)
        if (!positive(value))
            return bad("initial_parameters", "all kinetic values must be positive");
    for (const auto& parameter : request.fitted_parameters)
        if (!known_parameter(parameter) || !fitted.insert(parameter).second)
            return bad("fitted_parameters", "fitted parameters must be known and unique");
    if (fitted.empty() || request.solver.nz == 0 || request.solver.max_steps == 0
        || request.solver.waveform_knots < 2 || !positive(request.solver.population_rtol)
        || !positive(request.solver.quadrature_rtol) || !positive(request.solver.atol)
        || request.solver.pulse_start_fwhm >= request.solver.pulse_end_fwhm
        || request.optimizer.max_evaluations <= 0 || !positive(request.optimizer.ftol)
        || !positive(request.optimizer.xtol) || !positive(request.optimizer.gtol)
        || !positive(request.optimizer.epsfcn))
        return bad("solver", "invalid fitted set, solver, or optimizer settings");
    std::set<std::string> priors;
    for (const auto& prior : request.fixed_parameter_priors)
        if (!known_parameter(prior.parameter) || fitted.contains(prior.parameter)
            || !priors.insert(prior.parameter).second || !std::isfinite(prior.standard_uncertainty)
            || prior.standard_uncertainty < 0 || prior.provenance_id.empty())
            return bad("fixed_parameter_priors", "invalid, duplicate, or fitted-parameter prior");
    return { };
}

Result<void> validate(const FitResultDocument& result)
{
    if (!Uuid::parse(result.run_id.to_string()) || result.function_evaluations < 0
        || !std::isfinite(result.residual_norm) || !std::isfinite(result.initial_residual_norm)
        || !std::isfinite(result.max_abs_residual)
        || (result.chi_squared && (!std::isfinite(*result.chi_squared) || *result.chi_squared < 0)))
        return bad("fit_result", "invalid fit result diagnostics");
    std::set<std::string> seen;
    for (const auto& parameter : result.parameters) {
        if (!known_parameter(parameter.parameter) || !seen.insert(parameter.parameter).second
            || !positive(parameter.value))
            return bad("parameters", "invalid or duplicate fitted parameter result");
        const auto valid_optional = [](std::optional<double> value) {
            return !value || (std::isfinite(*value) && *value >= 0);
        };
        if (!valid_optional(parameter.conditional_standard_error)
            || !valid_optional(parameter.total_standard_error))
            return bad("parameters.standard_error", "parameter uncertainty must be nonnegative");
        for (double contribution : parameter.calibration_variance_contributions)
            if (!std::isfinite(contribution) || contribution < 0)
                return bad("parameters.calibration_variance_contributions",
                    "variance contribution must be nonnegative");
    }
    if (result.fit_arrays && !file_sealed(*result.fit_arrays))
        return bad("fit_arrays", "fit arrays must be sealed");
    return { };
}

Result<void> validate(const DerivedRunDocument& run)
{
    if (!Uuid::parse(run.run_id.to_string()) || !Uuid::parse(run.session_id.to_string())
        || run.run_id == run.session_id || run.zscan_revision.empty() || run.zfit_revision.empty()
        || run.inputs.empty() || !file_sealed(run.fit_request)
        || (run.state == DerivedRunState::Complete
            && (!run.fit_result || !file_sealed(*run.fit_result)))
        || !std::isfinite(run.maximum_ai0_drift_v) || run.maximum_ai0_drift_v < 0
        || !std::isfinite(run.maximum_ai1_drift_v) || run.maximum_ai1_drift_v < 0)
        return bad("run", "invalid run identity, provenance, drift policy, or output state");
    std::set<std::string> paths;
    for (const auto& input : run.inputs)
        if (input.kind.empty() || !file_sealed(input.file)
            || !paths.insert(input.file.path.to_string()).second)
            return bad("inputs", "run inputs need kinds, seals and unique paths");
    std::set<std::string> superseded;
    for (const auto& id : run.supersedes_run_ids)
        if (id == run.run_id || !superseded.insert(id.to_string()).second)
            return bad("supersedes_run_ids", "run supersession must be unique and acyclic locally");
    return { };
}
} // namespace zscan::schema

namespace zscan::schema::toml_io {
namespace {
    [[noreturn]] void reject(std::string location, std::string message)
    {
        throw SchemaError { ErrorCode::InvalidValue, std::move(message), { }, { },
            std::move(location) };
    }
    toml::table parse(std::string_view text)
    {
        try {
            return toml::parse(text);
        } catch (const toml::parse_error& error) {
            reject("toml", std::string(error.description()));
        }
    }
    void allowed(const toml::table& table, std::initializer_list<std::string_view> names,
        std::string_view where)
    {
        for (const auto& [key, value] : table)
            if (std::find(names.begin(), names.end(), key.str()) == names.end())
                reject(std::string(where) + "." + std::string(key.str()), "unknown field");
    }
    const toml::table& tab(const toml::table& table, std::string_view key)
    {
        const auto* result = table.get(key) ? table.get(key)->as_table() : nullptr;
        if (!result)
            reject(std::string(key), "missing table");
        return *result;
    }
    const toml::array& arr(const toml::table& table, std::string_view key)
    {
        const auto* result = table.get(key) ? table.get(key)->as_array() : nullptr;
        if (!result)
            reject(std::string(key), "missing array");
        return *result;
    }
    std::string str(const toml::table& table, std::string_view key)
    {
        if (auto value = table[key].value<std::string>())
            return *value;
        reject(std::string(key), "missing string");
    }
    double num(const toml::table& table, std::string_view key)
    {
        if (auto value = table[key].value<double>())
            return *value;
        reject(std::string(key), "missing number");
    }
    std::int64_t integer(const toml::table& table, std::string_view key)
    {
        if (auto value = table[key].value<std::int64_t>())
            return *value;
        reject(std::string(key), "missing integer");
    }
    bool boolean(const toml::table& table, std::string_view key)
    {
        if (auto value = table[key].value<bool>())
            return *value;
        reject(std::string(key), "missing boolean");
    }
    FileIdentity file(const toml::table& table)
    {
        allowed(table, { "path", "byte_size", "sha256" }, "file");
        const auto size = integer(table, "byte_size");
        if (size < 0)
            reject("byte_size", "negative size");
        return { *SessionPath::parse(str(table, "path")), static_cast<std::uint64_t>(size),
            *Sha256Digest::parse(str(table, "sha256")) };
    }
    toml::table file(const FileIdentity& value)
    {
        if (value.byte_size > std::uint64_t(INT64_MAX) || !value.sha256)
            reject("file", "unsealed or oversized file reference");
        return toml::table { { "path", value.path.to_string() },
            { "byte_size", static_cast<std::int64_t>(value.byte_size) },
            { "sha256", value.sha256->to_string() } };
    }
    toml::table parameters(const ModelParameterValues& values)
    {
        toml::table table;
        for (std::size_t i = 0; i < values.values.size(); ++i)
            table.insert(model_parameter_names[i], values.values[i]);
        return table;
    }
    ModelParameterValues parameters(const toml::table& table)
    {
        allowed(table,
            { "sigma_g", "sigma_s", "sigma_t", "tau10", "tau21", "tau13", "tau30", "tau43" },
            "initial_parameters");
        ModelParameterValues result;
        for (std::size_t i = 0; i < result.values.size(); ++i)
            result.values[i] = num(table, model_parameter_names[i]);
        return result;
    }
    template <typename T, typename F> Result<T> parse_result(F&& fn)
    {
        try {
            return fn();
        } catch (const SchemaError& error) {
            return std::unexpected(error);
        } catch (const toml::parse_error& error) {
            return std::unexpected(SchemaError {
                ErrorCode::InvalidValue, std::string(error.description()), { }, { }, "toml" });
        }
    }
    template <typename F> Result<std::string> format_result(F&& fn)
    {
        try {
            std::ostringstream output;
            output << fn();
            return output.str();
        } catch (const SchemaError& error) {
            return std::unexpected(error);
        }
    }
    void schema(const toml::table& root)
    {
        if (integer(root, "schema_version") != 1)
            reject("schema_version", "unsupported version");
    }
}

Result<FitRequestDocument> parse_fit_request(std::string_view text)
{
    return parse_result<FitRequestDocument>([&] {
        const auto root = parse(text);
        allowed(root,
            { "schema_version", "run_id", "curve_order", "fitted_parameters", "initial_parameters",
                "solver", "optimizer", "fixed_parameter_priors", "prepared_curves",
                "joint_covariance" },
            "fit_request");
        schema(root);
        FitRequestDocument request { *Uuid::parse(str(root, "run_id")), { }, { }, { }, { }, { },
            { }, file(tab(root, "prepared_curves")), file(tab(root, "joint_covariance")) };
        for (const auto& node : arr(root, "curve_order"))
            request.curve_order.push_back(*Uuid::parse(node.value<std::string>().value_or("")));
        for (const auto& node : arr(root, "fitted_parameters"))
            request.fitted_parameters.push_back(node.value<std::string>().value_or(""));
        request.initial_parameters = parameters(tab(root, "initial_parameters"));
        const auto& solver = tab(root, "solver");
        allowed(solver,
            { "nz", "population_rtol", "quadrature_rtol", "atol", "max_steps", "waveform_knots",
                "pulse_start_fwhm", "pulse_end_fwhm" },
            "solver");
        request.solver = { static_cast<std::uint64_t>(integer(solver, "nz")),
            num(solver, "population_rtol"), num(solver, "quadrature_rtol"), num(solver, "atol"),
            static_cast<std::uint64_t>(integer(solver, "max_steps")),
            static_cast<std::uint64_t>(integer(solver, "waveform_knots")),
            num(solver, "pulse_start_fwhm"), num(solver, "pulse_end_fwhm") };
        const auto& optimizer = tab(root, "optimizer");
        allowed(optimizer, { "max_evaluations", "ftol", "xtol", "gtol", "epsfcn" }, "optimizer");
        request.optimizer = { integer(optimizer, "max_evaluations"), num(optimizer, "ftol"),
            num(optimizer, "xtol"), num(optimizer, "gtol"), num(optimizer, "epsfcn") };
        for (const auto& node : arr(root, "fixed_parameter_priors")) {
            const auto* prior = node.as_table();
            if (!prior)
                reject("fixed_parameter_priors", "expected table");
            allowed(*prior, { "parameter", "standard_uncertainty", "provenance_id" }, "prior");
            request.fixed_parameter_priors.push_back({ str(*prior, "parameter"),
                num(*prior, "standard_uncertainty"), str(*prior, "provenance_id") });
        }
        if (auto valid = validate(request); !valid)
            throw valid.error();
        return request;
    });
}

Result<std::string> format_fit_request(const FitRequestDocument& request)
{
    return format_result([&] {
        if (auto valid = validate(request); !valid)
            throw valid.error();
        toml::table root { { "schema_version", 1 }, { "run_id", request.run_id.to_string() } };
        toml::array curves;
        for (const auto& id : request.curve_order)
            curves.push_back(id.to_string());
        root.insert("curve_order", std::move(curves));
        toml::array fitted;
        for (const auto& value : request.fitted_parameters)
            fitted.push_back(value);
        root.insert("fitted_parameters", std::move(fitted));
        root.insert("initial_parameters", parameters(request.initial_parameters));
        root.insert("solver",
            toml::table { { "nz", static_cast<std::int64_t>(request.solver.nz) },
                { "population_rtol", request.solver.population_rtol },
                { "quadrature_rtol", request.solver.quadrature_rtol },
                { "atol", request.solver.atol },
                { "max_steps", static_cast<std::int64_t>(request.solver.max_steps) },
                { "waveform_knots", static_cast<std::int64_t>(request.solver.waveform_knots) },
                { "pulse_start_fwhm", request.solver.pulse_start_fwhm },
                { "pulse_end_fwhm", request.solver.pulse_end_fwhm } });
        root.insert("optimizer",
            toml::table { { "max_evaluations", request.optimizer.max_evaluations },
                { "ftol", request.optimizer.ftol }, { "xtol", request.optimizer.xtol },
                { "gtol", request.optimizer.gtol }, { "epsfcn", request.optimizer.epsfcn } });
        toml::array priors;
        for (const auto& prior : request.fixed_parameter_priors)
            priors.push_back(toml::table { { "parameter", prior.parameter },
                { "standard_uncertainty", prior.standard_uncertainty },
                { "provenance_id", prior.provenance_id } });
        root.insert("fixed_parameter_priors", std::move(priors));
        root.insert("prepared_curves", file(request.prepared_curves));
        root.insert("joint_covariance", file(request.joint_covariance));
        return root;
    });
}

Result<FitResultDocument> parse_fit_result(std::string_view text)
{
    return parse_result<FitResultDocument>([&] {
        const auto root = parse(text);
        allowed(root,
            { "schema_version", "run_id", "converged", "status", "function_evaluations",
                "residual_norm", "initial_residual_norm", "max_abs_residual", "chi_squared",
                "jacobian_rank", "parameters", "fit_arrays" },
            "fit_result");
        schema(root);
        FitResultDocument result;
        result.run_id = *Uuid::parse(str(root, "run_id"));
        result.converged = boolean(root, "converged");
        result.status = integer(root, "status");
        result.function_evaluations = integer(root, "function_evaluations");
        result.residual_norm = num(root, "residual_norm");
        result.initial_residual_norm = num(root, "initial_residual_norm");
        result.max_abs_residual = num(root, "max_abs_residual");
        if (root.contains("chi_squared"))
            result.chi_squared = num(root, "chi_squared");
        if (root.contains("jacobian_rank")) {
            const auto rank = integer(root, "jacobian_rank");
            if (rank < 0)
                reject("jacobian_rank", "negative rank");
            result.jacobian_rank = static_cast<std::uint64_t>(rank);
        }
        for (const auto& node : arr(root, "parameters")) {
            const auto* parameter = node.as_table();
            if (!parameter)
                reject("parameters", "expected table");
            allowed(*parameter,
                { "parameter", "value", "conditional_standard_error", "total_standard_error",
                    "calibration_variance_contributions" },
                "parameters");
            ParameterResult value;
            value.parameter = str(*parameter, "parameter");
            value.value = num(*parameter, "value");
            if (parameter->contains("conditional_standard_error"))
                value.conditional_standard_error = num(*parameter, "conditional_standard_error");
            if (parameter->contains("total_standard_error"))
                value.total_standard_error = num(*parameter, "total_standard_error");
            const auto& contributions = arr(*parameter, "calibration_variance_contributions");
            if (contributions.size() != 8)
                reject("parameters.calibration_variance_contributions", "expected eight groups");
            for (std::size_t i = 0; i < 8; ++i)
                value.calibration_variance_contributions[i]
                    = contributions[i].value<double>().value_or(-1);
            result.parameters.push_back(std::move(value));
        }
        if (root.contains("fit_arrays"))
            result.fit_arrays = file(tab(root, "fit_arrays"));
        if (auto valid = validate(result); !valid)
            throw valid.error();
        return result;
    });
}

Result<std::string> format_fit_result(const FitResultDocument& result)
{
    return format_result([&] {
        if (auto valid = validate(result); !valid)
            throw valid.error();
        toml::table root { { "schema_version", 1 }, { "run_id", result.run_id.to_string() },
            { "converged", result.converged }, { "status", result.status },
            { "function_evaluations", result.function_evaluations },
            { "residual_norm", result.residual_norm },
            { "initial_residual_norm", result.initial_residual_norm },
            { "max_abs_residual", result.max_abs_residual } };
        if (result.chi_squared)
            root.insert("chi_squared", *result.chi_squared);
        if (result.jacobian_rank)
            root.insert("jacobian_rank", static_cast<std::int64_t>(*result.jacobian_rank));
        toml::array parameters;
        for (const auto& parameter : result.parameters) {
            toml::table table { { "parameter", parameter.parameter },
                { "value", parameter.value } };
            if (parameter.conditional_standard_error)
                table.insert("conditional_standard_error", *parameter.conditional_standard_error);
            if (parameter.total_standard_error)
                table.insert("total_standard_error", *parameter.total_standard_error);
            toml::array contributions;
            for (double value : parameter.calibration_variance_contributions)
                contributions.push_back(value);
            table.insert("calibration_variance_contributions", std::move(contributions));
            parameters.push_back(std::move(table));
        }
        root.insert("parameters", std::move(parameters));
        if (result.fit_arrays)
            root.insert("fit_arrays", file(*result.fit_arrays));
        return root;
    });
}

Result<DerivedRunDocument> parse_run(std::string_view text)
{
    return parse_result<DerivedRunDocument>([&] {
        const auto root = parse(text);
        allowed(root,
            { "schema_version", "run_id", "session_id", "created_at", "state", "software", "inputs",
                "workup", "fit_request", "fit_result", "supersedes_run_ids" },
            "run");
        schema(root);
        DerivedRunDocument run { *Uuid::parse(str(root, "run_id")),
            *Uuid::parse(str(root, "session_id")), *UtcTimestamp::parse(str(root, "created_at")),
            DerivedRunState::Complete, { }, { }, false, { }, { }, 0, 0,
            file(tab(root, "fit_request")), { }, { } };
        const auto state = str(root, "state");
        if (state != "complete" && state != "failed")
            reject("state", "unknown run state");
        run.state = state == "complete" ? DerivedRunState::Complete : DerivedRunState::Failed;
        const auto& software = tab(root, "software");
        allowed(
            software, { "zscan_revision", "zfit_revision", "working_tree_modified" }, "software");
        run.zscan_revision = str(software, "zscan_revision");
        run.zfit_revision = str(software, "zfit_revision");
        run.working_tree_modified = boolean(software, "working_tree_modified");
        for (const auto& node : arr(root, "inputs")) {
            const auto* input = node.as_table();
            if (!input)
                reject("inputs", "expected table");
            allowed(*input, { "kind", "record_id", "file" }, "inputs");
            run.inputs.push_back({ str(*input, "kind"),
                input->contains("record_id") ? str(*input, "record_id") : std::string { },
                file(tab(*input, "file")) });
        }
        const auto& workup = tab(root, "workup");
        allowed(workup, { "selected_dark_ids", "maximum_ai0_drift_v", "maximum_ai1_drift_v" },
            "workup");
        for (const auto& node : arr(workup, "selected_dark_ids"))
            run.selected_dark_ids.push_back(*Uuid::parse(node.value<std::string>().value_or("")));
        run.maximum_ai0_drift_v = num(workup, "maximum_ai0_drift_v");
        run.maximum_ai1_drift_v = num(workup, "maximum_ai1_drift_v");
        if (root.contains("fit_result"))
            run.fit_result = file(tab(root, "fit_result"));
        for (const auto& node : arr(root, "supersedes_run_ids"))
            run.supersedes_run_ids.push_back(*Uuid::parse(node.value<std::string>().value_or("")));
        if (auto valid = validate(run); !valid)
            throw valid.error();
        return run;
    });
}

Result<std::string> format_run(const DerivedRunDocument& run)
{
    return format_result([&] {
        if (auto valid = validate(run); !valid)
            throw valid.error();
        toml::table root { { "schema_version", 1 }, { "run_id", run.run_id.to_string() },
            { "session_id", run.session_id.to_string() },
            { "created_at", run.created_at.to_string() },
            { "state", run.state == DerivedRunState::Complete ? "complete" : "failed" } };
        root.insert("software",
            toml::table { { "zscan_revision", run.zscan_revision },
                { "zfit_revision", run.zfit_revision },
                { "working_tree_modified", run.working_tree_modified } });
        toml::array inputs;
        for (const auto& input : run.inputs) {
            toml::table table { { "kind", input.kind }, { "file", file(input.file) } };
            if (!input.record_id.empty())
                table.insert("record_id", input.record_id);
            inputs.push_back(std::move(table));
        }
        root.insert("inputs", std::move(inputs));
        toml::array darks;
        for (const auto& id : run.selected_dark_ids)
            darks.push_back(id.to_string());
        root.insert("workup",
            toml::table { { "selected_dark_ids", std::move(darks) },
                { "maximum_ai0_drift_v", run.maximum_ai0_drift_v },
                { "maximum_ai1_drift_v", run.maximum_ai1_drift_v } });
        root.insert("fit_request", file(run.fit_request));
        if (run.fit_result)
            root.insert("fit_result", file(*run.fit_result));
        toml::array superseded;
        for (const auto& id : run.supersedes_run_ids)
            superseded.push_back(id.to_string());
        root.insert("supersedes_run_ids", std::move(superseded));
        return root;
    });
}

} // namespace zscan::schema::toml_io
