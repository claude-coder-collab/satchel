// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/zip_writer.hpp"

#include <catch2/catch_test_macros.hpp>

using zp::Zip64Policy;

TEST_CASE("Zip64 entry thresholds", "[zip64]")
{
    const std::uint64_t limit = 0xFFFFFFFFull - Zip64Policy::local_cushion;
    CHECK_FALSE(Zip64Policy::needs_zip64_entry(limit - 1, 0));
    CHECK(Zip64Policy::needs_zip64_entry(limit, 0));
    CHECK(Zip64Policy::needs_zip64_entry(limit + 1, 0));
    CHECK_FALSE(Zip64Policy::needs_zip64_entry(0, 0));
    CHECK(Zip64Policy::needs_zip64_entry(std::nullopt, 0));
    CHECK_FALSE(Zip64Policy::needs_zip64_entry(10, 0xFFFFFFFEull));
    CHECK(Zip64Policy::needs_zip64_entry(10, 0xFFFFFFFFull));
    CHECK(Zip64Policy::needs_zip64_entry(10, 0x100000000ull));
}

TEST_CASE("Zip64 archive thresholds", "[zip64]")
{
    CHECK_FALSE(Zip64Policy::needs_zip64_archive(0xFFFFFFFEull, 0xFFFE));
    CHECK(Zip64Policy::needs_zip64_archive(0xFFFFFFFFull, 0));
    CHECK(Zip64Policy::needs_zip64_archive(0x100000000ull, 0));
    CHECK(Zip64Policy::needs_zip64_archive(0, 0xFFFF));
    CHECK(Zip64Policy::needs_zip64_archive(0, 0x10000));
}
