// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/flac_codec.hpp"
#include "codecs/flac/flac_format.hpp"
#include "common/bytes.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace zp;
using namespace zp::flac;

TEST_CASE("FLAC CRCs", "[flac]")
{
    CHECK(crc8(as_bytes("123456789")) == 0xF4);
    CHECK(crc16(as_bytes("123456789")) == 0xFEE8);
}

TEST_CASE("STREAMINFO round trip", "[flac]")
{
    StreamInfo s;
    s.min_frame_size = 14;
    s.max_frame_size = 12345;
    s.sample_rate = 768000;
    s.channels = 8;
    s.bits_per_sample = 32;
    s.total_samples = (1ull << 36) - 1;
    s.md5[0] = 0xAB;
    s.md5[15] = 0xCD;
    const auto bytes = s.serialize();
    const auto back = StreamInfo::parse(bytes).value();
    CHECK(back.min_block_size == 4096);
    CHECK(back.min_frame_size == 14);
    CHECK(back.max_frame_size == 12345);
    CHECK(back.sample_rate == 768000);
    CHECK(back.channels == 8);
    CHECK(back.bits_per_sample == 32);
    CHECK(back.total_samples == (1ull << 36) - 1);
    CHECK(back.md5 == s.md5);
}

TEST_CASE("project block layout", "[flac]")
{
    ProjectBlock p;
    p.layout = Layout::MultiMonoMember;
    p.group_id.fill(7);
    p.channel_index = 3;
    p.channel_count = 16;
    p.original_name = "Take 1.WAV";
    p.private_data = { 1, 2, 3 };
    p.trailing = { 9 };
    p.sha256.fill(0x55);
    const auto bytes = p.serialize();
    CHECK(bytes.size() == 4 + 1 + 1 + 16 + 2 + 2 + 2 + 10 + 4 + 3 + 4 + 1 + 32);
    CHECK(p.sha_offset() == bytes.size() - 32);
    CHECK(bytes[4] == 1);
    CHECK(bytes[5] == 1);
    CHECK(bytes[22] == 0);
    CHECK(bytes[23] == 3);
    const auto back = ProjectBlock::parse(bytes).value();
    CHECK(back.original_name == p.original_name);
    CHECK(back.channel_index == 3);
    CHECK(back.channel_count == 16);
    CHECK(back.private_data == p.private_data);
    CHECK(back.trailing == p.trailing);
    CHECK(back.sha256 == p.sha256);
    auto truncated = bytes;
    truncated.pop_back();
    CHECK_FALSE(ProjectBlock::parse(truncated));
}

TEST_CASE("private storage round trip", "[flac]")
{
    std::vector<ForeignRecord> records = { { { 'c', 'a', 'f', 'f' }, test::text_bytes("caff\x00\x01\x00\x00") }, { { 'c', 'a', 'f', 'f' }, test::random_bytes(5000, 3) } };
    const auto packed = pack_private(records).value();
    const auto back = unpack_private(packed).value();
    REQUIRE(back.size() == 2);
    CHECK(back[1].bytes == records[1].bytes);
    CHECK(back[0].id == records[0].id);
}

TEST_CASE("segments renumber frames continuously", "[flac]")
{
    EncoderSettings s;
    s.channels = 2;
    s.bits_per_sample = 16;
    s.sample_rate = 44100;
    std::vector<std::int32_t> samples(2 * 10000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<std::int32_t>(std::lround(8000 * std::sin(static_cast<double>(i) * 0.01)));
    auto seg = encode_segment(s, samples, 128).value();
    CHECK(seg.frame_count == 3);
    CHECK(frame_number(seg.bytes) == 128u);
    auto zero = encode_segment(s, samples, 0).value();
    CHECK(frame_number(zero.bytes) == 0u);
    CHECK(zero.bytes.size() + 3 == seg.bytes.size());
    CHECK(seg.min_frame_size > 0);
    CHECK(seg.max_frame_size >= seg.min_frame_size);
}
