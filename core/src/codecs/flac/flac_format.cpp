// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/flac_format.hpp"

#include "codecs/deflate.hpp"
#include "common/product.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace zp::flac
{

namespace
{

constexpr std::array<std::uint8_t, 256> make_crc8_table()
{
    std::array<std::uint8_t, 256> t{};
    for (unsigned i = 0; i < 256; ++i)
    {
        unsigned c = i;
        for (int b = 0; b < 8; ++b)
            c = (c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1);
        t[i] = static_cast<std::uint8_t>(c);
    }
    return t;
}

constexpr std::array<std::uint16_t, 256> make_crc16_table()
{
    std::array<std::uint16_t, 256> t{};
    for (unsigned i = 0; i < 256; ++i)
    {
        unsigned c = i << 8;
        for (int b = 0; b < 8; ++b)
            c = (c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1);
        t[i] = static_cast<std::uint16_t>(c);
    }
    return t;
}

constexpr auto crc8_table = make_crc8_table();
constexpr auto crc16_table = make_crc16_table();

void put_coded_number(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    if (v < 0x80)
    {
        out.push_back(static_cast<std::uint8_t>(v));
        return;
    }
    int extra = 0;
    std::uint8_t lead = 0;
    if (v < 0x800)
    {
        extra = 1;
        lead = 0xC0;
    }
    else if (v < 0x10000)
    {
        extra = 2;
        lead = 0xE0;
    }
    else if (v < 0x200000)
    {
        extra = 3;
        lead = 0xF0;
    }
    else if (v < 0x4000000)
    {
        extra = 4;
        lead = 0xF8;
    }
    else if (v < 0x80000000)
    {
        extra = 5;
        lead = 0xFC;
    }
    else
    {
        extra = 6;
        lead = 0xFE;
    }
    out.push_back(static_cast<std::uint8_t>(lead | (v >> (6 * extra))));
    for (int i = extra - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(0x80 | ((v >> (6 * i)) & 0x3F)));
}

// Returns (value, bytes used).
std::optional<std::pair<std::uint64_t, std::size_t>> get_coded_number(std::span<const std::uint8_t> in)
{
    if (in.empty())
        return std::nullopt;
    const auto first = in[0];
    if (first < 0x80)
        return std::pair<std::uint64_t, std::size_t>{ first, 1 };
    int extra = 0;
    std::uint64_t v = 0;
    if ((first & 0xE0) == 0xC0)
    {
        extra = 1;
        v = first & 0x1F;
    }
    else if ((first & 0xF0) == 0xE0)
    {
        extra = 2;
        v = first & 0x0F;
    }
    else if ((first & 0xF8) == 0xF0)
    {
        extra = 3;
        v = first & 0x07;
    }
    else if ((first & 0xFC) == 0xF8)
    {
        extra = 4;
        v = first & 0x03;
    }
    else if ((first & 0xFE) == 0xFC)
    {
        extra = 5;
        v = first & 0x01;
    }
    else if (first == 0xFE)
    {
        extra = 6;
        v = 0;
    }
    else
        return std::nullopt;
    if (in.size() < static_cast<std::size_t>(extra) + 1)
        return std::nullopt;
    for (int i = 1; i <= extra; ++i)
    {
        const auto c = in[static_cast<std::size_t>(i)];
        if ((c & 0xC0) != 0x80)
            return std::nullopt;
        v = (v << 6) | (c & 0x3F);
    }
    return std::pair<std::uint64_t, std::size_t>{ v, static_cast<std::size_t>(extra) + 1 };
}

struct ParsedHeader
{
    std::uint64_t number = 0;
    std::size_t number_offset = 4;
    std::size_t number_length = 0;
    std::size_t tail_length = 0; // optional blocksize/sample-rate bytes after the number
    std::size_t header_length = 0; // including CRC-8
};

std::optional<ParsedHeader> parse_frame_header(std::span<const std::uint8_t> f)
{
    if (f.size() < 6 || f[0] != 0xFF || (f[1] & 0xFE) != 0xF8)
        return std::nullopt;
    if ((f[1] & 1) != 0)
        return std::nullopt; // variable blocksize: not produced by this library
    ParsedHeader h;
    auto n = get_coded_number(f.subspan(4));
    if (!n)
        return std::nullopt;
    h.number = n->first;
    h.number_length = n->second;
    const auto bs_code = f[2] >> 4;
    const auto sr_code = f[2] & 0x0F;
    if (bs_code == 6)
        h.tail_length += 1;
    else if (bs_code == 7)
        h.tail_length += 2;
    if (sr_code == 12)
        h.tail_length += 1;
    else if (sr_code == 13 || sr_code == 14)
        h.tail_length += 2;
    h.header_length = 4 + h.number_length + h.tail_length + 1;
    if (f.size() < h.header_length + 2)
        return std::nullopt;
    return h;
}

constexpr AppId riff_id = { 'r', 'i', 'f', 'f' };
constexpr AppId aiff_id = { 'a', 'i', 'f', 'f' };
constexpr AppId w64_id = { 'w', '6', '4', ' ' };

}

std::uint8_t crc8(std::span<const std::uint8_t> data)
{
    std::uint8_t c = 0;
    for (const auto b : data)
        c = crc8_table[c ^ b];
    return c;
}

std::uint16_t crc16(std::span<const std::uint8_t> data)
{
    std::uint16_t c = 0;
    for (const auto b : data)
        c = static_cast<std::uint16_t>((c << 8) ^ crc16_table[((c >> 8) ^ b) & 0xFF]);
    return c;
}

std::array<std::uint8_t, 34> StreamInfo::serialize() const
{
    std::array<std::uint8_t, 34> o{};
    o[0] = static_cast<std::uint8_t>(min_block_size >> 8);
    o[1] = static_cast<std::uint8_t>(min_block_size);
    o[2] = static_cast<std::uint8_t>(max_block_size >> 8);
    o[3] = static_cast<std::uint8_t>(max_block_size);
    o[4] = static_cast<std::uint8_t>(min_frame_size >> 16);
    o[5] = static_cast<std::uint8_t>(min_frame_size >> 8);
    o[6] = static_cast<std::uint8_t>(min_frame_size);
    o[7] = static_cast<std::uint8_t>(max_frame_size >> 16);
    o[8] = static_cast<std::uint8_t>(max_frame_size >> 8);
    o[9] = static_cast<std::uint8_t>(max_frame_size);
    const std::uint64_t packed = (static_cast<std::uint64_t>(sample_rate & 0xFFFFF) << 44) | (static_cast<std::uint64_t>((channels - 1) & 7) << 41)
        | (static_cast<std::uint64_t>((bits_per_sample - 1) & 31) << 36) | (total_samples & 0xFFFFFFFFFull);
    for (std::size_t i = 0; i < 8; ++i)
        o[10 + i] = static_cast<std::uint8_t>(packed >> (56 - 8 * i));
    std::memcpy(o.data() + 18, md5.data(), 16);
    return o;
}

std::optional<StreamInfo> StreamInfo::parse(std::span<const std::uint8_t> d)
{
    if (d.size() < 34)
        return std::nullopt;
    StreamInfo s;
    s.min_block_size = static_cast<std::uint16_t>((d[0] << 8) | d[1]);
    s.max_block_size = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
    s.min_frame_size = (static_cast<std::uint32_t>(d[4]) << 16) | (static_cast<std::uint32_t>(d[5]) << 8) | d[6];
    s.max_frame_size = (static_cast<std::uint32_t>(d[7]) << 16) | (static_cast<std::uint32_t>(d[8]) << 8) | d[9];
    std::uint64_t packed = 0;
    for (std::size_t i = 0; i < 8; ++i)
        packed = (packed << 8) | d[10 + i];
    s.sample_rate = static_cast<std::uint32_t>(packed >> 44);
    s.channels = static_cast<std::uint32_t>((packed >> 41) & 7) + 1;
    s.bits_per_sample = static_cast<std::uint32_t>((packed >> 36) & 31) + 1;
    s.total_samples = packed & 0xFFFFFFFFFull;
    std::memcpy(s.md5.data(), d.data() + 18, 16);
    return s;
}

Result<std::vector<std::uint8_t>> renumber_frame(std::span<const std::uint8_t> frame, std::uint64_t number)
{
    const auto h = parse_frame_header(frame);
    if (!h)
        return fail(Status::Internal, "unexpected FLAC frame header");
    std::vector<std::uint8_t> out;
    out.reserve(frame.size() + 8);
    out.insert(out.end(), frame.begin(), frame.begin() + 4);
    put_coded_number(out, number);
    const auto tail = frame.subspan(4 + h->number_length, h->tail_length);
    out.insert(out.end(), tail.begin(), tail.end());
    out.push_back(crc8(out));
    const auto body = frame.subspan(h->header_length, frame.size() - h->header_length - 2);
    out.insert(out.end(), body.begin(), body.end());
    const auto c = crc16(out);
    out.push_back(static_cast<std::uint8_t>(c >> 8));
    out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

std::optional<std::uint64_t> frame_start(std::span<const std::uint8_t> bytes)
{
    const auto h = parse_frame_header(bytes);
    if (!h || crc8(bytes.first(h->header_length - 1)) != bytes[h->header_length - 1])
        return std::nullopt;
    return h->number;
}

std::optional<std::uint64_t> frame_number(std::span<const std::uint8_t> frame)
{
    const auto h = parse_frame_header(frame);
    if (!h)
        return std::nullopt;
    return h->number;
}

void append_block_header(ByteWriter& w, BlockType type, std::size_t length, bool last)
{
    w.u8(static_cast<std::uint8_t>((last ? 0x80 : 0x00) | static_cast<std::uint8_t>(type)));
    w.u8(static_cast<std::uint8_t>(length >> 16));
    w.u8(static_cast<std::uint8_t>(length >> 8));
    w.u8(static_cast<std::uint8_t>(length));
}

std::vector<std::uint8_t> VorbisComments::serialize() const
{
    ByteWriter w;
    w.u32le(static_cast<std::uint32_t>(vendor.size()));
    w.str(vendor);
    w.u32le(static_cast<std::uint32_t>(fields.size()));
    for (const auto& [k, v] : fields)
    {
        w.u32le(static_cast<std::uint32_t>(k.size() + 1 + v.size()));
        w.str(k);
        w.u8('=');
        w.str(v);
    }
    return w.take();
}

std::optional<VorbisComments> VorbisComments::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data);
    VorbisComments c;
    const auto vlen = r.u32le();
    if (!vlen)
        return std::nullopt;
    const auto vendor = r.bytes(*vlen);
    if (!vendor)
        return std::nullopt;
    c.vendor.assign(vendor->begin(), vendor->end());
    const auto count = r.u32le();
    if (!count)
        return std::nullopt;
    for (std::uint32_t i = 0; i < *count; ++i)
    {
        const auto len = r.u32le();
        if (!len)
            return std::nullopt;
        const auto field = r.bytes(*len);
        if (!field)
            return std::nullopt;
        std::string s(field->begin(), field->end());
        const auto eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        c.fields.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    return c;
}

std::optional<std::string> VorbisComments::get(std::string_view name) const
{
    for (const auto& [k, v] : fields)
    {
        if (k.size() == name.size() && std::ranges::equal(k, name, [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }))
            return v;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> ProjectBlock::serialize() const
{
    ByteWriter w;
    w.bytes(product::flac_application_id);
    w.u8(schema);
    w.u8(static_cast<std::uint8_t>(layout));
    w.bytes(group_id);
    w.u16be(channel_index);
    w.u16be(channel_count);
    w.u16be(static_cast<std::uint16_t>(original_name.size()));
    w.str(original_name);
    w.u32be(static_cast<std::uint32_t>(private_data.size()));
    w.bytes(private_data);
    w.u32be(static_cast<std::uint32_t>(trailing.size()));
    w.bytes(trailing);
    w.bytes(sha256);
    return w.take();
}

std::size_t ProjectBlock::sha_offset() const
{
    return 4 + 1 + 1 + 16 + 2 + 2 + 2 + original_name.size() + 4 + private_data.size() + 4 + trailing.size();
}

std::optional<ProjectBlock> ProjectBlock::parse(std::span<const std::uint8_t> content)
{
    ByteReader r(content);
    const auto id = r.bytes(4);
    if (!id || !is_project_id(*id))
        return std::nullopt;
    ProjectBlock p;
    const auto schema = r.u8();
    const auto layout = r.u8();
    if (!schema || *schema == 0 || !layout || *layout > 2)
        return std::nullopt;
    p.schema = *schema;
    p.layout = static_cast<Layout>(*layout);
    const auto group = r.bytes(16);
    const auto index = r.u16be();
    const auto count = r.u16be();
    const auto name_len = r.u16be();
    if (!group || !index || !count || !name_len)
        return std::nullopt;
    std::memcpy(p.group_id.data(), group->data(), 16);
    p.channel_index = *index;
    p.channel_count = *count;
    const auto name = r.bytes(*name_len);
    const auto priv_len = name ? r.u32be() : std::nullopt;
    if (!name || !priv_len)
        return std::nullopt;
    p.original_name.assign(name->begin(), name->end());
    const auto priv = r.bytes(*priv_len);
    const auto trail_len = priv ? r.u32be() : std::nullopt;
    if (!priv || !trail_len)
        return std::nullopt;
    p.private_data.assign(priv->begin(), priv->end());
    const auto trail = r.bytes(*trail_len);
    const auto sha = trail ? r.bytes(32) : std::nullopt;
    if (!trail || !sha)
        return std::nullopt;
    p.trailing.assign(trail->begin(), trail->end());
    std::memcpy(p.sha256.data(), sha->data(), 32);
    return p;
}

Result<std::vector<std::uint8_t>> pack_private(const std::vector<ForeignRecord>& records)
{
    ByteWriter w;
    for (const auto& r : records)
    {
        if (r.bytes.size() + 4 > max_block_length)
            return fail(Status::Internal, "record too large for private storage");
        append_block_header(w, BlockType::Application, r.bytes.size() + 4, false);
        w.bytes(r.id);
        w.bytes(r.bytes);
    }
    const auto raw = w.take();
    auto packed = deflate_buffer(raw, 9);
    if (!packed)
        return std::unexpected(packed.error());
    ByteWriter out;
    out.u32be(static_cast<std::uint32_t>(raw.size()));
    out.bytes(*packed);
    return out.take();
}

Result<std::vector<ForeignRecord>> unpack_private(std::span<const std::uint8_t> packed)
{
    std::vector<ForeignRecord> records;
    if (packed.empty())
        return records;
    ByteReader head(packed);
    const auto raw_size = head.u32be();
    if (!raw_size)
        return fail(Status::CorruptArchive, "private storage is truncated");
    auto raw = inflate_buffer(packed.subspan(4), *raw_size);
    if (!raw)
        return std::unexpected(raw.error());
    ByteReader r(*raw);
    while (r.remaining() > 0)
    {
        const auto type = r.u8();
        const auto b1 = r.u8();
        const auto b2 = r.u8();
        const auto b3 = r.u8();
        if (!type || !b1 || !b2 || !b3 || (*type & 0x7F) != static_cast<std::uint8_t>(BlockType::Application))
            return fail(Status::CorruptArchive, "private storage record is damaged");
        const std::size_t len = (static_cast<std::size_t>(*b1) << 16) | (static_cast<std::size_t>(*b2) << 8) | *b3;
        const auto content = r.bytes(len);
        if (!content || len < 4)
            return fail(Status::CorruptArchive, "private storage record is truncated");
        ForeignRecord rec;
        std::memcpy(rec.id.data(), content->data(), 4);
        rec.bytes.assign(content->begin() + 4, content->end());
        records.push_back(std::move(rec));
    }
    return records;
}

bool is_project_id(std::span<const std::uint8_t> id)
{
    return id.size() >= 4 && std::equal(id.begin(), id.begin() + 4, product::flac_application_id.begin());
}

bool is_standard_foreign_id(std::span<const std::uint8_t> id)
{
    if (id.size() < 4)
        return false;
    const auto eq = [&](const AppId& a) { return std::equal(a.begin(), a.end(), id.begin()); };
    return eq(riff_id) || eq(aiff_id) || eq(w64_id);
}

Result<Header> read_header(IChunkedStream& in)
{
    std::array<std::uint8_t, 4> magic{};
    auto n = in.read_full(magic);
    if (!n)
        return std::unexpected(n.error());
    if (*n != 4 || std::memcmp(magic.data(), "fLaC", 4) != 0)
        return fail(Status::CorruptArchive, "not a FLAC stream");
    Header h;
    h.metadata_size = 4;
    bool have_info = false;
    while (true)
    {
        std::array<std::uint8_t, 4> bh{};
        n = in.read_full(bh);
        if (!n)
            return std::unexpected(n.error());
        if (*n != 4)
            return fail(Status::CorruptArchive, "FLAC metadata is truncated");
        const bool last = (bh[0] & 0x80) != 0;
        const auto type = static_cast<BlockType>(bh[0] & 0x7F);
        const std::size_t len = (static_cast<std::size_t>(bh[1]) << 16) | (static_cast<std::size_t>(bh[2]) << 8) | bh[3];
        std::vector<std::uint8_t> body(len);
        n = in.read_full(body);
        if (!n)
            return std::unexpected(n.error());
        if (*n != len)
            return fail(Status::CorruptArchive, "FLAC metadata block is truncated");
        h.metadata_size += 4 + len;
        if (type == BlockType::StreamInfo)
        {
            auto si = StreamInfo::parse(body);
            if (!si)
                return fail(Status::CorruptArchive, "STREAMINFO is damaged");
            h.stream_info = *si;
            have_info = true;
        }
        else if (type == BlockType::VorbisComment)
            h.comments = VorbisComments::parse(body);
        else if (type == BlockType::Application && len >= 4)
        {
            if (is_project_id(body))
            {
                auto p = ProjectBlock::parse(body);
                if (!p)
                    return fail(Status::CorruptArchive, "project block is damaged");
                h.project = std::move(*p);
            }
            else if (is_standard_foreign_id(body))
            {
                ForeignRecord rec;
                std::memcpy(rec.id.data(), body.data(), 4);
                rec.bytes.assign(body.begin() + 4, body.end());
                h.foreign.push_back(std::move(rec));
            }
        }
        if (last)
            break;
    }
    if (!have_info)
        return fail(Status::CorruptArchive, "FLAC stream has no STREAMINFO");
    return h;
}

}
