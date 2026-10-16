// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/build_job.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace zp;
using zp::build::sample_offsets;

TEST_CASE("sample windows are centred at 20, 50 and 80 percent", "[unit][deflate]")
{
    constexpr std::size_t w = 128u << 10;
    constexpr std::uint64_t size = std::uint64_t{ 100 } << 20;
    const auto offsets = sample_offsets(size, w);
    REQUIRE(offsets.size() == 3);
    CHECK(offsets[0] == size / 5 - w / 2);
    CHECK(offsets[1] == size / 2 - w / 2);
    CHECK(offsets[2] == size / 100 * 80 - w / 2);
    for (const auto o : offsets)
        CHECK(o + w <= size);
}

TEST_CASE("sample windows stay inside the entry and never overlap", "[unit][deflate]")
{
    constexpr std::size_t w = 128u << 10;
    constexpr std::uint64_t ww = w;
    for (const auto size : std::array<std::uint64_t, 8>{ ww, ww + 1, 2 * ww, 3 * ww - 1, 3 * ww, std::uint64_t{ 4 } << 20, (std::uint64_t{ 4 } << 20) + 1, std::uint64_t{ 1 } << 40 })
    {
        const auto offsets = sample_offsets(size, w);
        REQUIRE_FALSE(offsets.empty());
        for (std::size_t i = 0; i < offsets.size(); ++i)
        {
            CHECK(offsets[i] + w <= size);
            if (i > 0)
                CHECK(offsets[i] >= offsets[i - 1] + w);
        }
    }
    CHECK(sample_offsets(1000, w) == std::vector<std::uint64_t>{ 0 });
}
