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
#include <thread>
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
    // per_channel: one MD5 per channel (multi-mono), split into `md5_groups` groups of channels
    // that can be hashed on separate threads.
    SourceHasher(const PcmLayout& layout, bool per_channel, std::size_t md5_groups = 1);
    void update(std::span<const std::uint8_t> chunk);
    // The parts of update(): SHA-256, and the MD5s of one channel group. They touch disjoint
    // state, so each may run on its own thread.
    void update_sha256(std::span<const std::uint8_t> chunk);
    void update_md5(std::span<const std::uint8_t> chunk, std::size_t group = 0);
    [[nodiscard]] std::size_t md5_groups() const { return groups_.size(); }
    Hashes finish();

private:
    struct Group
    {
        std::size_t first_channel = 0;
        std::size_t channels = 0;
        std::uint64_t position = 0;
        std::vector<std::uint8_t> carry;
        std::vector<std::uint8_t> scratch;
        std::vector<std::vector<std::uint8_t>> channel_scratch;
    };
    void audio(Group& g, std::span<const std::uint8_t> bytes);

    const PcmLayout& layout_;
    bool per_channel_;
    Sha256 sha_;
    std::vector<Md5> md5_;
    std::vector<Group> groups_;
};

// Runs a SourceHasher on several threads ("lanes") that read one queue of chunks in order: lane 0
// is SHA-256, lanes 1.. hash MD5 channel groups. Lanes 0 and 1 are run by the caller (service
// threads); a multi-mono source with many channels gets more MD5 lanes, run on threads owned by
// the job so they never wait for a busy pool.
class HashJob
{
public:
    static constexpr std::size_t max_queued = 16;

    HashJob(const PcmLayout& layout, bool per_channel);
    HashJob(const HashJob&) = delete;
    HashJob& operator=(const HashJob&) = delete;
    HashJob(HashJob&&) = delete;
    HashJob& operator=(HashJob&&) = delete;
    ~HashJob();

    // Blocks while a lane has max_queued chunks waiting.
    void push(std::shared_ptr<const std::vector<std::uint8_t>> chunk);
    void close();
    // Body of lane 0 or 1 (the caller runs both); the job runs any further lanes itself.
    void run(std::size_t lane);
    Hashes wait();

    // MD5 channel groups for a source: more than one only for multi-mono with at least 8 channels
    // (at most 4, never under WebAssembly).
    static std::size_t md5_groups_for(const PcmLayout& layout, bool per_channel);

private:
    SourceHasher hasher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> queue_;
    std::uint64_t first_ = 0;
    std::uint64_t pushed_ = 0;
    std::vector<std::uint64_t> next_;
    bool closed_ = false;
    std::size_t lanes_done_ = 0;
    std::optional<Hashes> result_;
    std::vector<std::thread> extra_lanes_;
};

}
