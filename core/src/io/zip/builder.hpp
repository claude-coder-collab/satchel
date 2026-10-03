// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/codec.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"
#include "io/zip/archive_metadata.hpp"
#include "io/zip/plan.hpp"
#include "pipeline/context.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zp {

class ArchiveReader;

struct BuilderOptions
{
    std::uint64_t small_entry_threshold = 4ull << 20;
};

struct EntryResult
{
    std::size_t plan_index = 0;
    std::string name;
    ZipMethod method = ZipMethod::Store;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;
    Status status = Status::Ok;
    std::string message;
};

struct BuildResult
{
    std::vector<EntryResult> per_entry;
    Status status = Status::Ok;
    std::string message;
    std::uint64_t peak_buffered_bytes = 0;
    bool zip64 = false;
};

class ArchiveBuilder
{
public:
    ArchiveBuilder(IChunkedStream& output, Context& context, BuilderOptions options = {});

    void set_metadata(const ArchiveMetadata& metadata) { metadata_ = metadata; }
    // Source of PlanCodec::Kept entries (editor).
    void set_kept_source(ArchiveReader* reader) { kept_source_ = reader; }

    // Refuses a plan with unresolved conflicts. A file that changed since planning is left out
    // with SOURCE_CHANGED in its result; a change while it is being read fails the build.
    BuildResult execute(const ArchivePlan& plan, ProgressSink& progress);

private:
    IChunkedStream& output_;
    Context& context_;
    BuilderOptions options_;
    std::optional<ArchiveMetadata> metadata_;
    ArchiveReader* kept_source_ = nullptr;
};

}
