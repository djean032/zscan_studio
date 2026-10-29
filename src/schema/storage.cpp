#include "zscan/schema/storage.hpp"
#include "zscan/schema/toml_io.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace zscan::schema::storage {
namespace {

    template <typename T> Result<T> fail(ErrorCode code, std::string message, std::string location)
    {
        return std::unexpected(
            SchemaError { code, std::move(message), { }, { }, std::move(location) });
    }

    Result<void> durable_flush(std::FILE* file)
    {
        if (std::fflush(file) != 0)
            return fail<void>(ErrorCode::FilesystemError, "file flush failed", "file");
#ifdef _WIN32
        if (_commit(_fileno(file)) != 0)
#else
        if (fsync(fileno(file)) != 0)
#endif
            return fail<void>(ErrorCode::FilesystemError, "file sync failed", "file");
        return { };
    }

    struct FileCloser {
        void operator()(std::FILE* file) const
        {
            if (file)
                std::fclose(file);
        }
    };

} // namespace

Result<InterruptedScan> inspect_interrupted_scan(
    std::span<const std::uint8_t> original, binary::ResourceLimits limits)
{
    auto header = binary::decode_header(original);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != binary::FileKind::Scan)
        return fail<InterruptedScan>(
            ErrorCode::InvalidBinary, "expected an interrupted scan", "header.kind");
    auto digest = binary::sha256(original);
    if (!digest)
        return std::unexpected(digest.error());
    InterruptedScan result { *header, { static_cast<std::uint64_t>(original.size()), *digest },
        binary::header_size, 0, { } };
    binary::FrameReader reader(original.subspan(binary::header_size), limits);
    while (true) {
        auto frame = reader.next();
        if (!frame) {
            if (frame.error().code == ErrorCode::ResourceLimit)
                return std::unexpected(frame.error());
            result.tail_error = frame.error();
            break;
        }
        if (!*frame)
            break;
        if ((**frame).type == static_cast<std::uint16_t>(binary::FrameType::ScanEnd))
            return fail<InterruptedScan>(ErrorCode::InvalidBinary,
                "closing frame present: this is not an interrupted acquisition", "scan_end");
    }
    result.valid_prefix_byte_size = binary::header_size + reader.offset();
    result.valid_frame_count = reader.frames_seen();
    return result;
}

Result<ScanRecord> recover_interrupted_scan(
    const std::filesystem::path& session_root, ScanRecord metadata, std::uint64_t max_bytes)
{
    auto resolved = metadata.file.path.resolve_within(session_root);
    if (!resolved)
        return std::unexpected(resolved.error());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*resolved, ec) || ec)
        return fail<ScanRecord>(ErrorCode::FilesystemError,
            "interrupted scan is not a regular file", metadata.file.path.to_string());
    const auto size = std::filesystem::file_size(*resolved, ec);
    if (ec || size > max_bytes || size > std::numeric_limits<std::streamsize>::max())
        return fail<ScanRecord>(ErrorCode::ResourceLimit, "interrupted scan exceeds recovery cap",
            metadata.file.path.to_string());
    std::ifstream input(*resolved, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size))
        return fail<ScanRecord>(ErrorCode::TruncatedBinary,
            "interrupted scan changed during recovery", metadata.file.path.to_string());
    auto inspection = inspect_interrupted_scan(bytes);
    if (!inspection)
        return std::unexpected(inspection.error());
    auto manifest = read_session_manifest(session_root);
    if (!manifest)
        return std::unexpected(manifest.error());
    if (inspection->header.session_id != manifest->session_id
        || inspection->header.record_id != metadata.acquisition_id
        || inspection->header.timestamp != metadata.acquired_at)
        return fail<ScanRecord>(
            ErrorCode::InvalidValue, "orphan metadata disagrees with binary header", "scans");
    metadata.state = AcquisitionState::Interrupted;
    metadata.file.byte_size = inspection->original.byte_size;
    metadata.file.sha256 = inspection->original.sha256;
    metadata.valid_prefix_byte_size = inspection->valid_prefix_byte_size;
    metadata.valid_frame_count = inspection->valid_frame_count;
    auto found = std::find_if(manifest->scans.begin(), manifest->scans.end(),
        [&](const ScanRecord& scan) { return scan.acquisition_id == metadata.acquisition_id; });
    if (found != manifest->scans.end()) {
        if (found->state != AcquisitionState::InProgress
            || found->file.path.to_string() != metadata.file.path.to_string())
            return fail<ScanRecord>(ErrorCode::InvalidValue,
                "only matching in-progress scan can be recovered", "scans.state");
        *found = metadata;
    } else {
        for (const auto& scan : manifest->scans)
            if (scan.file.path.to_string() == metadata.file.path.to_string())
                return fail<ScanRecord>(ErrorCode::InvalidValue,
                    "orphan path already belongs to another scan", "scans.file.path");
        manifest->scans.push_back(metadata);
    }
    if (manifest->manifest_revision == std::numeric_limits<std::uint64_t>::max())
        return fail<ScanRecord>(
            ErrorCode::InvalidValue, "manifest revision overflow", "manifest_revision");
    ++manifest->manifest_revision;
    if (auto written = write_session_manifest_atomic(session_root, *manifest); !written)
        return std::unexpected(written.error());
    return metadata;
}

Result<std::vector<std::uint8_t>> read_sealed_file(const std::filesystem::path& session_root,
    const SessionPath& path, const Seal& seal, std::uint64_t max_bytes)
{
    auto resolved = path.resolve_within(session_root);
    if (!resolved)
        return std::unexpected(resolved.error());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*resolved, ec) || ec)
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::FilesystemError, "sealed input must be a regular file", path.to_string());
    const auto actual_size = std::filesystem::file_size(*resolved, ec);
    if (ec || actual_size != seal.byte_size)
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::InvalidBinary, "sealed input size has changed", path.to_string());
    if (actual_size > max_bytes || actual_size > std::numeric_limits<std::streamsize>::max())
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::ResourceLimit, "sealed input exceeds reader cap", path.to_string());
    std::ifstream input(*resolved, std::ios::binary);
    if (!input)
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::FilesystemError, "cannot open sealed input", path.to_string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(actual_size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(actual_size));
    if (input.gcount() != static_cast<std::streamsize>(actual_size))
        return fail<std::vector<std::uint8_t>>(ErrorCode::TruncatedBinary,
            "sealed input was truncated during reading", path.to_string());
    const auto digest = binary::sha256(bytes);
    if (!digest)
        return std::unexpected(digest.error());
    if (*digest != seal.sha256)
        return fail<std::vector<std::uint8_t>>(
            ErrorCode::ChecksumMismatch, "sealed input SHA-256 mismatch", path.to_string());
    return bytes;
}

Result<Seal> write_sealed_scan(const std::filesystem::path& session_root, const SessionPath& path,
    const binary::ScanFile& scan)
{
    auto encoded = binary::encode_scan(scan);
    if (!encoded)
        return std::unexpected(encoded.error());
    auto resolved = path.resolve_within(session_root);
    if (!resolved)
        return std::unexpected(resolved.error());
    std::error_code ec;
    if (!std::filesystem::is_directory(resolved->parent_path(), ec) || ec)
        return fail<Seal>(
            ErrorCode::FilesystemError, "scan parent directory must exist", path.to_string());
    // Exclusive creation protects an existing acquisition from replacement.
    std::unique_ptr<std::FILE, FileCloser> output(std::fopen(resolved->string().c_str(), "wbx"));
    if (!output)
        return fail<Seal>(ErrorCode::FilesystemError,
            "cannot create new scan (path may already exist)", path.to_string());
    const auto write = [&](std::size_t start, std::size_t length) {
        return std::fwrite(encoded->data() + start, 1, length, output.get()) == length;
    };
    if (!write(0, binary::header_size))
        return fail<Seal>(ErrorCode::FilesystemError, "scan header write failed", path.to_string());
    std::size_t offset = binary::header_size;
    while (offset < encoded->size()) {
        binary::Reader reader { std::span(*encoded).subspan(offset) };
        const auto type = *reader.u16();
        (void)reader.u16();
        const auto payload_size = *reader.u32();
        const auto size = binary::frame_prefix_size + payload_size + binary::frame_checksum_size;
        if (!write(offset, size))
            return fail<Seal>(
                ErrorCode::FilesystemError, "scan frame write failed", path.to_string());
        offset += size;
        if (type == static_cast<std::uint16_t>(binary::FrameType::PositionEnd)) {
            if (auto result = durable_flush(output.get()); !result)
                return std::unexpected(result.error());
        }
    }
    if (auto result = durable_flush(output.get()); !result)
        return std::unexpected(result.error());
    if (std::fclose(output.release()) != 0)
        return fail<Seal>(ErrorCode::FilesystemError, "scan close failed", path.to_string());
    const auto expected = binary::sha256(*encoded);
    if (!expected)
        return std::unexpected(expected.error());
    Seal seal { static_cast<std::uint64_t>(encoded->size()), *expected };
    auto disk = read_sealed_file(session_root, path, seal);
    if (!disk)
        return std::unexpected(disk.error());
    if (auto result = binary::decode_scan(*disk); !result)
        return std::unexpected(result.error());
    return seal;
}

Result<Seal> write_energy_file_atomic(const std::filesystem::path& session_root,
    const SessionPath& path, const binary::EnergyFile& energy)
{
    auto encoded = binary::encode_energy(energy);
    if (!encoded)
        return std::unexpected(encoded.error());
    auto target = path.resolve_within(session_root);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (!std::filesystem::is_directory(target->parent_path(), ec) || ec)
        return fail<Seal>(
            ErrorCode::FilesystemError, "energy parent directory must exist", path.to_string());
    auto uuid = binary::random_uuid_v4();
    if (!uuid)
        return std::unexpected(uuid.error());
    auto temp_relative = SessionPath::parse(path.to_string() + ".tmp." + uuid->to_string());
    if (!temp_relative)
        return std::unexpected(temp_relative.error());
    auto temp = temp_relative->resolve_within(session_root);
    if (!temp)
        return std::unexpected(temp.error());
    std::unique_ptr<std::FILE, FileCloser> output(std::fopen(temp->string().c_str(), "wbx"));
    if (!output)
        return fail<Seal>(ErrorCode::FilesystemError, "cannot create energy temporary file",
            temp_relative->to_string());
    if (std::fwrite(encoded->data(), 1, encoded->size(), output.get()) != encoded->size())
        return fail<Seal>(
            ErrorCode::FilesystemError, "energy write failed", temp_relative->to_string());
    if (auto synced = durable_flush(output.get()); !synced)
        return std::unexpected(synced.error());
    if (std::fclose(output.release()) != 0)
        return fail<Seal>(
            ErrorCode::FilesystemError, "energy close failed", temp_relative->to_string());
    auto digest = binary::sha256(*encoded);
    if (!digest)
        return std::unexpected(digest.error());
    Seal seal { static_cast<std::uint64_t>(encoded->size()), *digest };
    auto verified = read_sealed_file(session_root, *temp_relative, seal);
    if (!verified)
        return std::unexpected(verified.error());
    if (auto decoded = binary::decode_energy(*verified); !decoded)
        return std::unexpected(decoded.error());
    // An atomic, no-replace publish on filesystems supporting hard links.
    std::filesystem::create_hard_link(*temp, *target, ec);
    if (ec)
        return fail<Seal>(ErrorCode::FilesystemError,
            "cannot publish energy burst without replacing an existing file", path.to_string());
    std::filesystem::remove(*temp, ec);
    if (ec)
        return fail<Seal>(ErrorCode::FilesystemError,
            "energy burst published but temporary link remains", temp_relative->to_string());
    auto published = read_sealed_file(session_root, path, seal);
    if (!published)
        return std::unexpected(published.error());
    return seal;
}

Result<Seal> write_derived_arrays_atomic(const std::filesystem::path& session_root,
    const SessionPath& path, const binary::DerivedArrayFile& arrays)
{
    auto encoded = binary::encode_arrays(arrays);
    if (!encoded)
        return std::unexpected(encoded.error());
    auto target = path.resolve_within(session_root);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (!std::filesystem::is_directory(target->parent_path(), ec) || ec)
        return fail<Seal>(ErrorCode::FilesystemError, "derived-array parent directory must exist",
            path.to_string());
    std::unique_ptr<std::FILE, FileCloser> output(std::fopen(target->string().c_str(), "wbx"));
    if (!output)
        return fail<Seal>(
            ErrorCode::FilesystemError, "cannot create immutable derived arrays", path.to_string());
    if (std::fwrite(encoded->data(), 1, encoded->size(), output.get()) != encoded->size())
        return fail<Seal>(
            ErrorCode::FilesystemError, "derived-array write failed", path.to_string());
    if (auto result = durable_flush(output.get()); !result)
        return std::unexpected(result.error());
    if (std::fclose(output.release()) != 0)
        return fail<Seal>(
            ErrorCode::FilesystemError, "derived-array close failed", path.to_string());
    auto digest = binary::sha256(*encoded);
    if (!digest)
        return std::unexpected(digest.error());
    Seal seal { static_cast<std::uint64_t>(encoded->size()), *digest };
    auto verified = read_sealed_file(session_root, path, seal);
    if (!verified)
        return std::unexpected(verified.error());
    if (auto decoded = binary::decode_arrays(*verified); !decoded)
        return std::unexpected(decoded.error());
    return seal;
}

Result<SessionManifest> read_session_manifest(const std::filesystem::path& session_root)
{
    auto path = SessionPath::parse("session.toml");
    auto resolved = path->resolve_within(session_root);
    if (!resolved)
        return std::unexpected(resolved.error());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*resolved, ec) || ec)
        return fail<SessionManifest>(
            ErrorCode::FilesystemError, "session.toml is missing", "session.toml");
    const auto size = std::filesystem::file_size(*resolved, ec);
    if (ec || size > 16 * 1024 * 1024)
        return fail<SessionManifest>(
            ErrorCode::ResourceLimit, "session manifest is too large", "session.toml");
    std::ifstream input(*resolved, std::ios::binary);
    if (!input)
        return fail<SessionManifest>(
            ErrorCode::FilesystemError, "cannot open session manifest", "session.toml");
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size))
        return fail<SessionManifest>(
            ErrorCode::TruncatedBinary, "session manifest changed during reading", "session.toml");
    auto parsed = toml_io::parse_session(text);
    if (!parsed)
        return std::unexpected(with_source(parsed.error(), *resolved));
    return parsed;
}

Result<void> write_session_manifest_atomic(
    const std::filesystem::path& session_root, const SessionManifest& manifest)
{
    auto text = toml_io::format_session(manifest);
    if (!text)
        return std::unexpected(text.error());
    auto path = SessionPath::parse("session.toml");
    auto target = path->resolve_within(session_root);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (std::filesystem::exists(*target, ec)) {
        auto old = read_session_manifest(session_root);
        if (!old)
            return std::unexpected(old.error());
        if (old->session_id != manifest.session_id
            || old->manifest_revision == std::numeric_limits<std::uint64_t>::max()
            || manifest.manifest_revision != old->manifest_revision + 1)
            return fail<void>(ErrorCode::InvalidValue, "session ID or manifest revision mismatch",
                "manifest_revision");
    } else if (ec || manifest.manifest_revision != 1) {
        return fail<void>(
            ErrorCode::InvalidValue, "first manifest revision must be 1", "manifest_revision");
    }
    auto uuid = binary::random_uuid_v4();
    if (!uuid)
        return std::unexpected(uuid.error());
    auto temporary = *target;
    temporary += ".tmp." + uuid->to_string();
    std::unique_ptr<std::FILE, FileCloser> output(std::fopen(temporary.string().c_str(), "wbx"));
    if (!output)
        return fail<void>(
            ErrorCode::FilesystemError, "cannot create temporary manifest", "session.toml");
    if (std::fwrite(text->data(), 1, text->size(), output.get()) != text->size())
        return fail<void>(
            ErrorCode::FilesystemError, "cannot write temporary manifest", "session.toml");
    if (auto result = durable_flush(output.get()); !result)
        return result;
    if (std::fclose(output.release()) != 0)
        return fail<void>(
            ErrorCode::FilesystemError, "cannot close temporary manifest", "session.toml");
    // Verify the temporary file before publishing it. A failure leaves it for audit.
    bool verified = false;
    {
        std::ifstream verify(temporary, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(verify)), std::istreambuf_iterator<char>());
        verified = verify.is_open() && !verify.bad() && content == *text
            && toml_io::parse_session(content).has_value();
    } // Close the temporary file before renaming it on Windows.
    if (verified) {
        std::filesystem::rename(temporary, *target, ec);
        if (!ec)
            return { };
    }
    return fail<void>(ErrorCode::FilesystemError,
        "temporary manifest verification or rename failed", "session.toml");
}

} // namespace zscan::schema::storage
