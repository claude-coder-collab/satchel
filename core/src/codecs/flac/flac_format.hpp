// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/bytes.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace zp::flac
{

using AppId = std::array<std::uint8_t, 4>;

inline constexpr std::uint32_t block_size = 4096;
inline constexpr std::uint32_t blocks_per_segment = 64;
inline constexpr std::uint32_t max_block_length = (1u << 24) - 1;

enum class BlockType : std::uint8_t {
    StreamInfo = 0,
    Padding = 1,
    Application = 2,
    SeekTable = 3,
    VorbisComment = 4,
    CueSheet = 5,
    Picture = 6,
};

std::uint8_t crc8(std::span<const std::uint8_t> data);
std::uint16_t crc16(std::span<const std::uint8_t> data);

struct StreamInfo
{
    std::uint16_t min_block_size = block_size;
    std::uint16_t max_block_size = block_size;
    std::uint32_t min_frame_size = 0;
    std::uint32_t max_frame_size = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits_per_sample = 0;
    std::uint64_t total_samples = 0;
    std::array<std::uint8_t, 16> md5{};

    [[nodiscard]] std::array<std::uint8_t, 34> serialize() const;
    static std::optional<StreamInfo> parse(std::span<const std::uint8_t> data);
};

// One encoded frame, as libFLAC wrote it.
struct Frame
{
    std::vector<std::uint8_t> bytes;
};

// Rewrites the frame number of a fixed-blocksize frame and recomputes CRC-8 and CRC-16.
Result<std::vector<std::uint8_t>> renumber_frame(std::span<const std::uint8_t> frame, std::uint64_t frame_number);
// Frame number of a fixed-blocksize frame (for tests and checks).
std::optional<std::uint64_t> frame_number(std::span<const std::uint8_t> frame);

void append_block_header(ByteWriter& w, BlockType type, std::size_t length, bool last);

struct VorbisComments
{
    std::string vendor;
    std::vector<std::pair<std::string, std::string>> fields;

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    static std::optional<VorbisComments> parse(std::span<const std::uint8_t> data);
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const;
};

enum class Layout : std::uint8_t {
    Standard = 0,
    MultiMonoMember = 1,
    Private = 2,
};

// The project APPLICATION block (main spec 7.2). Integers are big-endian.
struct ProjectBlock
{
    static constexpr std::uint8_t current_schema = 1;
    using GroupId = std::array<std::uint8_t, 16>;

    std::uint8_t schema = current_schema;
    Layout layout = Layout::Standard;
    std::array<std::uint8_t, 16> group_id{};
    std::uint16_t channel_index = 1;
    std::uint16_t channel_count = 0;
    std::string original_name;
    std::vector<std::uint8_t> private_data; // deflate-compressed records
    std::vector<std::uint8_t> trailing;
    std::array<std::uint8_t, 32> sha256{};

    // Block content including the 4-byte application ID.
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    static std::optional<ProjectBlock> parse(std::span<const std::uint8_t> content);
    // Offset of the SHA-256 inside serialize()'s output.
    [[nodiscard]] std::size_t sha_offset() const;
};

// A foreign metadata record: one chunk (or header) of the original file.
struct ForeignRecord
{
    AppId id{};
    std::vector<std::uint8_t> bytes;
};

// Private storage: records encoded as APPLICATION metadata blocks, concatenated, then deflated.
Result<std::vector<std::uint8_t>> pack_private(const std::vector<ForeignRecord>& records);
Result<std::vector<ForeignRecord>> unpack_private(std::span<const std::uint8_t> packed);

// Everything before the first audio frame.
struct Header
{
    StreamInfo stream_info;
    std::optional<VorbisComments> comments;
    std::vector<ForeignRecord> foreign; // standard riff/aiff/w64 blocks, in order
    std::optional<ProjectBlock> project;
    std::uint64_t metadata_size = 0; // bytes up to the first frame
};

// Reads "fLaC" and all metadata blocks from the start of a stream.
Result<Header> read_header(IChunkedStream& in);
bool is_project_id(std::span<const std::uint8_t> id);
bool is_standard_foreign_id(std::span<const std::uint8_t> id);

}
