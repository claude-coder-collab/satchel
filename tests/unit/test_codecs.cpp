// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/codec.hpp"
#include "codecs/deflate.hpp"
#include "common/bytes.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

TEST_CASE("crc32 known vector and combine", "[codec]")
{
    const std::string s = "123456789";
    CHECK(crc32_update(0, as_bytes(s)) == 0xCBF43926u);
    const auto a = crc32_update(0, as_bytes("1234"));
    const auto b = crc32_update(0, as_bytes("56789"));
    CHECK(crc32_combine(a, b, 5) == 0xCBF43926u);
    CHECK(crc32_combine(0, b, 5) == b);
}

TEST_CASE("segmented deflate concatenates into one valid stream", "[codec]")
{
    const auto data = test::text_like((3u << 20) + 12345, 7);
    auto enc = CodecRegistry::make_encoder(ZipMethod::Deflate, 6);
    std::vector<std::uint8_t> joined;
    const auto seg = enc->segment_size();
    for (std::size_t off = 0; off < data.size(); off += seg)
    {
        const auto n = std::min(seg, data.size() - off);
        std::span<const std::uint8_t> hist;
        if (off > 0)
            hist = std::span(data).subspan(off - enc->history_size(), enc->history_size());
        std::vector<std::uint8_t> in(data.begin() + static_cast<std::ptrdiff_t>(off), data.begin() + static_cast<std::ptrdiff_t>(off + n));
        auto out = enc->encode(hist, std::move(in), off + n == data.size());
        REQUIRE(out);
        joined.insert(joined.end(), out->begin(), out->end());
    }
    CHECK(joined.size() < data.size() / 3);
    auto back = inflate_buffer(joined, data.size());
    REQUIRE(back);
    CHECK(*back == data);

    auto decoder = CodecRegistry::make_decoder(8, std::make_unique<MemoryStream>(joined));
    REQUIRE(decoder);
    std::vector<std::uint8_t> streamed(data.size() + 10);
    auto n = (*decoder)->read_full(streamed);
    REQUIRE(n);
    CHECK(*n == data.size());
    streamed.resize(*n);
    CHECK(streamed == data);
}

TEST_CASE("empty input deflates to a valid empty stream", "[codec]")
{
    auto out = deflate_buffer({}, 6);
    REQUIRE(out);
    auto back = inflate_buffer(*out, 0);
    REQUIRE(back);
    CHECK(back->empty());
}

TEST_CASE("truncated deflate is reported as corrupt", "[codec]")
{
    const auto data = test::random_bytes(100000, 3);
    auto out = deflate_buffer(data, 6);
    REQUIRE(out);
    out->resize(out->size() / 2);
    auto decoder = CodecRegistry::make_decoder(8, std::make_unique<MemoryStream>(*out));
    REQUIRE(decoder);
    std::vector<std::uint8_t> buf(data.size());
    auto n = (*decoder)->read_full(buf);
    REQUIRE_FALSE(n);
    CHECK(n.error().status == Status::CorruptArchive);
}

TEST_CASE("unknown methods have no decoder", "[codec]")
{
    CHECK_FALSE(CodecRegistry::can_decode(12));
    auto d = CodecRegistry::make_decoder(14, std::make_unique<MemoryStream>());
    REQUIRE_FALSE(d);
    CHECK(d.error().status == Status::UnsupportedMethod);
}
