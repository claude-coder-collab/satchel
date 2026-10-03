// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "test_support.hpp"

#include "codecs/codec.hpp"
#include "common/bytes.hpp"
#include "io/file_system.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <random>

namespace zp::test
{

std::vector<std::uint8_t> random_bytes(std::size_t n, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out(n);
    for (auto& b : out)
        b = static_cast<std::uint8_t>(rng());
    return out;
}

std::vector<std::uint8_t> text_bytes(std::string_view s)
{
    return { s.begin(), s.end() };
}

std::vector<std::uint8_t> text_like(std::size_t n, std::uint32_t seed)
{
    static constexpr std::array<std::string_view, 8> words{ "take ", "scene ", "audio ", "track ", "mix ", "field ", "boom ", "lav\n" };
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out;
    out.reserve(n);
    while (out.size() < n)
    {
        const auto w = words[rng() % words.size()];
        out.insert(out.end(), w.begin(), w.end());
    }
    out.resize(n);
    return out;
}

TempDir::TempDir()
{
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() / std::format("zp-test-{:08x}{:08x}", rd(), rd());
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir()
{
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::filesystem::path TempDir::write(const std::string& rel, const std::vector<std::uint8_t>& data) const
{
    const auto p = path_ / path_from_utf8(rel);
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return p;
}

std::filesystem::path TempDir::mkdir(const std::string& rel) const
{
    const auto p = path_ / path_from_utf8(rel);
    std::filesystem::create_directories(p);
    return p;
}

Context& shared_context(int threads)
{
    static std::map<int, std::unique_ptr<Context>> contexts;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    auto& c = contexts[threads];
    if (!c)
        c = std::make_unique<Context>(threads, 0);
    return *c;
}

ArchivePlan plan_of(InputSource& input, PlannerOptions options)
{
    auto plan = ArchivePlanner(options).plan(input);
    REQUIRE(plan);
    return std::move(*plan);
}

Built build(const ArchivePlan& plan, int threads, bool seekable, std::uint64_t memory_budget)
{
    Built b;
    MemoryStream out;
    ProgressSink progress;
    std::unique_ptr<Context> own;
    Context* ctx = &shared_context(threads);
    if (memory_budget != 0)
    {
        own = std::make_unique<Context>(threads, memory_budget);
        ctx = own.get();
    }
    if (seekable)
    {
        ArchiveBuilder builder(out, *ctx);
        b.result = builder.execute(plan, progress);
    }
    else
    {
        NonSeekableStream ns(out);
        ArchiveBuilder builder(ns, *ctx);
        b.result = builder.execute(plan, progress);
    }
    b.zip = std::move(out.data());
    return b;
}

Built build(InputSource& input, PlannerOptions options, int threads, bool seekable)
{
    return build(plan_of(input, options), threads, seekable);
}

Extracted extract_all(const std::vector<std::uint8_t>& zip, ExtractOptions options)
{
    Extracted x;
    MemoryStream in(zip);
    auto reader = ArchiveReader::open(in);
    REQUIRE(reader);
    MemoryOutputSink sink;
    ArchiveExtractor extractor(**reader, sink, shared_context(), options);
    auto plan = extractor.plan_extraction({});
    REQUIRE(plan);
    x.plan = *plan;
    ProgressSink progress;
    x.result = extractor.execute(*plan, progress);
    x.files = sink.files();
    x.dirs = sink.directories();
    return x;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

std::optional<std::string> find_tool(const std::string& name)
{
    const char* path = std::getenv("PATH");
    if (!path)
        return std::nullopt;
#ifdef _WIN32
    constexpr char sep = ';';
    const std::array<std::string, 2> exts{ ".exe", "" };
#else
    constexpr char sep = ':';
    const std::array<std::string, 1> exts{ "" };
#endif
    std::string_view rest(path);
    while (!rest.empty())
    {
        const auto pos = rest.find(sep);
        const auto dir = rest.substr(0, pos);
        for (const auto& ext : exts)
        {
            const auto candidate = std::filesystem::path(std::string(dir)) / (name + ext);
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate.string();
        }
        if (pos == std::string_view::npos)
            break;
        rest.remove_prefix(pos + 1);
    }
    return std::nullopt;
}

std::vector<std::uint8_t> craft_zip(const std::vector<RawEntry>& entries, const std::string& comment)
{
    ByteWriter out;
    ByteWriter cd;
    for (const auto& e : entries)
    {
        const auto offset = static_cast<std::uint32_t>(out.data().size());
        const auto crc = e.crc.value_or(crc32_update(0, e.data));
        const auto usize = static_cast<std::uint32_t>(e.uncompressed.value_or(e.data.size()));
        const auto write_common = [&](ByteWriter& w) {
            w.u16le(20);
            w.u16le(e.flags);
            w.u16le(e.method);
            w.u16le(0);
            w.u16le(0x5021);
            w.u32le(crc);
            w.u32le(static_cast<std::uint32_t>(e.data.size()));
            w.u32le(usize);
            w.u16le(static_cast<std::uint16_t>(e.name.size()));
            w.u16le(static_cast<std::uint16_t>(e.extra.size()));
        };
        out.u32le(0x04034b50);
        write_common(out);
        out.str(e.name);
        out.bytes(e.extra);
        out.bytes(e.data);

        cd.u32le(0x02014b50);
        cd.u16le(e.version_made_by);
        write_common(cd);
        cd.u16le(0);
        cd.u16le(0);
        cd.u16le(0);
        cd.u32le(e.external_attributes);
        cd.u32le(offset);
        cd.str(e.name);
        cd.bytes(e.extra);
    }
    const auto cd_offset = static_cast<std::uint32_t>(out.data().size());
    const auto cd_size = static_cast<std::uint32_t>(cd.data().size());
    out.bytes(cd.data());
    out.u32le(0x06054b50);
    out.u16le(0);
    out.u16le(0);
    out.u16le(static_cast<std::uint16_t>(entries.size()));
    out.u16le(static_cast<std::uint16_t>(entries.size()));
    out.u32le(cd_size);
    out.u32le(cd_offset);
    out.u16le(static_cast<std::uint16_t>(comment.size()));
    out.str(comment);
    return out.take();
}

int run(const std::string& command)
{
    return std::system(command.c_str());
}

}
