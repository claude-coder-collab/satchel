// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/reader.hpp"

#include "codecs/codec.hpp"
#include "common/bytes.hpp"
#include "io/zip/minizip_adapter.hpp"
#include "io/zip/path_policy.hpp"
#include "io/zip/zip_writer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zp
{

namespace
{

constexpr std::uint16_t extended_timestamp_tag = 0x5455;
constexpr std::uint16_t unicode_path_tag = 0x7075;
constexpr std::uint32_t local_header_signature = 0x04034b50;
constexpr std::uint32_t unix_type_mask = 0170000;
constexpr std::uint32_t unix_type_link = 0120000;
constexpr std::uint32_t unix_type_dir = 0040000;

// Read-ahead cache used while minizip walks the central directory with many tiny reads.
class BufferedReadStream final : public IChunkedStream
{
public:
    explicit BufferedReadStream(IChunkedStream& inner) :
        inner_(inner),
        buf_(64u << 10)
    {
    }

    Result<std::size_t> read(std::uint8_t* out, std::size_t len) override
    {
        std::size_t total = 0;
        while (total < len)
        {
            if (pos_ >= start_ && pos_ < start_ + filled_)
            {
                const auto off = static_cast<std::size_t>(pos_ - start_);
                const auto n = std::min(len - total, filled_ - off);
                std::memcpy(out + total, buf_.data() + off, n);
                total += n;
                pos_ += n;
                continue;
            }
            if (len - total >= buf_.size())
            {
                if (auto r = inner_.seek(pos_); !r)
                    return std::unexpected(r.error());
                auto n = inner_.read(out + total, len - total);
                if (!n)
                    return n;
                total += *n;
                pos_ += *n;
                break;
            }
            if (auto r = inner_.seek(pos_); !r)
                return std::unexpected(r.error());
            auto n = inner_.read_full(buf_);
            if (!n)
                return n;
            start_ = pos_;
            filled_ = *n;
            if (filled_ == 0)
                break;
        }
        return total;
    }
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override
    {
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return inner_.size(); }

private:
    IChunkedStream& inner_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t start_ = 0;
    std::size_t filled_ = 0;
    std::uint64_t pos_ = 0;
};

class RangeStream final : public IChunkedStream
{
public:
    RangeStream(ArchiveReader& reader, std::uint64_t offset, std::uint64_t length) :
        reader_(reader),
        offset_(offset),
        length_(length)
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override
    {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(len, length_ - pos_));
        if (n == 0)
            return 0;
        auto got = reader_.read_at(offset_ + pos_, buf, n);
        if (!got)
            return got;
        if (*got != n)
            return fail(Status::CorruptArchive, "entry data is truncated");
        pos_ += n;
        return n;
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return length_; }

private:
    ArchiveReader& reader_;
    std::uint64_t offset_;
    std::uint64_t length_;
    std::uint64_t pos_ = 0;
};

constexpr std::array<char16_t, 128> cp437_high = {
    u'Ç',
    u'ü',
    u'é',
    u'â',
    u'ä',
    u'à',
    u'å',
    u'ç',
    u'ê',
    u'ë',
    u'è',
    u'ï',
    u'î',
    u'ì',
    u'Ä',
    u'Å',
    u'É',
    u'æ',
    u'Æ',
    u'ô',
    u'ö',
    u'ò',
    u'û',
    u'ù',
    u'ÿ',
    u'Ö',
    u'Ü',
    u'¢',
    u'£',
    u'¥',
    u'₧',
    u'ƒ',
    u'á',
    u'í',
    u'ó',
    u'ú',
    u'ñ',
    u'Ñ',
    u'ª',
    u'º',
    u'¿',
    u'⌐',
    u'¬',
    u'½',
    u'¼',
    u'¡',
    u'«',
    u'»',
    u'░',
    u'▒',
    u'▓',
    u'│',
    u'┤',
    u'╡',
    u'╢',
    u'╖',
    u'╕',
    u'╣',
    u'║',
    u'╗',
    u'╝',
    u'╜',
    u'╛',
    u'┐',
    u'└',
    u'┴',
    u'┬',
    u'├',
    u'─',
    u'┼',
    u'╞',
    u'╟',
    u'╚',
    u'╔',
    u'╩',
    u'╦',
    u'╠',
    u'═',
    u'╬',
    u'╧',
    u'╨',
    u'╤',
    u'╥',
    u'╙',
    u'╘',
    u'╒',
    u'╓',
    u'╫',
    u'╪',
    u'┘',
    u'┌',
    u'█',
    u'▄',
    u'▌',
    u'▐',
    u'▀',
    u'α',
    u'ß',
    u'Γ',
    u'π',
    u'Σ',
    u'σ',
    u'µ',
    u'τ',
    u'Φ',
    u'Θ',
    u'Ω',
    u'δ',
    u'∞',
    u'φ',
    u'ε',
    u'∩',
    u'≡',
    u'±',
    u'≥',
    u'≤',
    u'⌠',
    u'⌡',
    u'÷',
    u'≈',
    u'°',
    u'∙',
    u'·',
    u'√',
    u'ⁿ',
    u'²',
    u'■',
    u' ',
};

void append_utf8(std::string& out, char32_t cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct ExtraFields
{
    std::optional<std::int64_t> mtime;
    std::optional<std::string> unicode_path;
};

ExtraFields parse_extra(std::span<const std::uint8_t> extra, std::string_view raw_name)
{
    ExtraFields out;
    ByteReader r(extra);
    while (true)
    {
        const auto tag = r.u16le();
        const auto len = r.u16le();
        if (!tag || !len)
            break;
        auto body = r.bytes(*len);
        if (!body)
            break;
        ByteReader b(*body);
        if (*tag == extended_timestamp_tag)
        {
            const auto flags = b.u8();
            if (flags && (*flags & 1))
            {
                if (auto t = b.u32le())
                    out.mtime = static_cast<std::int64_t>(static_cast<std::int32_t>(*t));
            }
        }
        else if (*tag == unicode_path_tag)
        {
            const auto version = b.u8();
            const auto crc = b.u32le();
            if (version && *version == 1 && crc && *crc == crc32_update(0, as_bytes(raw_name)))
            {
                const auto rest = b.bytes(b.remaining()).value_or(std::span<const std::uint8_t>{});
                std::string name(rest.begin(), rest.end());
                if (PathPolicy::is_valid_utf8(name))
                    out.unicode_path = std::move(name);
            }
        }
    }
    return out;
}

}

std::string cp437_to_utf8(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw)
    {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x80)
            out += c;
        else
            append_utf8(out, cp437_high[b - 0x80]);
    }
    return out;
}

Result<std::unique_ptr<ArchiveReader>> ArchiveReader::open(IChunkedStream& input)
{
    if (!input.seekable())
        return fail(Status::InvalidArgument, "reading an archive needs a seekable input");
    std::unique_ptr<ArchiveReader> reader(new ArchiveReader(input));
    BufferedReadStream buffered(input);
    MinizipStreamAdapter adapter(buffered);
    MinizipHandle zip;
    auto rc = mz_zip_open(zip.get(), adapter.handle(), MZ_OPEN_MODE_READ);
    if (rc != MZ_OK)
        return std::unexpected(adapter.error_or(Status::CorruptArchive, "not a zip archive or the central directory is damaged"));
    zip.open_ = true;

    const char* comment = nullptr;
    if (mz_zip_get_comment(zip.get(), &comment) == MZ_OK && comment)
    {
        reader->comment_ = comment;
        reader->metadata_ = ArchiveMetadata::from_comment(reader->comment_);
    }
    std::uint64_t count = 0;
    mz_zip_get_number_entry(zip.get(), &count);
    reader->entries_.reserve(static_cast<std::size_t>(count));

    for (rc = mz_zip_goto_first_entry(zip.get()); rc == MZ_OK; rc = mz_zip_goto_next_entry(zip.get()))
    {
        mz_zip_file* fi = nullptr;
        if (mz_zip_entry_get_info(zip.get(), &fi) != MZ_OK || !fi)
            return fail(Status::CorruptArchive, "cannot read a central directory record");
        ZipEntryInfo e;
        e.raw_name.assign(fi->filename, fi->filename_size);
        const auto extra = parse_extra({ fi->extrafield, fi->extrafield_size }, e.raw_name);
        if ((fi->flag & MZ_ZIP_FLAG_UTF8) && PathPolicy::is_valid_utf8(e.raw_name))
            e.name = e.raw_name;
        else if (extra.unicode_path)
            e.name = *extra.unicode_path;
        else
            e.name = cp437_to_utf8(e.raw_name);
        e.flags = fi->flag;
        e.encrypted = (fi->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
        e.raw_method = fi->compression_method;
        e.method = e.encrypted            ? EntryMethod::Unsupported
            : fi->compression_method == 0 ? EntryMethod::Store
            : fi->compression_method == 8 ? EntryMethod::Deflate
                                          : EntryMethod::Unsupported;
        e.compressed_size = static_cast<std::uint64_t>(fi->compressed_size);
        e.uncompressed_size = static_cast<std::uint64_t>(fi->uncompressed_size);
        e.crc32 = fi->crc;
        e.mtime = extra.mtime.value_or(static_cast<std::int64_t>(fi->modified_date));
        e.local_header_offset = static_cast<std::uint64_t>(fi->disk_offset);
        e.zip64 = e.compressed_size >= 0xFFFFFFFFu || e.uncompressed_size >= 0xFFFFFFFFu || e.local_header_offset >= 0xFFFFFFFFu;

        const auto host = static_cast<std::uint8_t>(fi->version_madeby >> 8);
        const auto unix_bits = fi->external_fa >> 16;
        const bool unix_host = host == MZ_HOST_SYSTEM_UNIX || host == 19;
        if (unix_host && unix_bits != 0)
            e.unix_mode = unix_bits & 07777u;
        const bool is_dir = e.raw_name.ends_with('/') || e.raw_name.ends_with('\\') || (fi->external_fa & 0x10) != 0 || (unix_host && (unix_bits & unix_type_mask) == unix_type_dir);
        if (unix_host && (unix_bits & unix_type_mask) == unix_type_link)
            e.kind = ItemKind::Symlink;
        else if (is_dir)
            e.kind = ItemKind::Directory;
        else
            e.kind = ItemKind::File;
        reader->entries_.push_back(std::move(e));
    }
    if (rc != MZ_END_OF_LIST)
        return std::unexpected(adapter.error_or(Status::CorruptArchive, "the central directory is damaged"));
    reader->zip64_ = std::ranges::any_of(reader->entries_, &ZipEntryInfo::zip64) || reader->entries_.size() >= 0xFFFF;
    reader->data_offsets_.resize(reader->entries_.size());
    return reader;
}

Result<std::size_t> ArchiveReader::read_at(std::uint64_t offset, std::uint8_t* buf, std::size_t len)
{
    std::lock_guard lock(io_mutex_);
    if (auto r = input_.seek(offset); !r)
        return std::unexpected(r.error());
    return input_.read_full({ buf, len });
}

Result<std::uint64_t> ArchiveReader::data_offset(std::size_t index)
{
    {
        std::lock_guard lock(io_mutex_);
        if (const auto known = data_offsets_[index])
            return *known;
    }
    const auto& e = entries_[index];
    std::array<std::uint8_t, 30> header{};
    auto n = read_at(e.local_header_offset, header.data(), header.size());
    if (!n)
        return std::unexpected(n.error());
    ByteReader r(header);
    if (*n != header.size() || r.u32le() != local_header_signature)
        return fail(Status::CorruptArchive, std::format("local header of '{}' is missing", e.name));
    r.skip(22);
    const auto name_len = r.u16le().value_or(0);
    const auto extra_len = r.u16le().value_or(0);
    const auto offset = e.local_header_offset + header.size() + name_len + extra_len;
    if (const auto size = input_.size(); size && offset + e.compressed_size > *size)
        return fail(Status::CorruptArchive, std::format("data of '{}' extends past the end of the archive", e.name));
    std::lock_guard lock(io_mutex_);
    data_offsets_[index] = offset;
    return offset;
}

Result<std::unique_ptr<IChunkedStream>> ArchiveReader::open_raw(std::size_t index)
{
    if (index >= entries_.size())
        return fail(Status::InvalidArgument, "entry index out of range");
    auto off = data_offset(index);
    if (!off)
        return std::unexpected(off.error());
    return std::make_unique<RangeStream>(*this, *off, entries_[index].compressed_size);
}

Result<std::unique_ptr<IChunkedStream>> ArchiveReader::open_entry(std::size_t index)
{
    if (index >= entries_.size())
        return fail(Status::InvalidArgument, "entry index out of range");
    const auto& e = entries_[index];
    if (e.method == EntryMethod::Unsupported)
        return fail(Status::UnsupportedMethod, std::format("'{}' uses compression method {}{}", e.name, e.raw_method, e.encrypted ? " (encrypted)" : ""));
    auto raw = open_raw(index);
    if (!raw)
        return raw;
    return CodecRegistry::make_decoder(e.raw_method, std::move(*raw));
}

Result<CopiedEntry> ArchiveReader::copy_raw(std::size_t index, const std::string& name, std::int64_t mtime, std::uint32_t unix_mode, ZipWriter& writer)
{
    if (index >= entries_.size())
        return fail(Status::InvalidArgument, "entry index out of range");
    const auto& e = entries_[index];
    if (e.encrypted)
        return fail(Status::UnsupportedMethod, std::format("'{}' is encrypted and cannot be copied", e.name));
    auto raw = open_raw(index);
    if (!raw)
        return std::unexpected(raw.error());
    EntryHeader h;
    h.name = name;
    h.directory = e.kind == ItemKind::Directory;
    h.method = static_cast<ZipMethod>(e.raw_method);
    h.mtime = mtime;
    h.unix_mode = unix_mode;
    h.size_hint = std::max(e.compressed_size, e.uncompressed_size);
    auto w = writer.begin_entry(h);
    if (!w)
        return std::unexpected(w.error());
    std::vector<std::uint8_t> buf(1u << 20);
    while (true)
    {
        auto n = (*raw)->read(buf.data(), buf.size());
        if (!n)
            return std::unexpected(n.error());
        if (*n == 0)
            break;
        if (auto r = writer.write({ buf.data(), *n }); !r)
            return std::unexpected(r.error());
    }
    if (auto r = writer.end_entry(e.crc32, e.compressed_size, e.uncompressed_size); !r)
        return std::unexpected(r.error());
    return CopiedEntry{ w->zip64, e.raw_method, e.crc32, e.compressed_size, e.uncompressed_size };
}

}
