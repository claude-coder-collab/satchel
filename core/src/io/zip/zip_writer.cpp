// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/zip_writer.hpp"

#include "common/bytes.hpp"

#include <format>
#include <limits>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <utility>

namespace zp
{

namespace
{

constexpr std::uint16_t version_made_by = (MZ_HOST_SYSTEM_UNIX << 8) | 45;
constexpr std::uint16_t extended_timestamp_tag = 0x5455;
constexpr std::uint32_t unix_type_dir = 0040000;
constexpr std::uint32_t unix_type_file = 0100000;

}

bool Zip64Policy::needs_zip64_entry(std::optional<std::uint64_t> size_hint, std::uint64_t local_header_offset)
{
    if (local_header_offset >= std::numeric_limits<std::uint32_t>::max())
        return true;
    if (!size_hint)
        return true;
    return *size_hint >= std::numeric_limits<std::uint32_t>::max() - local_cushion;
}

bool Zip64Policy::needs_zip64_archive(std::uint64_t cd_offset, std::uint64_t entry_count)
{
    return cd_offset >= std::numeric_limits<std::uint32_t>::max() || entry_count >= std::numeric_limits<std::uint16_t>::max();
}

std::uint16_t dos_external_attributes(bool directory)
{
    return directory ? 0x10 : 0x00;
}

std::uint32_t external_attributes(bool directory, std::uint32_t unix_mode)
{
    const std::uint32_t type = directory ? unix_type_dir : unix_type_file;
    return ((type | (unix_mode & 07777u)) << 16) | dos_external_attributes(directory);
}

ZipWriter::ZipWriter(IChunkedStream& out) :
    adapter_(out)
{
}

ZipWriter::~ZipWriter()
{
    if (!finished_)
        abandon();
}

Result<std::unique_ptr<ZipWriter>> ZipWriter::open(IChunkedStream& out)
{
    std::unique_ptr<ZipWriter> w(new ZipWriter(out));
    w->descriptors_ = !out.seekable();
    const auto rc = mz_zip_open(w->zip_.get(), w->adapter_.handle(), MZ_OPEN_MODE_WRITE);
    if (rc != MZ_OK)
        return std::unexpected(w->last_error(rc, "cannot start archive"));
    w->zip_.open_ = true;
    mz_zip_set_version_madeby(w->zip_.get(), version_made_by);
    return w;
}

Error ZipWriter::last_error(std::int32_t rc, std::string_view what) const
{
    return adapter_.error_or(Status::IoError, std::format("{} (minizip error {})", what, rc));
}

std::uint64_t ZipWriter::position()
{
    return static_cast<std::uint64_t>(mz_stream_tell(adapter_.handle()));
}

Result<WrittenEntry> ZipWriter::begin_entry(const EntryHeader& header)
{
    if (in_entry_ || finished_)
        return fail(Status::Internal, "entry already open");
    WrittenEntry written;
    written.local_header_offset = position();
    written.zip64 = !header.directory && Zip64Policy::needs_zip64_entry(header.size_hint, written.local_header_offset);
    if (header.directory && written.local_header_offset >= std::numeric_limits<std::uint32_t>::max())
        written.zip64 = true;

    ByteWriter extra;
    extra.u16le(extended_timestamp_tag);
    extra.u16le(5);
    extra.u8(1);
    extra.u32le(static_cast<std::uint32_t>(header.mtime));

    mz_zip_file info{};
    info.version_madeby = version_made_by;
    info.flag = MZ_ZIP_FLAG_UTF8;
    if (descriptors_)
        info.flag |= MZ_ZIP_FLAG_DATA_DESCRIPTOR;
    info.compression_method = static_cast<std::uint16_t>(header.directory ? ZipMethod::Store : header.method);
    info.modified_date = static_cast<time_t>(header.mtime);
    info.uncompressed_size = descriptors_ ? 0 : static_cast<std::int64_t>(header.size_hint.value_or(0));
    info.filename = header.name.c_str();
    info.extrafield = extra.data().data();
    info.extrafield_size = static_cast<std::uint16_t>(extra.data().size());
    info.external_fa = external_attributes(header.directory, header.unix_mode);
    info.zip64 = written.zip64 ? MZ_ZIP64_FORCE : MZ_ZIP64_DISABLE;

    // minizip-ng turns level 0 into method 0; any other level keeps the raw method we pass.
    std::int16_t level = 0;
    if (!header.directory && header.method != ZipMethod::Store)
        level = static_cast<std::int16_t>(header.method == ZipMethod::Deflate && header.deflate_level > 0 ? header.deflate_level : 6);
    const auto rc = mz_zip_entry_write_open(zip_.get(), &info, level, 1, nullptr);
    if (rc != MZ_OK)
        return std::unexpected(last_error(rc, std::format("cannot write header for '{}'", header.name)));
    in_entry_ = true;
    return written;
}

VoidResult ZipWriter::write(std::span<const std::uint8_t> data)
{
    constexpr std::size_t max_chunk = 1u << 30;
    while (!data.empty())
    {
        const auto n = std::min(data.size(), max_chunk);
        const auto rc = mz_zip_entry_write(zip_.get(), data.data(), static_cast<std::int32_t>(n));
        if (std::cmp_not_equal(rc, n))
            return std::unexpected(last_error(rc, "cannot write entry data"));
        data = data.subspan(n);
    }
    return {};
}

VoidResult ZipWriter::end_entry(std::uint32_t crc, std::uint64_t compressed_size, std::uint64_t uncompressed_size)
{
    if (!in_entry_)
        return fail(Status::Internal, "no entry open");
    in_entry_ = false;
    const auto rc = mz_zip_entry_write_close(zip_.get(), crc, static_cast<std::int64_t>(compressed_size), static_cast<std::int64_t>(uncompressed_size));
    if (rc != MZ_OK)
        return std::unexpected(last_error(rc, "cannot finish entry"));
    ++count_;
    return {};
}

VoidResult ZipWriter::finish(const std::string& comment)
{
    if (in_entry_)
        return fail(Status::Internal, "entry still open");
    if (!comment.empty())
        mz_zip_set_comment(zip_.get(), comment.c_str());
    const auto rc = zip_.close();
    finished_ = true;
    if (rc != MZ_OK)
        return std::unexpected(last_error(rc, "cannot write central directory"));
    return adapter_.flush();
}

void ZipWriter::abandon()
{
    adapter_.discard_writes();
    if (in_entry_)
    {
        mz_zip_entry_close(zip_.get());
        in_entry_ = false;
    }
    zip_.close();
    finished_ = true;
}

}
