#pragma once

#include "zscan/schema/derived.hpp"
#include "zscan/schema/types.hpp"

namespace zscan::schema::toml_io {

// Version 1 stores timestamps as quoted RFC 3339 UTC strings, not timezone-less
// TOML local datetimes. All standard keys are strict; extensions are isolated.
[[nodiscard]] Result<SessionManifest> parse_session(std::string_view text);
[[nodiscard]] Result<std::string> format_session(const SessionManifest& session);
[[nodiscard]] Result<SampleSnapshot> parse_sample(std::string_view text);
[[nodiscard]] Result<std::string> format_sample(const SampleSnapshot& sample);
[[nodiscard]] Result<InstrumentSnapshot> parse_instrument(std::string_view text);
[[nodiscard]] Result<std::string> format_instrument(const InstrumentSnapshot& instrument);

} // namespace zscan::schema::toml_io
