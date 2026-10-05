// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Throughput benchmark (zip design doc 8.6): hashing, building and extracting generated audio,
// text and random data, with 1 and all hardware threads (or only --threads N).
// Usage: zp_bench [--size MiB] [--threads N] [--case TEXT] [--json]
#include "common/bytes.hpp"
#include "crypto/hash.hpp"
#include "io/input_source.hpp"
#include "io/stream.hpp"
#include "io/zip/builder.hpp"
#include "io/zip/extractor.hpp"
#include "io/zip/output_sink.hpp"
#include "io/zip/planner.hpp"
#include "io/zip/reader.hpp"
#include "pipeline/context.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <numbers>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace zp;

namespace
{

constexpr double mib = 1024.0 * 1024.0;

struct Options
{
    std::size_t size_mib = 256;
    int threads = 0;
    bool json = false;
    bool explicit_threads = false;
    std::string only;
};

struct Row
{
    std::string name;
    int threads = 0;
    double input_mib = 0;
    double build_mib_s = 0;
    double extract_mib_s = 0;
    double ratio = 0;
};

template <typename F>
double seconds(F&& f)
{
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::vector<std::uint8_t> wav_24bit(std::size_t bytes, std::uint16_t channels)
{
    constexpr std::uint32_t rate = 48000;
    const std::size_t frames = bytes / (3 * channels);
    ByteWriter w;
    w.str("RIFF");
    w.u32le(static_cast<std::uint32_t>(36 + frames * 3 * channels));
    w.str("WAVE");
    w.str("fmt ");
    w.u32le(16);
    w.u16le(1);
    w.u16le(channels);
    w.u32le(rate);
    w.u32le(rate * 3 * channels);
    w.u16le(3 * channels);
    w.u16le(24);
    w.str("data");
    w.u32le(static_cast<std::uint32_t>(frames * 3 * channels));
    auto out = w.take();
    out.reserve(out.size() + frames * 3 * channels);
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 2000.0);
    for (std::size_t f = 0; f < frames; ++f)
    {
        const double t = static_cast<double>(f) / rate;
        for (std::uint16_t c = 0; c < channels; ++c)
        {
            const double music = 1.5e6 * std::sin(2 * std::numbers::pi * 220.0 * t + c) + 8e5 * std::sin(2 * std::numbers::pi * (330.0 + 0.5 * c) * t) + 4e5 * std::sin(2 * std::numbers::pi * 1760.0 * t * (1 + 0.01 * std::sin(t)));
            const auto v = static_cast<std::int32_t>(std::lround(music + noise(rng)));
            const auto u = static_cast<std::uint32_t>(v);
            out.push_back(static_cast<std::uint8_t>(u));
            out.push_back(static_cast<std::uint8_t>(u >> 8));
            out.push_back(static_cast<std::uint8_t>(u >> 16));
        }
    }
    return out;
}

std::vector<std::uint8_t> text(std::size_t bytes)
{
    static constexpr std::array<std::string_view, 16> words{ "archive", "the", "of", "audio", "sample", "and", "frame", "zip", "header", "a", "stream", "to", "lossless", "in", "file", "data" };
    std::mt19937 rng(11);
    std::vector<std::uint8_t> out;
    out.reserve(bytes);
    while (out.size() < bytes)
    {
        const auto& word = words[rng() % words.size()];
        out.insert(out.end(), word.begin(), word.end());
        out.push_back(rng() % 12 == 0 ? '\n' : ' ');
    }
    out.resize(bytes);
    return out;
}

std::vector<std::uint8_t> random(std::size_t bytes)
{
    std::mt19937_64 rng(13);
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i + 8 <= bytes; i += 8)
    {
        const auto v = rng();
        std::memcpy(out.data() + i, &v, 8);
    }
    return out;
}

double hash_mib_s(const std::vector<std::uint8_t>& data, bool sha)
{
    (void) Sha256::of(std::span(data).first(std::min<std::size_t>(data.size(), 1 << 20)));
    const auto t = seconds([&] {
        if (sha)
            (void) Sha256::of(data);
        else
            (void) Md5::of(data);
    });
    return static_cast<double>(data.size()) / mib / t;
}

Row run(const std::string& name, const std::string& file, const std::vector<std::uint8_t>& data, PlannerOptions planner, int threads)
{
    MemoryInputSource input;
    input.add_file(file, data, 1'700'000'000LL * 1'000'000'000);
    auto plan = ArchivePlanner(planner).plan(input);
    if (!plan)
    {
        std::println(stderr, "plan failed: {}", plan.error().message);
        std::exit(1);
    }
    Context ctx(threads, 0);
    MemoryStream out;
    BuildResult built;
    const auto build_s = seconds([&] {
        ArchiveBuilder builder(out, ctx);
        ProgressSink progress;
        built = builder.execute(*plan, progress);
    });
    if (built.status != Status::Ok)
    {
        std::println(stderr, "build failed: {}", built.message);
        std::exit(1);
    }
    const auto& zip = out.data();
    MemoryStream in(zip);
    auto reader = ArchiveReader::open(in);
    NullOutputSink sink;
    ArchiveExtractor extractor(**reader, sink, ctx, { .overwrite = OverwritePolicy::Replace });
    auto xplan = extractor.plan_extraction({});
    ExtractResult extracted;
    const auto extract_s = seconds([&] {
        ProgressSink progress;
        extracted = extractor.execute(*xplan, progress);
    });
    if (extracted.status != Status::Ok)
    {
        std::println(stderr, "extract failed: {}", extracted.message);
        std::exit(1);
    }
    const auto in_mib = static_cast<double>(data.size()) / mib;
    return { name, threads, in_mib, in_mib / build_s, in_mib / extract_s, static_cast<double>(zip.size()) / static_cast<double>(data.size()) };
}

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--json")
            o.json = true;
        else if (a == "--size" && i + 1 < argc)
            o.size_mib = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--case" && i + 1 < argc)
            o.only = argv[++i];
        else if (a == "--threads" && i + 1 < argc)
        {
            o.threads = std::atoi(argv[++i]);
            o.explicit_threads = true;
        }
        else
        {
            std::println(stderr, "usage: zp_bench [--size MiB] [--threads N] [--case TEXT] [--json]");
            std::exit(2);
        }
    }
    if (o.threads <= 0)
        o.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return o;
}

}

int main(int argc, char** argv)
{
    const auto opt = parse(argc, argv);
    const auto bytes = opt.size_mib * 1024 * 1024;
    const auto audio = wav_24bit(bytes, 2);
    const auto multitrack = wav_24bit(bytes, 16);
    const auto prose = text(bytes);
    const auto noise = random(bytes);

    const auto sha = hash_mib_s(audio, true);
    const auto md5 = hash_mib_s(audio, false);

    std::vector<Row> rows;
    const auto add = [&](const std::string& name, const std::string& file, const std::vector<std::uint8_t>& data, PlannerOptions planner, int threads) {
        if (opt.only.empty() || name.find(opt.only) != std::string::npos)
            rows.push_back(run(name, file, data, planner, threads));
    };
    std::vector<int> thread_counts{ opt.threads };
    if (!opt.explicit_threads && opt.threads > 1)
        thread_counts.insert(thread_counts.begin(), 1);
    for (const int threads : thread_counts)
    {
        add("audio FLAC 5", "take.wav", audio, { .flac_enabled = true, .deflate_level = 6, .flac_level = 5 }, threads);
        add("audio FLAC 0", "take.wav", audio, { .flac_enabled = true, .deflate_level = 6, .flac_level = 0 }, threads);
        add("16-ch multi-mono", "poly.wav", multitrack, {}, threads);
        add("audio deflate 6", "take.wav", audio, { .flac_enabled = false, .deflate_level = 6, .flac_level = 5 }, threads);
        add("text deflate 6", "notes.txt", prose, {}, threads);
        add("text deflate 1", "notes.txt", prose, { .flac_enabled = true, .deflate_level = 1, .flac_level = 5 }, threads);
        add("random (stored)", "noise.bin", noise, {}, threads);
    }

    double flac_best = 0;
    for (const auto& r : rows)
        if (r.name == "audio FLAC 5")
            flac_best = std::max(flac_best, r.build_mib_s);
    const auto hasher_cap = std::min(sha, md5);
    const bool hasher_bound = flac_best >= 0.8 * hasher_cap;

    if (opt.json)
    {
        std::print("{{\"size_mib\":{},\"sha256_mib_s\":{:.1f},\"md5_mib_s\":{:.1f},\"hardware_sha256\":{},\"hasher_bound\":{},\"rows\":[", opt.size_mib, sha, md5, Sha256::hardware_accelerated(), hasher_bound);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            std::print("{}{{\"name\":\"{}\",\"threads\":{},\"build_mib_s\":{:.1f},\"extract_mib_s\":{:.1f},\"ratio\":{:.4f}}}", i ? "," : "", r.name, r.threads, r.build_mib_s, r.extract_mib_s, r.ratio);
        }
        std::println("]}}");
        return 0;
    }

    std::println("Input {} MiB per case. SHA-256 {:.0f} MiB/s ({}), MD5 {:.0f} MiB/s (one thread each).", opt.size_mib, sha, Sha256::hardware_accelerated() ? "CPU instructions" : "portable", md5);
    std::println("{:<18} {:>7} {:>13} {:>15} {:>7}", "case", "threads", "build MiB/s", "extract MiB/s", "size");
    for (const auto& r : rows)
        std::println("{:<18} {:>7} {:>13.0f} {:>15.0f} {:>6.1f}%", r.name, r.threads, r.build_mib_s, r.extract_mib_s, 100.0 * r.ratio);
    std::println("FLAC 5 build {:.0f} MiB/s; the hasher (SHA-256 and MD5 on parallel threads) caps it at {:.0f} MiB/s: {}", flac_best, hasher_cap, hasher_bound ? "hasher-bound" : "not hasher-bound");
    return 0;
}
