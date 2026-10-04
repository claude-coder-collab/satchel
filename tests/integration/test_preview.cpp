// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/preview.hpp"
#include "pcm_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>

using namespace zp;

TEST_CASE("zip preview summarises entries, savings and restorable audio", "[preview]")
{
    test::WavSpec w;
    w.channels = 12;
    w.bits = 24;
    w.frames = 20000;
    test::WavSpec stereo;
    stereo.frames = 30000;
    MemoryInputSource in;
    in.add_directory("docs");
    in.add_file("docs/<notes>.txt", test::text_like(50000, 1));
    in.add_file("multi.wav", test::make_wav(w));
    in.add_file("take.wav", test::make_wav(stereo));
    const auto built = test::build(in);
    REQUIRE(built.result.status == Status::Ok);

    MemoryStream s(built.zip);
    auto p = make_preview(s, "a & b.zip").value();
    CHECK(p.kind == Preview::Kind::Zip);
    CHECK(p.created_by.has_value());
    CHECK(p.entry_count == 1 + 1 + 12 + 1 + 1);
    CHECK(p.file_count == p.entry_count - 1);
    CHECK(p.restorable_audio == 2);
    CHECK_FALSE(p.restorable_audio_partial);
    CHECK(p.packed_size < p.total_size);
    const auto take = std::ranges::find(p.entries, std::string("take.flac"), &Preview::Entry::name);
    REQUIRE(take != p.entries.end());
    CHECK(take->method == "FLAC");
    CHECK(take->restores_to == "take.wav");

    const auto json = preview_json(p);
    CHECK(json.find("\"kind\":\"zip\"") != std::string::npos);
    CHECK(json.find("\"restorable_audio\":2") != std::string::npos);
    const auto html = preview_html(p);
    CHECK(html.find("<h1>a &amp; b.zip</h1>") != std::string::npos);
    CHECK(html.find("&lt;notes&gt;.txt") != std::string::npos);
    CHECK(html.find("<notes>") == std::string::npos);
    CHECK(html.find("2 audio files restorable") != std::string::npos);
}

TEST_CASE("zip preview caps the listing and the FLAC probing", "[preview]")
{
    MemoryInputSource in;
    for (int i = 0; i < 30; ++i)
        in.add_file(std::format("f{:02}.txt", i), test::text_bytes("x"));
    test::WavSpec w;
    w.frames = 5000;
    for (int i = 0; i < 3; ++i)
        in.add_file(std::format("t{}.wav", i), test::make_wav(w));
    const auto built = test::build(in);
    MemoryStream s(built.zip);
    auto p = make_preview(s, "many.zip", { .max_entries = 10, .max_probed = 2 }).value();
    CHECK(p.entries.size() == 10);
    CHECK(p.truncated);
    CHECK(p.entry_count == 34);
    CHECK(p.restorable_audio == 2);
    CHECK(p.restorable_audio_partial);
    CHECK(preview_html(p).find("… and 24 more entries") != std::string::npos);
    CHECK(preview_html(p).find("at least 2 audio files") != std::string::npos);
}

TEST_CASE("FLAC preview shows format, origin and tags without decoding", "[preview]")
{
    test::WavSpec w;
    w.channels = 2;
    w.bits = 24;
    w.rate = 96000;
    w.frames = 96000 * 2;
    MemoryInputSource in;
    in.add_file("Take 1.wav", test::make_wav(w));
    const auto built = test::build(in);
    const auto raw = test::extract_all(built.zip, { .restore_wav = false });
    MemoryStream s(raw.files.at("Take 1.flac").data);
    auto p = make_preview(s, "Take 1.flac").value();
    REQUIRE(p.kind == Preview::Kind::Flac);
    REQUIRE(p.flac);
    CHECK(p.flac->sample_rate == 96000);
    CHECK(p.flac->bits_per_sample == 24);
    CHECK(p.flac->total_samples == 192000);
    CHECK(p.flac->original_name == "Take 1.wav");
    CHECK(p.flac->container == "WAV");
    CHECK(p.flac->layout == "standard");
    CHECK(std::ranges::any_of(p.flac->chunks, [](const auto& c) { return c.audio; }));
    CHECK(std::ranges::any_of(p.flac->tags, [](const auto& t) { return t.first == "ENCODER"; }));
    const auto html = preview_html(p);
    CHECK(html.find("96000 Hz · 24-bit · 2 channels · 0:02.000") != std::string::npos);
    CHECK(html.find("Take 1.wav (WAV)") != std::string::npos);
    CHECK(preview_json(p).find("\"kind\":\"flac\"") != std::string::npos);
}

TEST_CASE("plain-text preview pieces", "[preview]")
{
    Preview p;
    p.file_count = 2;
    p.entry_count = 3;
    p.total_size = 2000;
    p.packed_size = 1000;
    p.restorable_audio = 1;
    p.created_by = "Satchel 1.0";
    CHECK(preview_summary_text(p) == "2 files · 2.0 KB → 1.0 KB (50% smaller) · 1 audio file restorable · created with Satchel 1.0");
    CHECK(preview_size_text(999) == "999 B");
    Preview::Flac f;
    f.sample_rate = 48000;
    f.bits_per_sample = 16;
    f.channels = 1;
    f.total_samples = 48000 * 61;
    f.original_name = "a.wav";
    f.container = "WAV";
    f.layout = "multi_mono";
    f.channel_index = 2;
    f.channel_count = 4;
    f.tags = { { "TRACK_NAME", "Boom" } };
    const auto rows = preview_flac_rows(f);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].second == "48000 Hz · 16-bit · 1 channel · 1:01.000");
    CHECK(rows[1].second == "a.wav (WAV)");
    CHECK(rows[2].second == "channel 2 of 4");
    CHECK(rows[3].first == "TRACK_NAME");
}

TEST_CASE("preview refuses files that are neither zip nor FLAC", "[preview]")
{
    const auto junk = test::text_bytes("just some text, not an archive");
    MemoryStream s(junk);
    CHECK_FALSE(make_preview(s, "x.txt"));
}
