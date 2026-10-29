#pragma once

#include "zscan/acquisition/runner.hpp"
#include "zscan/schema/storage.hpp"

namespace zscan::acquisition {

[[nodiscard]] schema::Result<schema::storage::Seal> persist_scan(
    const std::filesystem::path& session_root, const schema::SessionPath& path,
    const RunOutcome& outcome);

} // namespace zscan::acquisition
