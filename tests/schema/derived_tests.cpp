#include "zscan/schema/derived.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace zscan::schema;
using namespace zscan::schema::toml_io;

Uuid id(std::string_view text) { return *Uuid::parse(text); }
FileIdentity file(std::string_view path)
{
    return { *SessionPath::parse(path), 64, *Sha256Digest::parse(std::string(64, 'a')) };
}
}

TEST_CASE("derived fit request records every kinetic, solver and prior choice")
{
    FitRequestDocument request { id("550e8400-e29b-41d4-a716-446655440000"), { }, { }, { }, { },
        { }, { }, file("prepared_curves.bin"), file("measurement_covariance.bin") };
    request.curve_order = { id("33e47923-4466-4ec8-b957-3e4c39ad4516") };
    request.initial_parameters.values
        = { 4.9e-18, 1.6e-17, 1.5e-17, 1e-12, 1e-12, 1e-12, 1.29e-7, 1e-12 };
    request.fitted_parameters = { "sigma_g", "sigma_t" };
    request.fixed_parameter_priors = { { "tau43", 0, "exact-model-claim" } };
    auto encoded = format_fit_request(request);
    REQUIRE(encoded);
    auto decoded = parse_fit_request(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->initial_parameters.values == request.initial_parameters.values);
    REQUIRE(decoded->fitted_parameters == request.fitted_parameters);
    REQUIRE(decoded->fixed_parameter_priors[0].standard_uncertainty == 0);
    REQUIRE(*format_fit_request(*decoded) == *encoded);
    request.fixed_parameter_priors[0].parameter = "sigma_g";
    REQUIRE_FALSE(format_fit_request(request));
}

TEST_CASE("derived fit result keeps eight calibration variance groups")
{
    ParameterResult parameter { "sigma_g", 4.9e-18, 1e-19, 2e-19,
        { 1., 2., 3., 4., 5., 6., 7., 8. } };
    FitResultDocument result { id("550e8400-e29b-41d4-a716-446655440000"), true, 1, 20, 0.1, 0.2,
        0.01, 1.2, 1, { parameter }, file("fit_arrays.bin") };
    auto text = format_fit_result(result);
    REQUIRE(text);
    auto restored = parse_fit_result(*text);
    REQUIRE(restored);
    REQUIRE(restored->parameters[0].calibration_variance_contributions[7] == 8.);
    REQUIRE(restored->jacobian_rank == 1);
    REQUIRE(*format_fit_result(*restored) == *text);
}

TEST_CASE("derived run records immutable inputs, selected darks and supersession")
{
    DerivedRunDocument run { id("550e8400-e29b-41d4-a716-446655440000"),
        id("33e47923-4466-4ec8-b957-3e4c39ad4516"), *UtcTimestamp::parse("2026-10-25T15:00:00Z"),
        DerivedRunState::Complete, { }, { }, false, { }, { }, 0, 0, file("fit_request.toml"),
        file("fit_result.toml"), { } };
    run.zscan_revision = "abc";
    run.zfit_revision = "d3612e9";
    run.inputs = { { "scan", "scan-id", file("inputs/a.bin") } };
    run.selected_dark_ids = { id("171e8400-e29b-41d4-a716-446655440000") };
    run.maximum_ai0_drift_v = 0.01;
    run.maximum_ai1_drift_v = 0.02;
    run.supersedes_run_ids = { id("771e8400-e29b-41d4-a716-446655440000") };
    auto text = format_run(run);
    REQUIRE(text);
    auto restored = parse_run(*text);
    REQUIRE(restored);
    REQUIRE(restored->selected_dark_ids == run.selected_dark_ids);
    REQUIRE(restored->supersedes_run_ids == run.supersedes_run_ids);
    REQUIRE(*format_run(*restored) == *text);
    run.supersedes_run_ids.push_back(run.run_id);
    REQUIRE_FALSE(format_run(run));
}
