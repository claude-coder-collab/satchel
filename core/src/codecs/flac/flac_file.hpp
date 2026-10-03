// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/flac/flac_format.hpp"
#include "codecs/pcm/pcm_container.hpp"
#include "common/status.hpp"
#include "crypto/hash.hpp"
#include "io/stream.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace zp::flac
{

// Everything needed to write the metadata of one FLAC file (multichannel or one mono member).
struct HeaderPlan
{
    StreamInfo info;
    VorbisComments comments;
    std::vector<ForeignRecord> standard; // layout 0 only
    ProjectBlock project;
};

struct AssembledHeader
{
    std::vector<std::uint8_t> bytes;
    std::size_t streaminfo_offset = 8;
    std::size_t sha_offset = 0;
};

// fLaC, STREAMINFO, VORBIS_COMMENT, standard foreign blocks, project block.
AssembledHeader assemble_header(const HeaderPlan& plan);

// Application ID used for the records of a container (CAF has no standard one).
AppId record_id(PcmContainer c);
// One record per foreign block of the layout, read from the source.
Result<std::vector<ForeignRecord>> read_records(IChunkedStream& in, const PcmLayout& layout);
Result<std::vector<std::uint8_t>> read_range(IChunkedStream& in, std::uint64_t offset, std::uint64_t length);

// Upper bound for the FLAC encoding of `frames` sample frames (verbatim subframes plus frame
// overhead), used for the Zip64 decision.
std::uint64_t encoded_size_bound(const PcmLayout& layout, std::uint64_t frames, std::uint32_t channels_per_stream);

struct Hashes
{
    Sha256::Digest sha256{};
    std::vector<Md5::Digest> md5; // one per FLAC stream (1 for multichannel, N for multi-mono)
};

// SHA-256 of the whole source file and the FLAC MD5 signature(s) of its samples, fed with the
// file in order and in chunks of any size.
class SourceHasher
{
public:
    SourceHasher(const PcmLayout& layout, bool per_channel);
    void update(std::span<const std::uint8_t> chunk);
    Hashes finish();

private:
    void audio(std::span<const std::uint8_t> bytes);

    const PcmLayout& layout_;
    bool per_channel_;
    std::uint64_t position_ = 0;
    Sha256 sha_;
    std::vector<Md5> md5_;
    std::vector<std::uint8_t> carry_;
    std::vector<std::uint8_t> scratch_;
    std::vector<std::vector<std::uint8_t>> channel_scratch_;
};

// Runs a SourceHasher on its own thread; chunks are queued in order.
class HashJob
{
public:
    static constexpr std::size_t max_queued = 16;

    HashJob(const PcmLayout& layout, bool per_channel);
    // Blocks while max_queued chunks are waiting.
    void push(std::shared_ptr<const std::vector<std::uint8_t>> chunk);
    void close();
    // Body of the hasher thread.
    void run();
    Hashes wait();

private:
    SourceHasher hasher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> queue_;
    bool closed_ = false;
    bool done_ = false;
    std::optional<Hashes> result_;
};

}
