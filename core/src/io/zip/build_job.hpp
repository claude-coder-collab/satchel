// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

// Internal to the builder: the state shared by the reader thread, the workers and the writer.

#include "codecs/codec.hpp"
#include "codecs/flac/flac_file.hpp"
#include "io/file_system.hpp"
#include "io/zip/builder.hpp"
#include "io/zip/plan.hpp"
#include "pipeline/context.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace zp::build
{

enum class SegmentKind : std::uint8_t {
    Data,
    Directory,
    Kept,
    SkipEntry,
    Fatal,
    FlacHeader, // metadata of a multichannel FLAC entry (placeholders if patched later)
    FlacFrames, // encoded frames of a multichannel FLAC entry
    FlacEnd, // finish the FLAC entry: patch hashes, close
    MonoStart, // create spill files for a multi-mono group
    MonoFrames, // frames of one channel, appended to its spill file
    MonoMember, // finalize one member and copy it into the archive
};

struct SpillFile
{
    std::filesystem::path path;
    std::unique_ptr<FileStream> stream;
    SpillFile() = default;
    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;
    SpillFile(SpillFile&&) = default;
    SpillFile& operator=(SpillFile&&) = default;
    ~SpillFile();
};

// One FLAC output (multichannel) or one multi-mono group.
struct FlacState
{
    std::vector<std::size_t> entries; // plan indices (1, or one per channel)
    std::vector<flac::HeaderPlan> headers;
    std::vector<flac::AssembledHeader> assembled;
    bool patch_hashes = true;
    std::shared_ptr<flac::HashJob> hasher;
    std::optional<flac::Hashes> hashes; // known up front (two-pass)

    // Writer side.
    std::vector<std::uint32_t> min_frame;
    std::vector<std::uint32_t> max_frame;
    std::vector<std::uint64_t> frames_bytes;
    std::uint32_t frames_crc = 0;
    std::vector<SpillFile> spills;
};

struct Segment
{
    SegmentKind kind = SegmentKind::Data;
    std::size_t entry = 0;
    bool first = true;
    bool last = true;
    ZipMethod method = ZipMethod::Store;
    std::vector<std::uint8_t> output;
    std::uint32_t crc = 0;
    std::uint64_t input_size = 0;
    std::uint64_t budget = 0;
    Error error;
    std::shared_ptr<FlacState> flac;
    std::uint16_t channel = 0; // 0-based, MonoFrames / MonoMember
    std::uint32_t min_frame = 0;
    std::uint32_t max_frame = 0;
};

class Job
{
public:
    Job(const ArchivePlan& job_plan, Context& job_context, const BuilderOptions& job_options, bool seekable);

    void post(std::size_t seq, Segment s);
    void finish_reading(std::size_t total);
    void abort();

    const ArchivePlan& plan;
    Context& context;
    const BuilderOptions& options;
    const bool output_seekable;
    ByteBudget budget;
    std::unique_ptr<SegmentEncoder> store;
    std::unique_ptr<SegmentEncoder> deflate;
    TaskCounter tasks;
    std::atomic<bool> cancelled{ false };

    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::size_t, Segment> ready;
    std::optional<std::size_t> total_segments;
};

// Reader thread body: walks the plan, posts segments in order.
void read_entries(Job& job);

std::uint64_t deflate_size_hint(std::uint64_t size);

}
