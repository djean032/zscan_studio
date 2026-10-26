#include "zscan/schema/derived_arrays.hpp"

#include <limits>

#include <catch2/catch_test_macros.hpp>

using namespace zscan::schema;
using namespace zscan::schema::binary;

TEST_CASE("versioned derived arrays replay exact prepared curves and covariance")
{
    DerivedArrayFile file { { FileKind::DerivedArrays,
        *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
        *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
        *UtcTimestamp::parse("2026-10-25T15:00:00Z") } };
    file.arrays = {
        { ArrayRole::ScanPositions, 0, 1, 2, { -0.001, 0.001 } },
        { ArrayRole::Transmission, 0, 1, 2, { 0.8, 0.9 } },
        { ArrayRole::DarkSensitivities, 0, 2, 2, { 0.1, -0.2, 0.1, -0.2 } },
        { ArrayRole::JointMeasurementCovariance, global_array_index, 2, 2,
            { 0.01, 0.001, 0.001, 0.02 } },
    };
    const auto bytes = encode_arrays(file);
    REQUIRE(bytes);
    const auto replay = decode_arrays(*bytes);
    REQUIRE(replay);
    REQUIRE(replay->arrays[1].values_row_major == file.arrays[1].values_row_major);
    REQUIRE(replay->arrays[3].rows == 2);
    REQUIRE(*encode_arrays(*replay) == *bytes);
    file.arrays[0].values_row_major[0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(encode_arrays(file));
}

TEST_CASE("derived arrays reject duplicate roles, malformed shapes and truncation")
{
    DerivedArrayFile file { { FileKind::DerivedArrays,
                                *Uuid::parse("550e8400-e29b-41d4-a716-446655440000"),
                                *Uuid::parse("33e47923-4466-4ec8-b957-3e4c39ad4516"),
                                *UtcTimestamp::parse("2026-10-25T15:00:00Z") },
        { { ArrayRole::Transmission, 0, 1, 2, { 0.8, 0.9 } } } };
    auto duplicate = file;
    duplicate.arrays.push_back(duplicate.arrays[0]);
    REQUIRE_FALSE(encode_arrays(duplicate));
    auto shape = file;
    shape.arrays[0].rows = 2;
    REQUIRE_FALSE(encode_arrays(shape));
    const auto bytes = *encode_arrays(file);
    REQUIRE(decode_arrays(std::span(bytes).first(bytes.size() - 1)).error().code
        == ErrorCode::TruncatedBinary);
}
