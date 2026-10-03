// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/codec.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"
#include "io/zip/minizip_adapter.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace zp
{

struct Zip64Policy
{
    // minizip-ng reserves this much headroom when deciding Zip64 for a local header.
    static constexpr std::uint64_t local_cushion = 2u << 20;

    // Unknown size => Zip64 extra field reserved. An entry whose local header starts at or
    // beyond 4 GiB needs the Zip64 offset in its central record.
    static bool needs_zip64_entry(std::optional<std::uint64_t> size_hint, std::uint64_t local_header_offset);
    static bool needs_zip64_archive(std::uint64_t cd_offset, std::uint64_t entry_count);
};

struct EntryHeader
{
    std::string name;
    bool directory = false;
    ZipMethod method = ZipMethod::Store;
    int deflate_level = 6;
    std::int64_t mtime = 0;
    std::uint32_t unix_mode = 0644;
    // Upper bound for both sizes; drives the Zip64 decision.
    std::optional<std::uint64_t> size_hint;
};

struct WrittenEntry
{
    std::uint64_t local_header_offset = 0;
    bool zip64 = false;
};

// Writes zip records through minizip-ng. Entry data is supplied already encoded (raw mode):
// sizes and CRC are patched into the local header when the output is seekable, otherwise a
// data descriptor follows the data.
class ZipWriter
{
public:
    static Result<std::unique_ptr<ZipWriter>> open(IChunkedStream& out);
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ZipWriter(ZipWriter&&) = delete;
    ZipWriter& operator=(ZipWriter&&) = delete;
    ~ZipWriter();

    Result<WrittenEntry> begin_entry(const EntryHeader& header);
    VoidResult write(std::span<const std::uint8_t> data);
    VoidResult end_entry(std::uint32_t crc, std::uint64_t compressed_size, std::uint64_t uncompressed_size);
    VoidResult finish(const std::string& comment);
    // Drops the archive; nothing more is written to the output.
    void abandon();

    [[nodiscard]] bool uses_data_descriptors() const { return descriptors_; }
    [[nodiscard]] std::uint64_t entry_count() const { return count_; }
    [[nodiscard]] std::uint64_t position();

private:
    explicit ZipWriter(IChunkedStream& out);
    Error last_error(std::int32_t rc, std::string_view what) const;

    MinizipStreamAdapter adapter_;
    MinizipHandle zip_;
    bool descriptors_ = false;
    bool in_entry_ = false;
    bool finished_ = false;
    std::uint64_t count_ = 0;
};

std::uint16_t dos_external_attributes(bool directory);
std::uint32_t external_attributes(bool directory, std::uint32_t unix_mode);

}
