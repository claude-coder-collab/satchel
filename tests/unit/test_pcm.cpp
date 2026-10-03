// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/pcm/pcm_container.hpp"
#include "codecs/pcm/tags.hpp"
#include "pcm_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace zp;

namespace
{

PcmScan scan(const std::vector<std::uint8_t>& bytes)
{
    MemoryStream s(bytes);
    return scan_pcm(s).value();
}

void check_tiles(const PcmLayout& l)
{
    std::uint64_t pos = 0;
    for (const auto& b : l.blocks)
    {
        CHECK(b.offset == pos);
        pos = b.offset + b.length;
        if (b.audio_header)
            pos += l.audio_size + l.audio_padding;
    }
    CHECK(pos + l.trailing_size == l.file_size);
}

}

TEST_CASE("WAV variants are eligible", "[pcm]")
{
    for (const std::uint16_t bits : { std::uint16_t{ 8 }, std::uint16_t{ 16 }, std::uint16_t{ 24 }, std::uint16_t{ 32 } })
    {
        test::WavSpec spec;
        spec.bits = bits;
        spec.frames = 1001;
        spec.channels = 1;
        auto s = scan(test::make_wav(spec));
        REQUIRE(s.layout);
        CHECK(s.layout->bytes_per_sample == bits / 8);
        CHECK(s.layout->frames == 1001);
        CHECK(s.layout->unsigned_samples == (bits == 8));
        check_tiles(*s.layout);
    }
    test::WavSpec ext;
    ext.extensible = true;
    ext.bits = 20;
    ext.container_bits = 24;
    ext.chunks = { { "bext", test::bext_chunk("desc", "orig", 123), false }, { "odd ", { 1, 2, 3 }, false }, { "LIST", test::text_bytes("INFOINAM\x04\0\0\0abc\0"), true } };
    ext.trailing = test::text_bytes("TRAILING");
    auto s = scan(test::make_wav(ext));
    REQUIRE(s.layout);
    CHECK(s.layout->valid_bits == 20);
    CHECK(s.layout->bytes_per_sample == 3);
    CHECK(s.layout->trailing_size == 8);
    CHECK(s.layout->blocks.size() == 6);
    check_tiles(*s.layout);
}

TEST_CASE("RF64, Wave64, AIFF, AIFF-C and CAF are eligible", "[pcm]")
{
    test::WavSpec w;
    w.frames = 777;
    w.channels = 3;
    w.bits = 24;
    w.chunks = { { "junk", { 1, 2, 3, 4, 5 }, false }, { "cue ", { 9 }, true } };
    for (const auto& bytes : { test::make_rf64(w), test::make_w64(w) })
    {
        auto s = scan(bytes);
        REQUIRE(s.layout);
        CHECK(s.layout->frames == 777);
        check_tiles(*s.layout);
    }
    CHECK(scan(test::make_rf64(w)).layout->container == PcmContainer::Rf64);
    CHECK(scan(test::make_w64(w)).layout->container == PcmContainer::Wave64);

    for (const auto* comp : { "NONE", "sowt", "twos" })
    {
        test::AiffSpec a;
        a.aifc = true;
        a.compression = comp;
        a.frames = 333;
        a.chunks = { { "NAME", test::text_bytes("Take"), false }, { "ANNO", test::text_bytes("odd"), true } };
        auto s = scan(test::make_aiff(a));
        REQUIRE(s.layout);
        CHECK(s.layout->big_endian == (std::string(comp) != "sowt"));
        check_tiles(*s.layout);
    }
    test::AiffSpec plain;
    plain.bits = 12;
    auto s = scan(test::make_aiff(plain));
    REQUIRE(s.layout);
    CHECK(s.layout->bytes_per_sample == 2);
    CHECK(s.layout->sample_rate == 48000);

    for (const bool unknown : { false, true })
    {
        test::CafSpec c;
        c.unknown_data_size = unknown;
        c.little_endian = unknown;
        auto cs = scan(test::make_caf(c));
        REQUIRE(cs.layout);
        CHECK(cs.layout->container == PcmContainer::Caf);
        CHECK(cs.layout->frames == 10000);
        CHECK(cs.layout->big_endian == !unknown);
        check_tiles(*cs.layout);
    }
}

TEST_CASE("every fallback reason is detected from the headers", "[pcm]")
{
    test::WavSpec f;
    f.format_tag = 3;
    f.bits = 32;
    CHECK(scan(test::make_wav(f)).reason == FallbackReason::FloatSamples);
    test::WavSpec fx = f;
    fx.extensible = true;
    CHECK(scan(test::make_wav(fx)).reason == FallbackReason::FloatSamples);
    test::WavSpec adpcm;
    adpcm.format_tag = 2;
    CHECK(scan(test::make_wav(adpcm)).reason == FallbackReason::UnsupportedEncoding);
    test::WavSpec placeholder;
    placeholder.placeholder_sizes = true;
    CHECK(scan(test::make_wav(placeholder)).reason == FallbackReason::NotParsable);
    test::WavSpec fast;
    fast.rate = 2000000;
    fast.frames = 10;
    CHECK(scan(test::make_wav(fast)).reason == FallbackReason::SampleRate);
    test::WavSpec big;
    big.chunks = { { "junk", std::vector<std::uint8_t>((16u << 20) + 10, 0), false } };
    big.frames = 10;
    CHECK(scan(test::make_wav(big)).reason == FallbackReason::ChunkTooLarge);

    auto partial = test::make_wav({});
    partial[40] = static_cast<std::uint8_t>(partial[40] + 1);
    partial.push_back(0);
    CHECK(scan(partial).reason == FallbackReason::NotParsable);
    auto truncated = test::make_wav({});
    truncated.resize(truncated.size() - 100);
    CHECK(scan(truncated).reason == FallbackReason::NotParsable);

    test::AiffSpec compressed;
    compressed.aifc = true;
    compressed.compression = "ima4";
    CHECK(scan(test::make_aiff(compressed)).reason == FallbackReason::UnsupportedEncoding);
    compressed.compression = "fl32";
    CHECK(scan(test::make_aiff(compressed)).reason == FallbackReason::FloatSamples);
    test::CafSpec cf;
    cf.floating = true;
    cf.bits = 32;
    CHECK(scan(test::make_caf(cf)).reason == FallbackReason::FloatSamples);
    test::CafSpec cr;
    cr.rate = 44100.5;
    CHECK(scan(test::make_caf(cr)).reason == FallbackReason::SampleRate);
}

TEST_CASE("non-PCM files are not recognized", "[pcm]")
{
    CHECK_FALSE(scan(test::text_bytes("hello world, this is text")).recognized);
    CHECK_FALSE(scan(test::random_bytes(5000, 1)).recognized);
    CHECK_FALSE(scan({}).recognized);
}

TEST_CASE("sample conversion round trips", "[pcm]")
{
    for (const bool be : { false, true })
    {
        for (const std::uint16_t width : { std::uint16_t{ 1 }, std::uint16_t{ 2 }, std::uint16_t{ 3 }, std::uint16_t{ 4 } })
        {
            PcmLayout l;
            l.bytes_per_sample = width;
            l.channels = 2;
            l.big_endian = be;
            l.unsigned_samples = width == 1 && !be;
            const auto raw = test::audio_bytes(500, 2, width, be, l.unsigned_samples, 4);
            std::vector<std::int32_t> s(1000);
            decode_samples(l, raw, s);
            std::vector<std::uint8_t> back(raw.size());
            encode_samples(l, s, back);
            CHECK(back == raw);
            std::vector<std::int32_t> right(500);
            decode_channel(l, raw, 1, right);
            for (std::size_t i = 0; i < 500; ++i)
                CHECK(right[i] == s[2 * i + 1]);
        }
    }
}

TEST_CASE("mirrored tags from bext, iXML and INFO", "[pcm][tags]")
{
    test::WavSpec w;
    w.channels = 2;
    w.chunks = {
        { "bext", test::bext_chunk("Scene 4 take 2", "Recorder", 172800000), false },
        { "iXML", test::ixml_chunk("Film &amp; TV", "4", "2", { { 1, "Boom" }, { 2, "Lav" } }), false },
        { "LIST", test::text_bytes(std::string("INFOINAM\x06\0\0\0Title\0", 18)), true },
    };
    const auto bytes = test::make_wav(w);
    MemoryStream s(bytes);
    const auto layout = *scan_pcm(s).value().layout;
    std::vector<flac::ForeignRecord> records;
    for (const auto& b : layout.blocks)
        records.push_back({ { 'r', 'i', 'f', 'f' }, std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(b.offset), bytes.begin() + static_cast<std::ptrdiff_t>(b.offset + b.length)) });
    const auto tags = mirror_tags(records, layout, std::nullopt);
    const auto get = [&](const std::string& k) -> std::string {
        for (const auto& [name, value] : tags)
        {
            if (name == k)
                return value;
        }
        return "<missing>";
    };
    CHECK(get("DESCRIPTION") == "Scene 4 take 2");
    CHECK(get("ORIGINATOR") == "Recorder");
    CHECK(get("ORIGINATOR_REFERENCE") == "REF-0001");
    CHECK(get("DATE") == "2026-09-30T12:34:56");
    CHECK(get("TIME_REFERENCE") == "172800000");
    CHECK(get("PROJECT") == "Film & TV");
    CHECK(get("SCENE") == "4");
    CHECK(get("TAKE") == "2");
    CHECK(get("TRACK_NAME_01") == "Boom");
    CHECK(get("TRACK_NAME_02") == "Lav");
    CHECK(get("TITLE") == "Title");
    CHECK(get("TAPE") == "<missing>");
    const auto mono = mirror_tags(records, layout, 2);
    bool track = false;
    for (const auto& [name, value] : mono)
        track |= name == "TRACK_NAME" && value == "Lav";
    CHECK(track);
}
