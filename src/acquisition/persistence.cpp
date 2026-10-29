#include "zscan/acquisition/persistence.hpp"

namespace zscan::acquisition {

schema::Result<schema::storage::Seal> persist_scan(
    const std::filesystem::path& session_root, const schema::SessionPath& path,
    const RunOutcome& outcome)
{
    if (!outcome.error.empty() || outcome.scan.end.result != schema::binary::ScanResult::Complete)
        return std::unexpected(schema::SchemaError { schema::ErrorCode::InvalidValue,
            "incomplete acquisition cannot be published", session_root, { }, "acquisition" });
    return schema::storage::write_sealed_scan(session_root, path, outcome.scan);
}

} // namespace zscan::acquisition
