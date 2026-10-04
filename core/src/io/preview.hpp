// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/flac/flac_format.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zp
{

// Summary of a zip archive or of a FLAC file made by this library, for file-manager previewers
// (desktop UI spec, "OS integration"). Built from metadata only: the zip central directory plus the
// FLAC headers of ".flac" entries, never audio.
struct Preview
{
    enum class Kind : std::uint8_t {
        Zip,
        Flac,
    };

    struct Entry
    {
        std::string name;
        bool directory = false;
        std::uint64_t size = 0;
        std::uint64_t packed = 0;
        std::string method; // "Stored", "Deflate", "FLAC", "Method <n>"
        std::string restores_to; // original file name of a restorable FLAC entry
    };

    struct Chunk
    {
        std::string id;
        std::uint64_t size = 0;
        bool audio = false;
    };

    struct Flac
    {
        std::uint32_t sample_rate = 0;
        std::uint32_t bits_per_sample = 0;
        std::uint32_t channels = 0;
        std::uint64_t total_samples = 0;
        std::string original_name;
        std::string container; // "WAV", "RF64", "AIFF", "AIFF-C", "CAF", "Wave64", "unknown" or empty
        std::string layout; // "standard", "multi_mono", "private" or empty (no project block)
        std::uint16_t channel_index = 0;
        std::uint16_t channel_count = 0;
        std::vector<Chunk> chunks; // the original file's chunks after the file header
        std::vector<std::pair<std::string, std::string>> tags;
    };

    Kind kind = Kind::Zip;
    std::string file_name;

    std::optional<std::string> created_by; // app version from our archive comment
    bool zip64 = false;
    std::uint64_t entry_count = 0;
    std::uint64_t file_count = 0;
    std::uint64_t total_size = 0;
    std::uint64_t packed_size = 0;
    std::uint64_t restorable_audio = 0; // files that extraction restores (a multi-mono group counts once)
    bool restorable_audio_partial = false; // more FLAC entries than max_probed: the count is a lower bound
    std::vector<Entry> entries; // first max_entries entries in archive order
    bool truncated = false;

    std::optional<Flac> flac;
};

struct PreviewOptions
{
    std::size_t max_entries = 500;
    std::size_t max_probed = 2000; // FLAC entries whose headers are read
};

// Reads a zip archive or a FLAC file from `input` (seekable). `file_name` is only displayed.
Result<Preview> make_preview(IChunkedStream& input, std::string file_name, const PreviewOptions& options = {});

std::string preview_json(const Preview& preview);
// A self-contained HTML page (inline CSS, light and dark).
std::string preview_html(const Preview& preview);

// Plain-text pieces for native previewers (Windows preview handler).
std::string preview_size_text(std::uint64_t bytes);
// "3 files · 1.2 MB → 800.0 KB (33% smaller) · 2 audio files restorable · created with …"
std::string preview_summary_text(const Preview& preview);
// Property/value rows describing a FLAC file: audio format, original file, layout, then tags.
std::vector<std::pair<std::string, std::string>> preview_flac_rows(const Preview::Flac& flac);

// Details of a FLAC header for display: stream format, project block fields, original container
// and chunks, tags.
Preview::Flac describe_flac(const flac::Header& header);

}
