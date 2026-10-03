// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/file_system.hpp"
#include "io/stream.hpp"
#include "io/zip/archive_metadata.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zp
{

class ZipWriter;

enum class EntryMethod : std::uint8_t {
    Store,
    Deflate,
    Unsupported,
};

struct FlacGroupInfo
{
    std::array<std::uint8_t, 16> group_id{};
    std::uint16_t channel_index = 0;
    std::uint16_t channel_count = 0;
};

struct ZipEntryInfo
{
    std::string name; // decoded: UTF-8 when flagged, else Unicode Path extra field, else CP437
    std::string raw_name; // bytes as stored
    ItemKind kind = ItemKind::File;
    EntryMethod method = EntryMethod::Store;
    std::uint16_t raw_method = 0;
    std::uint16_t flags = 0;
    bool encrypted = false;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;
    std::int64_t mtime = 0;
    std::optional<std::uint32_t> unix_mode;
    std::uint64_t local_header_offset = 0;
    bool zip64 = false;
    bool flac_restorable = false;
    std::optional<FlacGroupInfo> flac_group;
};

struct CopiedEntry
{
    bool zip64 = false;
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
};

// Lists an archive from its central directory only. Entry data is read on demand; several
// entries may be read concurrently (reads of the shared input are serialized).
class ArchiveReader
{
public:
    static Result<std::unique_ptr<ArchiveReader>> open(IChunkedStream& input);

    [[nodiscard]] const std::vector<ZipEntryInfo>& entries() const { return entries_; }
    [[nodiscard]] const std::optional<ArchiveMetadata>& metadata() const { return metadata_; }
    [[nodiscard]] const std::string& comment() const { return comment_; }
    [[nodiscard]] bool zip64() const { return zip64_; }

    // Compressed bytes of an entry.
    Result<std::unique_ptr<IChunkedStream>> open_raw(std::size_t index);
    // Decompressed bytes (store/deflate). The CRC is not checked here.
    Result<std::unique_ptr<IChunkedStream>> open_entry(std::size_t index);
    // Copies an entry's compressed bytes unchanged into a new archive, under a possibly new name.
    Result<CopiedEntry> copy_raw(std::size_t index, const std::string& name, std::int64_t mtime, std::uint32_t unix_mode, ZipWriter& writer);

    Result<std::size_t> read_at(std::uint64_t offset, std::uint8_t* buf, std::size_t len);

private:
    explicit ArchiveReader(IChunkedStream& input) :
        input_(input)
    {
    }
    Result<std::uint64_t> data_offset(std::size_t index);

    IChunkedStream& input_;
    std::mutex io_mutex_;
    std::vector<ZipEntryInfo> entries_;
    std::vector<std::optional<std::uint64_t>> data_offsets_;
    std::optional<ArchiveMetadata> metadata_;
    std::string comment_;
    bool zip64_ = false;
};

std::string cp437_to_utf8(std::string_view raw);

}
