// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/bytes.hpp"
#include "io/zip/archive_metadata.hpp"

#include <catch2/catch_test_macros.hpp>

using zp::ArchiveMetadata;

TEST_CASE("archive metadata round-trips through bytes and comment", "[metadata]")
{
    auto m = ArchiveMetadata::current("README - How to restore original audio.txt");
    const auto bytes = m.serialize();
    CHECK(ArchiveMetadata::parse(bytes) == m);
    const auto comment = m.to_comment();
    CHECK(comment.find('\0') == std::string::npos);
    for (const char c : comment)
        CHECK((c >= 0x20 && c < 0x7F));
    CHECK(ArchiveMetadata::from_comment(comment) == m);
}

TEST_CASE("binary layout matches the spec", "[metadata]")
{
    ArchiveMetadata m = ArchiveMetadata::current();
    m.app_version = "X 1";
    m.readme_name = "";
    const auto b = m.serialize();
    REQUIRE(b.size() == 4 + 1 + 2 + 3 + 2);
    CHECK(b[4] == ArchiveMetadata::current_schema);
    CHECK(b[5] == 3);
    CHECK(b[6] == 0);
    CHECK(b[10] == 0);
    CHECK(b[11] == 0);
}

TEST_CASE("foreign, truncated and empty comments give nullopt", "[metadata]")
{
    CHECK_FALSE(ArchiveMetadata::from_comment(""));
    CHECK_FALSE(ArchiveMetadata::from_comment("Created by Info-ZIP"));
    CHECK_FALSE(ArchiveMetadata::from_comment("zpmeta:"));
    CHECK_FALSE(ArchiveMetadata::from_comment("zpmeta:!!!!"));
    CHECK_FALSE(ArchiveMetadata::parse({}));
    auto bytes = ArchiveMetadata::current("readme.txt").serialize();
    bytes.resize(bytes.size() - 3);
    CHECK_FALSE(ArchiveMetadata::parse(bytes));
    auto foreign = ArchiveMetadata::current().serialize();
    foreign[0] ^= 0xFF;
    CHECK_FALSE(ArchiveMetadata::parse(foreign));
    CHECK_FALSE(ArchiveMetadata::from_comment("zpmeta:" + zp::base64_encode(foreign)));
}
