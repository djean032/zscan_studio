#pragma once

#include "zscan/schema/derived_arrays.hpp"
#include "zscan/schema/workup_adapter.hpp"

#include "zfit/fit_bridge.hpp"

namespace zscan::schema {

[[nodiscard]] Result<zfit::FitRequest> make_fit_request(const PreparedWorkup& workup);
[[nodiscard]] Result<binary::DerivedArrayFile> make_prepared_arrays(
    const PreparedWorkup& workup, const zfit::FitRequest& request, binary::Header header);
[[nodiscard]] Result<binary::DerivedArrayFile> make_fit_result_arrays(
    const zfit::FitResult& result, binary::Header header);

} // namespace zscan::schema
