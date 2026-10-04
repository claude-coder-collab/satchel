// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/flac_codec.hpp"
#include "codecs/flac/flac_format.hpp"
#include "codecs/flac/restore.hpp"
#include "codecs/pcm/pcm_container.hpp"
#include "crypto/hash.hpp"
#include "io/zip/editor.hpp"
#include "io/zip/readme.hpp"
#include "pcm_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

using namespace zp;

namespace
{

struct Case
{
    std::string name;
    std::vector<std::uint8_t> bytes;
};

std::vector<Case> corpus()
{
    std::vector<Case> c;
    for (const std::uint16_t bits : { std::uint16_t{ 8 }, std::uint16_t{ 16 }, std::uint16_t{ 24 }, std::uint16_t{ 32 } })
    {
        test::WavSpec w;
        w.bits = bits;
        w.frames = 300000 + bits;
        w.seed = bits;
        c.push_back({ std::format("pcm{}.wav", bits), test::make_wav(w) });
    }
    test::WavSpec rich;
    rich.extensible = true;
    rich.bits = 24;
    rich.channels = 6;
    rich.frames = 70001;
    rich.chunks = { { "bext", test::bext_chunk("rich", "test", 42), false }, { "iXML", test::ixml_chunk("P", "1", "3", { { 1, "L" } }), false }, { "odd ", { 1, 2, 3 }, false }, { "cue ", test::random_bytes(41, 9), true } };
    rich.trailing = test::text_bytes("junk after RIFF");
    c.push_back({ "rich.wav", test::make_wav(rich) });
    test::WavSpec odd;
    odd.bits = 8;
    odd.channels = 1;
    odd.frames = 12345;
    c.push_back({ "odd8.wav", test::make_wav(odd) });
    test::WavSpec r;
    r.frames = 50000;
    r.chunks = { { "bext", test::bext_chunk("rf", "x", 1), false } };
    c.push_back({ "rf.rf64", test::make_rf64(r) });
    c.push_back({ "w.w64", test::make_w64(r) });
    test::AiffSpec a;
    a.frames = 40001;
    a.bits = 24;
    a.chunks = { { "NAME", test::text_bytes("aiff name"), false }, { "ANNO", test::text_bytes("x"), true } };
    c.push_back({ "a.aiff", test::make_aiff(a) });
    test::AiffSpec ac = a;
    ac.aifc = true;
    ac.compression = "sowt";
    ac.bits = 16;
    c.push_back({ "b.aifc", test::make_aiff(ac) });
    test::CafSpec caf;
    caf.frames = 30000;
    c.push_back({ "c1.caf", test::make_caf(caf) });
    test::CafSpec caf2 = caf;
    caf2.unknown_data_size = true;
    caf2.little_endian = true;
    caf2.bits = 24;
    c.push_back({ "c2.caf", test::make_caf(caf2) });
    return c;
}

const PlanEntry* find(const ArchivePlan& p, const std::string& name)
{
    for (const auto& e : p.entries)
    {
        if (e.output_name == name)
            return &e;
    }
    return nullptr;
}

}

TEST_CASE("every container round-trips bit-exactly through FLAC", "[flac][integration]")
{
    const auto cases = corpus();
    MemoryInputSource in;
    for (const auto& c : cases)
        in.add_file("audio/" + c.name, c.bytes, 1700000000LL * 1'000'000'000);
    const auto plan = test::plan_of(in);
    CHECK(plan.warnings.empty());
    for (const auto& c : cases)
    {
        const auto dot = c.name.rfind('.');
        INFO(c.name);
        const auto* e = find(plan, "audio/" + c.name.substr(0, dot) + ".flac");
        REQUIRE(e);
        CHECK(e->codec == PlanCodec::Flac);
    }
    REQUIRE(find(plan, std::string(readme_file_name)));

    for (const bool seekable : { true, false })
    {
        auto b = test::build(plan, 4, seekable);
        REQUIRE(b.result.status == Status::Ok);
        auto x = test::extract_all(b.zip);
        INFO("seekable " << seekable);
        REQUIRE(x.result.status == Status::Ok);
        for (const auto& c : cases)
        {
            INFO(c.name);
            REQUIRE(x.files.contains("audio/" + c.name));
            CHECK(x.files["audio/" + c.name].data == c.bytes);
        }
        CHECK_FALSE(x.files.contains(std::string(readme_file_name)));
    }
}

TEST_CASE("FLAC output is identical across thread counts and output kinds", "[flac][integration]")
{
    MemoryInputSource in;
    test::WavSpec w;
    w.frames = 600000;
    w.channels = 2;
    in.add_file("long.wav", test::make_wav(w));
    test::WavSpec m;
    m.frames = 50000;
    m.channels = 10;
    in.add_file("multi.wav", test::make_wav(m));
    const auto plan = test::plan_of(in);
    for (const auto& c : plan.conflicts)
        UNSCOPED_INFO("conflict " << c.collision_key << " " << c.detail);
    const auto one = test::build(plan, 1);
    REQUIRE(one.result.status == Status::Ok);
    CHECK(test::build(plan, 8).zip == one.zip);
    const auto stream = test::build(plan, 3, false);
    REQUIRE(stream.result.status == Status::Ok);
    auto a = test::extract_all(one.zip, { .restore_wav = false });
    auto b = test::extract_all(stream.zip, { .restore_wav = false });
    REQUIRE(a.files.size() == b.files.size());
    for (const auto& [name, f] : a.files)
    {
        if (!name.ends_with(".flac"))
            continue;
        MemoryStream sa(f.data);
        MemoryStream sb(b.files[name].data);
        auto ha = flac::read_header(sa).value();
        auto hb = flac::read_header(sb).value();
        CHECK(ha.project->sha256 == hb.project->sha256);
        CHECK(ha.stream_info.md5 == hb.stream_info.md5);
        CHECK(std::vector(f.data.begin() + static_cast<std::ptrdiff_t>(ha.metadata_size), f.data.end())
            == std::vector(b.files[name].data.begin() + static_cast<std::ptrdiff_t>(hb.metadata_size), b.files[name].data.end()));
    }
}

TEST_CASE("parallel FLAC equals a single sequential encoder", "[flac][integration]")
{
    test::WavSpec w;
    w.frames = 3 * 64 * 4096 + 1000;
    w.channels = 2;
    const auto wav = test::make_wav(w);
    MemoryInputSource in;
    in.add_file("t.wav", wav);
    const int level = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    INFO("level " << level);
    PlannerOptions options;
    options.flac_level = level;
    auto b = test::build(test::plan_of(in, options), 8);
    REQUIRE(b.result.status == Status::Ok);
    auto x = test::extract_all(b.zip, { .restore_wav = false });
    const auto& flac_file = x.files["t.flac"].data;
    MemoryStream s(flac_file);
    const auto header = flac::read_header(s).value();

    MemoryStream src(wav);
    const auto layout = *scan_pcm(src).value().layout;
    std::vector<std::int32_t> samples(static_cast<std::size_t>(layout.frames * layout.channels));
    decode_samples(layout, std::span(wav).subspan(static_cast<std::size_t>(layout.audio_offset), static_cast<std::size_t>(layout.audio_size)), samples);
    flac::EncoderSettings settings{ layout.channels, layout.flac_bits(), layout.sample_rate, level };
    const auto sequential = flac::encode_segment(settings, samples, 0).value();
    const std::vector<std::uint8_t> frames(flac_file.begin() + static_cast<std::ptrdiff_t>(header.metadata_size), flac_file.end());
    CHECK(frames == sequential.bytes);
    CHECK(header.stream_info.min_frame_size == sequential.min_frame_size);
    CHECK(header.stream_info.max_frame_size == sequential.max_frame_size);
    std::vector<std::uint8_t> md5_input;
    CHECK(header.stream_info.md5 == Md5::of(md5_bytes(layout, std::span(wav).subspan(static_cast<std::size_t>(layout.audio_offset), static_cast<std::size_t>(layout.audio_size)), md5_input)));
    CHECK(header.project->sha256 == Sha256::of(wav));
}

TEST_CASE("more than 8 channels become a multi-mono group", "[flac][integration]")
{
    test::WavSpec w;
    w.channels = 12;
    w.bits = 24;
    w.frames = 70000;
    w.chunks = { { "iXML", test::ixml_chunk("P", "S", "T", { { 3, "Ch three" } }), false } };
    const auto wav = test::make_wav(w);
    MemoryInputSource in;
    in.add_file("dir/Take2.WAV", wav);
    const auto plan = test::plan_of(in);
    REQUIRE(plan.entries.size() == 13);
    CHECK(plan.entries[0].output_name == "dir/Take2_ch01.flac");
    CHECK(plan.entries[11].output_name == "dir/Take2_ch12.flac");
    CHECK(plan.entries[0].group_id == plan.entries[11].group_id);
    auto b = test::build(plan);
    REQUIRE(b.result.status == Status::Ok);

    auto x = test::extract_all(b.zip);
    for (const auto& o : x.result.outcomes)
        UNSCOPED_INFO("outcome " << o.target << " " << o.message);
    REQUIRE(x.result.status == Status::Ok);
    CHECK(x.files.size() == 1);
    CHECK(x.files["dir/Take2.WAV"].data == wav);

    auto raw = test::extract_all(b.zip, { .restore_wav = false });
    CHECK(raw.files.size() == 12);
    MemoryStream s(raw.files["dir/Take2_ch03.flac"].data);
    auto h = flac::read_header(s).value();
    CHECK(h.stream_info.channels == 1);
    CHECK(h.project->channel_index == 3);
    CHECK(h.project->private_data.empty());
    CHECK(h.comments->get("TRACK_NAME") == "Ch three");
    CHECK(h.comments->get("CHANNELS") == "12");
}

TEST_CASE("incomplete multi-mono groups are refused", "[flac][integration]")
{
    test::WavSpec w;
    w.channels = 9;
    w.frames = 5000;
    MemoryInputSource in;
    in.add_file("t.wav", test::make_wav(w));
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    MemoryStream src(b.zip);
    ArchiveEditor ed(test::shared_context());
    REQUIRE(ed.open(src));
    REQUIRE(ed.remove(4));
    MemoryStream out;
    ProgressSink progress;
    REQUIRE(ed.commit(out, progress).status == Status::Ok);
    auto x = test::extract_all(out.data());
    for (const auto& i : x.plan.issues)
        UNSCOPED_INFO("issue " << i.name << " " << i.detail);
    for (const auto& i : x.plan.items)
        UNSCOPED_INFO("item " << i.target);
    CHECK(x.result.status == Status::IncompleteGroup);
    CHECK(x.plan.items.empty());
    std::size_t errors = 0;
    for (const auto& i : x.plan.issues)
        errors += i.kind == ExtractIssueKind::IncompleteGroup ? 1 : 0;
    CHECK(errors == 8);
}

TEST_CASE("ineligible PCM is stored with a warning", "[flac][integration]")
{
    test::WavSpec f;
    f.format_tag = 3;
    f.bits = 32;
    f.frames = 5000;
    MemoryInputSource in;
    in.add_file("float.wav", test::make_wav(f));
    const auto plan = test::plan_of(in);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].output_name == "float.wav");
    CHECK(plan.entries[0].flac_fallback_reason == FallbackReason::FloatSamples);
    REQUIRE(plan.warnings.size() == 1);
    CHECK(plan.warnings[0].kind == WarningKind::FlacFallback);
    auto b = test::build(plan);
    REQUIRE(b.result.status == Status::Ok);
    CHECK(b.result.per_entry[0].method == ZipMethod::Store);
}

TEST_CASE("collisions with generated FLAC and restored names", "[flac][planner]")
{
    const auto wav = test::make_wav({});
    SECTION("take1.wav + take1.flac")
    {
        MemoryInputSource in;
        in.add_file("take1.wav", wav);
        in.add_file("take1.flac", test::text_bytes("not really flac"));
        auto p = test::plan_of(in);
        REQUIRE_FALSE(p.executable());
        auto r = ArchivePlanner::replan(p, { { 0, ResolutionAction::DisableFlac, {} } });
        REQUIRE(r);
        CHECK(r->executable());
        CHECK_FALSE(find(*r, std::string(readme_file_name)));
    }
    SECTION("restored name collides with a stored file")
    {
        test::WavSpec f;
        f.format_tag = 3;
        f.bits = 32;
        MemoryInputSource in;
        in.add_file("TAKE1.WAV", wav);
        in.add_file("take1.wav", test::make_wav(f));
        auto p = test::plan_of(in);
        CHECK_FALSE(p.executable());
        auto r = ArchivePlanner::replan(p, { { 0, ResolutionAction::Rename, "take1b.flac" } });
        REQUIRE(r);
        CHECK(r->executable());
        CHECK(r->entries[0].restored_name() == "take1b.WAV");
    }
    SECTION("readme name collides with an input file")
    {
        MemoryInputSource in;
        in.add_file("a.wav", wav);
        in.add_file(std::string(readme_file_name), test::text_bytes("mine"));
        auto p = test::plan_of(in);
        REQUIRE_FALSE(p.executable());
        const auto readme_index = p.entries.size() - 1;
        CHECK_FALSE(ArchivePlanner::replan(p, { { readme_index, ResolutionAction::Skip, {} } }));
        auto r = ArchivePlanner::replan(p, { { 1, ResolutionAction::Rename, "my readme.txt" } });
        REQUIRE(r);
        CHECK(r->executable());
    }
    SECTION("a multi-mono group is resolved as one unit")
    {
        test::WavSpec w;
        w.channels = 9;
        w.frames = 100;
        MemoryInputSource in;
        in.add_file("m.wav", test::make_wav(w));
        in.add_file("m_ch05.flac", test::text_bytes("x"));
        auto p = test::plan_of(in);
        REQUIRE_FALSE(p.executable());
        auto renamed = ArchivePlanner::replan(p, { { 0, ResolutionAction::Rename, "n_ch01.flac" } });
        REQUIRE(renamed);
        CHECK(renamed->executable());
        CHECK(renamed->entries[8].output_name == "n_ch09.flac");
        auto skipped = ArchivePlanner::replan(p, { { 3, ResolutionAction::Skip, {} } });
        REQUIRE(skipped);
        CHECK(skipped->entries.size() == 1);
        auto stored = ArchivePlanner::replan(p, { { 2, ResolutionAction::DisableFlac, {} } });
        REQUIRE(stored);
        CHECK(stored->entries.size() == 2);
        CHECK(stored->entries[0].output_name == "m.wav");
    }
}

TEST_CASE("generated readme content", "[flac][readme]")
{
    MemoryInputSource in;
    in.add_file("take1.wav", test::make_wav({}), 5'000'000'000);
    test::WavSpec m;
    m.channels = 16;
    m.frames = 100;
    in.add_file("take2.wav", test::make_wav(m), 9'000'000'000);
    in.add_file("notes.txt", test::text_bytes("n"), 7'000'000'000);
    const auto plan = test::plan_of(in);
    const auto text = render_readme(plan, {});
    CHECK(text.find("take1.flac                          -> take1.wav\r\n") != std::string::npos);
    CHECK(text.find("take2_ch01.flac ... take2_ch16.flac -> take2.wav (16 channels)\r\n") != std::string::npos);
    CHECK(text.find("created with Satchel") != std::string::npos);
    CHECK(text.find("\n\n") == std::string::npos);
    CHECK(text.find('\r') != std::string::npos);
    const auto& readme = plan.entries.back();
    CHECK(readme.codec == PlanCodec::Generated);
    CHECK(readme.item.mtime_seconds() == 9);

    auto b = test::build(plan);
    REQUIRE(b.result.status == Status::Ok);
    auto all = test::extract_all(b.zip, { .include_readme = true });
    REQUIRE(all.files.contains(std::string(readme_file_name)));
    CHECK(all.files[std::string(readme_file_name)].data == test::text_bytes(text));
    MemoryStream s(b.zip);
    auto r = ArchiveReader::open(s).value();
    CHECK(r->metadata()->readme_name == readme_file_name);
    const auto custom = render_readme(plan, {}, "Get {APP_NAME} at {DEARCHIVER_URL}\n{FILE_LIST}");
    CHECK(custom.starts_with("Get Satchel at https://"));
}

TEST_CASE("tampered FLAC fails the SHA-256 check and leaves no output", "[flac][integration]")
{
    MemoryInputSource in;
    in.add_file("t.wav", test::make_wav({}));
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    MemoryStream s(b.zip);
    auto r = ArchiveReader::open(s).value();
    const auto h = r->flac_header(0).value();
    auto zip = b.zip;
    const auto at = std::search(zip.begin(), zip.end(), h.project->sha256.begin(), h.project->sha256.end());
    REQUIRE(at != zip.end());
    *at ^= 0xFF;
    auto x = test::extract_all(zip);
    CHECK(x.result.status != Status::Ok);
    CHECK(x.files.empty());
}

TEST_CASE("flac command-line tool interoperability", "[flac][interop]")
{
    const auto flac_tool = test::find_tool("flac");
    if (!flac_tool)
        SKIP("flac not installed");
    test::TempDir dir;
    test::WavSpec w;
    w.frames = 100000;
    w.chunks = { { "bext", test::bext_chunk("d", "o", 7), false }, { "LIST", test::text_bytes("INFOabc"), true } };
    const auto wav = test::make_wav(w);
    MemoryInputSource in;
    in.add_file("take.wav", wav);
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    auto x = test::extract_all(b.zip, { .restore_wav = false });
    const auto flac_path = dir.write("take.flac", x.files["take.flac"].data);
    const auto q = [](const std::filesystem::path& p) { return "\"" + p.string() + "\""; };
    CHECK(test::run(std::format("{} -s -t {}", q(*flac_tool), q(flac_path))) == 0);
    const auto restored = dir.path() / "restored.wav";
    REQUIRE(test::run(std::format("{} -s -d --keep-foreign-metadata -o {} {}", q(*flac_tool), q(restored), q(flac_path))) == 0);
    CHECK(test::read_file(restored) == wav);

    const auto source = dir.write("source.wav", wav);
    const auto theirs = dir.path() / "theirs.flac";
    REQUIRE(test::run(std::format("{} -s --keep-foreign-metadata -o {} {}", q(*flac_tool), q(theirs), q(source))) == 0);
    const auto their_bytes = test::read_file(theirs);
    MemoryStream hs(their_bytes);
    const auto header = flac::read_header(hs).value();
    CHECK_FALSE(header.project);
    std::vector<std::uint8_t> rebuilt;
    auto r = flac::restore(
        { header },
        [&](std::size_t) -> Result<std::unique_ptr<IChunkedStream>> { return std::make_unique<MemoryStream>(their_bytes); },
        [&](std::span<const std::uint8_t> bytes) -> VoidResult {
            rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.end());
            return {};
        }
    );
    if (!r)
        UNSCOPED_INFO("restore " << r.error().message);
    REQUIRE(r);
    CHECK(rebuilt == wav);
}

// Encoder output must not depend on platform or CPU. CRC-32 of the frames of a fixed input.
TEST_CASE("FLAC output matches the golden value", "[flac][golden]")
{
    test::WavSpec w;
    w.frames = 200000;
    w.channels = 2;
    w.bits = 24;
    MemoryInputSource in;
    in.add_file("g.wav", test::make_wav(w));
    for (const int level : { 0, 5, 8 })
    {
        PlannerOptions o;
        o.flac_level = level;
        auto b = test::build(in, o);
        REQUIRE(b.result.status == Status::Ok);
        auto x = test::extract_all(b.zip, { .restore_wav = false });
        const auto& f = x.files["g.flac"].data;
        MemoryStream s(f);
        const auto h = flac::read_header(s).value();
        const auto crc = crc32_update(0, std::span(f).subspan(static_cast<std::size_t>(h.metadata_size)));
        UNSCOPED_INFO("level " << level << " golden crc " << std::hex << crc);
        const std::uint32_t expected = level == 0 ? 0xf912efedu : level == 5 ? 0x2e551478u
                                                                             : 0x9f28c2b6u;
        CHECK(crc == expected);
    }
}

TEST_CASE("the editor regenerates or removes the readme", "[flac][editor]")
{
    MemoryInputSource in;
    in.add_file("a.wav", test::make_wav({}));
    in.add_file("notes.txt", test::text_bytes("n"));
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);

    MemoryStream src(b.zip);
    ArchiveEditor ed(test::shared_context());
    REQUIRE(ed.open(src));
    auto view = ed.plan();
    REQUIRE(view.entries.size() == 3);
    CHECK(view.entries[0].kept_restorable);
    CHECK(view.entries.back().codec == PlanCodec::Generated);

    SECTION("adding audio lists both files")
    {
        MemoryInputSource more;
        test::WavSpec w;
        w.channels = 9;
        w.frames = 500;
        more.add_file("b.wav", test::make_wav(w));
        REQUIRE(ed.add(more));
        MemoryStream out;
        ProgressSink progress;
        REQUIRE(ed.commit(out, progress).status == Status::Ok);
        auto x = test::extract_all(out.data(), { .include_readme = true });
        REQUIRE(x.result.status == Status::Ok);
        const auto& text = x.files[std::string(readme_file_name)].data;
        const std::string readme(text.begin(), text.end());
        CHECK(readme.find("a.flac") != std::string::npos);
        CHECK(readme.find("b_ch01.flac ... b_ch09.flac -> b.wav (9 channels)") != std::string::npos);
        CHECK(x.files.contains("a.wav"));
        CHECK(x.files.contains("b.wav"));
    }
    SECTION("removing the last converted file removes the readme")
    {
        REQUIRE(ed.remove(0));
        MemoryStream out;
        ProgressSink progress;
        REQUIRE(ed.commit(out, progress).status == Status::Ok);
        auto x = test::extract_all(out.data(), { .include_readme = true });
        CHECK(x.files.size() == 1);
        CHECK(x.files.contains("notes.txt"));
    }
}

TEST_CASE("64 channels and sample rates up to 768 kHz round-trip", "[flac][integration]")
{
    struct Case
    {
        std::uint16_t channels;
        std::uint32_t rate;
        std::uint16_t bits;
        std::size_t members;
    };
    for (const auto& c : { Case{ 64, 48000, 24, 64 }, Case{ 9, 192000, 24, 9 }, Case{ 16, 96000, 16, 16 }, Case{ 2, 768000, 24, 1 }, Case{ 1, 768000, 32, 1 } })
    {
        CAPTURE(c.channels, c.rate, c.bits);
        test::WavSpec w;
        w.channels = c.channels;
        w.rate = c.rate;
        w.bits = c.bits;
        w.frames = 6000;
        w.extensible = c.channels > 2 || c.bits > 16;
        const auto wav = test::make_wav(w);
        MemoryInputSource in;
        in.add_file("x.wav", wav);
        const auto plan = test::plan_of(in);
        const auto flac_entries = std::ranges::count_if(plan.entries, [](const auto& e) { return e.codec != PlanCodec::General && e.codec != PlanCodec::Generated; });
        CHECK(static_cast<std::size_t>(flac_entries) == c.members);
        const auto built = test::build(plan);
        REQUIRE(built.result.status == Status::Ok);
        const auto x = test::extract_all(built.zip);
        REQUIRE(x.result.status == Status::Ok);
        CHECK(x.files.at("x.wav").data == wav);
        const auto raw = test::extract_all(built.zip, { .restore_wav = false });
        const auto& first = raw.files.at(c.members == 1 ? std::string("x.flac") : std::string("x_ch01.flac")).data;
        MemoryStream s(first);
        const auto h = flac::read_header(s).value();
        CHECK(h.stream_info.sample_rate == c.rate);
    }
}
