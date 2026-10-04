// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Multi-GB tests (hidden tag [.large]; run with `zp_tests "[large]"`). Inputs are generated on the
// fly so nothing large is held in memory; archives go to a temporary file.
#include "codecs/codec.hpp"
#include "crypto/hash.hpp"
#include "io/file_system.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstring>
#include <format>
#include <functional>

using namespace zp;

namespace
{

// Read-only stream of `size` bytes: `head` verbatim, then generator(offset, out) for the rest.
class GeneratedStream final : public IChunkedStream
{
public:
    using Generator = std::function<void(std::uint64_t offset, std::span<std::uint8_t> out)>;

    GeneratedStream(std::uint64_t size, std::vector<std::uint8_t> head, Generator gen) :
        size_(size),
        head_(std::move(head)),
        gen_(std::move(gen))
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override
    {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(len, size_ - std::min(size_, pos_)));
        std::size_t done = 0;
        while (done < n)
        {
            const auto at = pos_ + done;
            if (at < head_.size())
            {
                const auto k = std::min<std::size_t>(n - done, head_.size() - static_cast<std::size_t>(at));
                std::memcpy(buf + done, head_.data() + at, k);
                done += k;
            }
            else
            {
                gen_(at - head_.size(), { buf + done, n - done });
                done = n;
            }
        }
        pos_ += n;
        return n;
    }
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override
    {
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return size_; }
    VoidResult reopen() override { return seek(0); }

private:
    std::uint64_t size_;
    std::vector<std::uint8_t> head_;
    Generator gen_;
    std::uint64_t pos_ = 0;
};

class GeneratedSource final : public InputSource
{
public:
    GeneratedSource(std::string name, std::uint64_t size, std::vector<std::uint8_t> head, GeneratedStream::Generator gen) :
        name_(std::move(name)),
        size_(size),
        head_(std::move(head)),
        gen_(std::move(gen))
    {
    }

    Result<std::vector<InputItem>> enumerate() override
    {
        InputItem item;
        item.source_path = "generated:" + name_;
        item.archive_path = name_;
        item.size = size_;
        item.mtime_ns = 1'700'000'000LL * 1'000'000'000;
        item.opener = [this]() -> Result<std::unique_ptr<IChunkedStream>> { return std::make_unique<GeneratedStream>(size_, head_, gen_); };
        return std::vector<InputItem>{ item };
    }

    std::unique_ptr<IChunkedStream> open() const { return std::make_unique<GeneratedStream>(size_, head_, gen_); }

private:
    std::string name_;
    std::uint64_t size_;
    std::vector<std::uint8_t> head_;
    GeneratedStream::Generator gen_;
};

// Incompressible bytes, reproducible from the offset alone.
void noise(std::uint64_t offset, std::span<std::uint8_t> out)
{
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        std::uint64_t x = (offset + i) / 8 * 0x9E3779B97F4A7C15ull;
        x ^= x >> 29;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 32;
        out[i] = static_cast<std::uint8_t>(x >> (8 * ((offset + i) % 8)));
    }
}

// Sink that only hashes what it receives (per file).
class HashingSink final : public OutputSink
{
public:
    std::map<std::string, Sha256::Digest> digests;
    std::map<std::string, std::uint64_t> sizes;

    bool exists(const std::string&) override { return false; }
    VoidResult make_directory(const std::string&) override { return {}; }
    VoidResult set_directory_attributes(const std::string&, std::int64_t, std::optional<std::uint32_t>) override { return {}; }
    Result<std::unique_ptr<OutputFile>> create_file(const std::string& path, bool) override { return std::make_unique<File>(*this, path); }

private:
    class File final : public OutputFile
    {
    public:
        File(HashingSink& sink, std::string path) :
            sink_(sink),
            path_(std::move(path))
        {
        }
        VoidResult write(std::span<const std::uint8_t> data) override
        {
            sha_.update(data);
            size_ += data.size();
            return {};
        }
        VoidResult commit(std::int64_t, std::optional<std::uint32_t>) override
        {
            sink_.digests[path_] = sha_.finish();
            sink_.sizes[path_] = size_;
            return {};
        }

    private:
        HashingSink& sink_;
        std::string path_;
        Sha256 sha_;
        std::uint64_t size_ = 0;
    };
};

Sha256::Digest hash_stream(IChunkedStream& s)
{
    Sha256 sha;
    std::vector<std::uint8_t> buf(4u << 20);
    while (true)
    {
        auto n = s.read(buf.data(), buf.size());
        REQUIRE(n);
        if (*n == 0)
            break;
        sha.update({ buf.data(), *n });
    }
    return sha.finish();
}

void extract_hashing(const std::filesystem::path& zip, HashingSink& sink)
{
    auto in = FileStream::open(zip, FileMode::Read).value();
    auto reader = ArchiveReader::open(*in).value();
    ArchiveExtractor x(*reader, sink, test::shared_context(), { .overwrite = OverwritePolicy::Replace });
    auto plan = x.plan_extraction({}).value();
    ProgressSink progress;
    const auto r = x.execute(plan, progress);
    CHECK(r.status == Status::Ok);
}

}

TEST_CASE("an entry over 4 GiB uses Zip64 and survives standard tools", "[.large][large]")
{
    constexpr std::uint64_t size = (4ull << 30) + 12345;
    GeneratedSource source("big.bin", size, {}, noise);
    const auto plan = test::plan_of(source);
    test::TempDir dir;
    const auto zip = dir.path() / "big.zip";
    {
        auto out = AtomicFileStream::create(zip).value();
        Context ctx(0, 64ull << 20);
        ArchiveBuilder builder(*out, ctx);
        ProgressSink progress;
        const auto r = builder.execute(plan, progress);
        REQUIRE(r.status == Status::Ok);
        CHECK(r.zip64);
        CHECK(r.per_entry[0].method == ZipMethod::Store);
        CHECK(r.peak_buffered_bytes <= 64ull << 20);
        REQUIRE(out->commit());
    }
    auto in = FileStream::open(zip, FileMode::Read).value();
    auto reader = ArchiveReader::open(*in).value();
    CHECK(reader->zip64());
    CHECK(reader->entries()[0].uncompressed_size == size);
    HashingSink sink;
    extract_hashing(zip, sink);
    CHECK(sink.digests.at("big.bin") == hash_stream(*source.open()));
    if (const auto unzip = test::find_tool("unzip"))
        CHECK(test::run(std::format("\"{}\" -tqq \"{}\"", *unzip, zip.string())) == 0);
    if (const auto sz = test::find_tool("7z"))
        CHECK(test::run(std::format("\"{}\" t \"{}\" > /dev/null", *sz, zip.string())) == 0);
}

TEST_CASE("RF64 over 4 GiB round-trips through FLAC under a memory cap", "[.large][large]")
{
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bytes = 3;
    constexpr std::uint64_t frames = (4ull << 30) / (channels * bytes) + 1000;
    constexpr std::uint64_t audio = frames * channels * bytes;
    ByteWriter h;
    h.str("RF64");
    h.u32le(0xFFFFFFFFu);
    h.str("WAVE");
    h.str("ds64");
    h.u32le(28);
    const std::uint64_t riff_size = 4 + 36 + 24 + 8 + audio;
    h.u64le(riff_size);
    h.u64le(audio);
    h.u64le(frames);
    h.u32le(0);
    h.str("fmt ");
    h.u32le(16);
    h.u16le(1);
    h.u16le(channels);
    h.u32le(48000);
    h.u32le(48000 * channels * bytes);
    h.u16le(channels * bytes);
    h.u16le(24);
    h.str("data");
    h.u32le(0xFFFFFFFFu);
    const auto head = h.take();
    const auto samples = [](std::uint64_t offset, std::span<std::uint8_t> out) {
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const auto at = offset + i;
            const auto sample_index = at / bytes;
            const auto frame = static_cast<double>(sample_index / channels);
            const auto v = static_cast<std::int32_t>(3000000 * std::sin(frame * 0.003 * static_cast<double>(1 + sample_index % channels)));
            out[i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * (at % bytes)));
        }
    };
    GeneratedSource source("long.rf64", head.size() + audio, head, samples);
    const auto plan = test::plan_of(source);
    REQUIRE(plan.entries[0].codec == PlanCodec::Flac);
    test::TempDir dir;
    const auto zip = dir.path() / "rf64.zip";
    {
        auto out = AtomicFileStream::create(zip).value();
        Context ctx(0, 96ull << 20);
        ArchiveBuilder builder(*out, ctx);
        ProgressSink progress;
        const auto r = builder.execute(plan, progress);
        REQUIRE(r.status == Status::Ok);
        CHECK(r.peak_buffered_bytes <= 96ull << 20);
        REQUIRE(out->commit());
    }
    HashingSink sink;
    extract_hashing(zip, sink);
    CHECK(sink.sizes.at("long.rf64") == head.size() + audio);
    CHECK(sink.digests.at("long.rf64") == hash_stream(*source.open()));
}

TEST_CASE("the memory budget holds on a multi-GB deflate entry with 1 and 64 threads", "[.large][large]")
{
    constexpr std::uint64_t size = 2ull << 30;
    constexpr std::uint64_t budget = 48ull << 20;
    const auto text = [](std::uint64_t offset, std::span<std::uint8_t> out) {
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const auto at = offset + i;
            out[i] = static_cast<std::uint8_t>('a' + (at * 7 + at / 4093) % 26);
        }
    };
    GeneratedSource source("big.txt", size, {}, text);
    const auto plan = test::plan_of(source);
    const auto threads = GENERATE(1, 64);
    CAPTURE(threads);
    test::TempDir dir;
    const auto zip = dir.path() / "text.zip";
    {
        auto out = AtomicFileStream::create(zip).value();
        Context ctx(threads, budget);
        ArchiveBuilder builder(*out, ctx);
        ProgressSink progress;
        const auto r = builder.execute(plan, progress);
        REQUIRE(r.status == Status::Ok);
        CHECK(r.per_entry[0].method == ZipMethod::Deflate);
        CHECK(r.peak_buffered_bytes <= budget);
        REQUIRE(out->commit());
    }
    HashingSink sink;
    extract_hashing(zip, sink);
    CHECK(sink.digests.at("big.txt") == hash_stream(*source.open()));
}
