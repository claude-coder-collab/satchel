// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/parallel_decode.hpp"
#include "codecs/flac/restore.hpp"
#include "pcm_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <tuple>

using namespace zp;

namespace
{

// A FLAC file made by the builder from `source` (one entry).
std::vector<std::uint8_t> flac_of(const std::string& name, const std::vector<std::uint8_t>& source, const std::string& flac_name)
{
    MemoryInputSource in;
    in.add_file(name, source);
    const auto raw = test::extract_all(test::build(in).zip, { .restore_wav = false });
    return raw.files.at(flac_name).data;
}

std::vector<std::uint8_t> noisy_wav(std::uint64_t frames)
{
    test::WavSpec w;
    w.frames = frames;
    w.bits = 24;
    auto wav = test::make_wav(w);
    std::mt19937 rng(42);
    for (std::size_t i = 44; i < wav.size(); ++i)
        wav[i] = static_cast<std::uint8_t>(rng());
    return wav;
}

std::vector<std::uint8_t> restore_with(const std::vector<std::uint8_t>& flac, std::size_t threads, Status* status = nullptr)
{
    MemoryStream s(flac);
    const auto header = flac::read_header(s).value();
    std::vector<std::uint8_t> out;
    const auto r = flac::restore(
        { header }, [&](std::size_t) -> Result<std::unique_ptr<IChunkedStream>> { return std::make_unique<MemoryStream>(flac); }, [&](std::span<const std::uint8_t> b) -> VoidResult {
            out.insert(out.end(), b.begin(), b.end());
            return {}; }, threads
    );
    if (status)
        *status = r ? Status::Ok : r.error().status;
    return out;
}

}

TEST_CASE("the frame splitter finds every frame of noisy audio under any chunking", "[flac][parallel]")
{
    const auto flac = flac_of("n.wav", noisy_wav(300000), "n.flac");
    MemoryStream s(flac);
    const auto header = flac::read_header(s).value();
    const std::span frames = std::span(flac).subspan(static_cast<std::size_t>(header.metadata_size));
    const auto expected = (header.stream_info.total_samples + 4095) / 4096;
    std::mt19937 rng(7);
    for (int round = 0; round < 3; ++round)
    {
        flac::FrameSplitter splitter;
        std::vector<std::uint8_t> joined;
        std::size_t count = 0;
        for (std::size_t off = 0; off < frames.size();)
        {
            const auto n = std::min<std::size_t>(frames.size() - off, 1 + rng() % (round == 0 ? 7 : 70000));
            REQUIRE(splitter.feed(frames.subspan(off, n)));
            off += n;
            auto taken = splitter.take();
            count += taken.count;
            joined.insert(joined.end(), taken.bytes.begin(), taken.bytes.end());
        }
        REQUIRE(splitter.finish());
        auto rest = splitter.take();
        count += rest.count;
        joined.insert(joined.end(), rest.bytes.begin(), rest.bytes.end());
        CHECK(count == expected);
        CHECK(std::ranges::equal(joined, frames));
    }
}

TEST_CASE("the frame splitter refuses a stream that does not start with frame 0", "[flac][parallel]")
{
    const auto flac = flac_of("n.wav", noisy_wav(20000), "n.flac");
    MemoryStream s(flac);
    const auto header = flac::read_header(s).value();
    flac::FrameSplitter splitter;
    const auto r = splitter.feed(std::span(flac).subspan(static_cast<std::size_t>(header.metadata_size) + 1));
    const auto f = r ? splitter.finish() : r;
    CHECK_FALSE(f);
}

TEST_CASE("parallel restore equals sequential restore", "[flac][parallel]")
{
    test::WavSpec small;
    small.frames = 1000;
    test::WavSpec multi;
    multi.channels = 6;
    multi.bits = 24;
    multi.frames = 200000;
    test::AiffSpec aiff;
    aiff.bits = 24;
    aiff.frames = 150000;
    const std::vector<std::tuple<std::string, std::vector<std::uint8_t>, std::string>> cases{
        { "small.wav", test::make_wav(small), "small.flac" },
        { "multi.wav", test::make_wav(multi), "multi.flac" },
        { "noise.wav", noisy_wav(250000), "noise.flac" },
        { "a.aif", test::make_aiff(aiff), "a.flac" },
    };
    for (const auto& [name, source, flac_name] : cases)
    {
        CAPTURE(name);
        const auto flac = flac_of(name, source, flac_name);
        CHECK(restore_with(flac, 1) == source);
        CHECK(restore_with(flac, 4) == source);
        CHECK(restore_with(flac, 16) == source);
    }
}

TEST_CASE("parallel restore detects damaged frames", "[flac][parallel]")
{
    const auto flac = flac_of("n.wav", noisy_wav(100000), "n.flac");
    auto damaged = flac;
    damaged[damaged.size() / 2] ^= 0x10;
    Status status = Status::Ok;
    restore_with(damaged, 4, &status);
    CHECK(status != Status::Ok);
    auto truncated = flac;
    truncated.resize(truncated.size() - 100);
    restore_with(truncated, 4, &status);
    CHECK(status != Status::Ok);
}
