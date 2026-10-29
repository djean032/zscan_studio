#pragma once

#include "zscan/schema/session_validator.hpp"

namespace zscan::schema {

struct ValidationReport {
    std::vector<SchemaError> errors;
    std::vector<SchemaError> warnings;

    [[nodiscard]] bool has_errors() const noexcept { return !errors.empty(); }
    [[nodiscard]] bool is_valid() const noexcept { return errors.empty(); }
};

// UI-facing validation entry point. It reports all currently discoverable
// session failures while preserving the fail-fast production loader below.
[[nodiscard]] ValidationReport validate_session(const std::filesystem::path& root);

} // namespace zscan::schema
