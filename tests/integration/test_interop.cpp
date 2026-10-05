// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/file_system.hpp"
#include "pcm_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <format>
#include <string>

using namespace zp;

namespace
{

std::filesystem::path write_archive(const test::TempDir& dir, const std::string& name, bool seekable)
{
    MemoryInputSource in;
    in.add_directory("set");
    in.add_directory("set/empty");
    in.add_file("set/text.txt", test::text_like(200000, 3));
    in.add_file("set/noise.bin", test::random_bytes(5u << 20, 4));
    in.add_file("set/\xC3\xA9t\xC3\xA9.txt", test::text_bytes("utf8"));
    auto b = test::build(test::plan_of(in), 4, seekable);
    REQUIRE(b.result.status == Status::Ok);
    return dir.write(name, b.zip);
}

std::string q(const std::filesystem::path& p)
{
    return "\"" + p.string() + "\"";
}

}

TEST_CASE("extracted FLACs decode in ffmpeg and show their tags", "[interop]")
{
    const auto ffprobe = test::find_tool("ffprobe");
    const auto ffmpeg = test::find_tool("ffmpeg");
    if (!ffprobe || !ffmpeg)
        SKIP("ffprobe/ffmpeg not installed");
    test::WavSpec six;
    six.channels = 6;
    six.bits = 24;
    six.frames = 48000;
    six.extensible = true;
    six.chunks = { { "bext", test::bext_chunk("Scene 4 take 2", "Recorder", 1234567), false } };
    test::WavSpec ten;
    ten.channels = 10;
    ten.frames = 24000;
    ten.chunks = { { "iXML", test::ixml_chunk("P", "S", "T", { { 2, "Boom" } }), false } };
    MemoryInputSource in;
    in.add_file("six.wav", test::make_wav(six));
    in.add_file("ten.wav", test::make_wav(ten));
    const auto raw = test::extract_all(test::build(in).zip, { .restore_wav = false });
    test::TempDir dir;
    const auto probe = [&](const std::string& name) {
        const auto path = dir.write(name, raw.files.at(name).data);
        CHECK(test::run(std::format("\"{}\" -v error -i {} -f null -", *ffmpeg, q(path))) == 0);
        const auto json = test::run_capture(std::format("\"{}\" -v error -show_entries stream=channels,sample_rate:format_tags -of json {}", *ffprobe, q(path)));
        REQUIRE(json);
        return *json;
    };
    const auto multichannel = probe("six.flac");
    CHECK(multichannel.find("\"channels\": 6") != std::string::npos);
    CHECK(multichannel.find("Scene 4 take 2") != std::string::npos);
    const auto member = probe("ten_ch02.flac");
    CHECK(member.find("\"channels\": 1") != std::string::npos);
    CHECK(member.find("Boom") != std::string::npos);
    CHECK(member.find("Satchel") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("Info-ZIP unzip accepts our archives", "[interop]")
{
    const auto unzip = test::find_tool("unzip");
    if (!unzip)
        SKIP("unzip not installed");
    test::TempDir dir;
    for (const bool seekable : { true, false })
    {
        const auto zip = write_archive(dir, seekable ? "seek.zip" : "stream.zip", seekable);
        CHECK(test::run(std::format("{} -tq {}", q(*unzip), q(zip))) == 0);
        const auto out = dir.path() / (seekable ? "out-seek" : "out-stream");
        REQUIRE(test::run(std::format("{} -q {} -d {}", q(*unzip), q(zip), q(out))) == 0);
        CHECK(test::read_file(out / "set" / "noise.bin") == test::random_bytes(5u << 20, 4));
        CHECK(std::filesystem::is_directory(out / "set" / "empty"));
    }
}

TEST_CASE("7-Zip accepts our archives", "[interop]")
{
    auto sz = test::find_tool("7z");
    if (!sz)
        sz = test::find_tool("7zz");
    if (!sz)
        SKIP("7-Zip not installed");
    test::TempDir dir;
    for (const bool seekable : { true, false })
    {
        const auto zip = write_archive(dir, seekable ? "seek.zip" : "stream.zip", seekable);
        CHECK(test::run(std::format("{} t {} > {}", q(*sz), q(zip), q(dir.path() / "log.txt"))) == 0);
    }
}

TEST_CASE("archives made by Info-ZIP zip are read and extracted", "[interop]")
{
    const auto zip_tool = test::find_tool("zip");
    if (!zip_tool)
        SKIP("zip not installed");
    test::TempDir dir;
    dir.write("src/a.txt", test::text_like(50000, 1));
    dir.write("src/b.bin", test::random_bytes(10000, 2));
    dir.mkdir("src/empty");
    #ifndef _WIN32
    std::filesystem::create_symlink("a.txt", dir.path() / "src" / "link");
    #endif
    const auto zip = dir.path() / "infozip.zip";
    REQUIRE(test::run(std::format("cd {} && {} -qry {} src", q(dir.path()), q(*zip_tool), q(zip))) == 0);
    auto x = test::extract_all(test::read_file(zip));
    CHECK(x.files["src/a.txt"].data == test::text_like(50000, 1));
    CHECK(x.files["src/b.bin"].data == test::random_bytes(10000, 2));
    CHECK(x.dirs.contains("src/empty"));
    #ifndef _WIN32
    bool warned = false;
    for (const auto& i : x.plan.issues)
        warned |= i.kind == ExtractIssueKind::SymlinkSkipped;
    CHECK(warned);
    #endif
}
#endif
