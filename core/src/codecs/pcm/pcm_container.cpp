// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/pcm/pcm_container.hpp"

#include "common/bytes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace zp
{

namespace
{

using Guid = std::array<std::uint8_t, 16>;

constexpr Guid w64_riff = { 'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 };
constexpr Guid w64_wave = { 'w', 'a', 'v', 'e', 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A };
constexpr Guid w64_fmt = { 'f', 'm', 't', ' ', 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A };
constexpr Guid w64_data = { 'd', 'a', 't', 'a', 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A };
// KSDATAFORMAT_SUBTYPE_* without the leading 16-bit format code.
constexpr std::array<std::uint8_t, 14> subformat_tail = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

constexpr std::uint16_t wave_format_pcm = 1;
constexpr std::uint16_t wave_format_float = 3;
constexpr std::uint16_t wave_format_extensible = 0xFFFE;

class Scanner
{
public:
    explicit Scanner(IChunkedStream& in, std::uint64_t size) :
        in_(in),
        size_(size)
    {
    }

    Result<std::vector<std::uint8_t>> read(std::uint64_t offset, std::size_t n)
    {
        std::vector<std::uint8_t> buf(n);
        if (offset > size_ || n > size_ - offset)
            return fail(Status::CorruptArchive, "read past end of file");
        if (auto r = in_.seek(offset); !r)
            return std::unexpected(r.error());
        auto got = in_.read_full(buf);
        if (!got)
            return std::unexpected(got.error());
        if (*got != n)
            return fail(Status::SourceChanged, "file is shorter than expected");
        return buf;
    }

    [[nodiscard]] std::uint64_t size() const { return size_; }

private:
    IChunkedStream& in_;
    std::uint64_t size_;
};

PcmScan reject(FallbackReason r, std::string detail)
{
    PcmScan s;
    s.recognized = true;
    s.reason = r;
    s.detail = std::move(detail);
    return s;
}

PcmScan not_parsable(std::string detail)
{
    return reject(FallbackReason::NotParsable, std::move(detail));
}

std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p)
{
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

std::uint16_t le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint64_t be64(const std::uint8_t* p)
{
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

std::uint16_t be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

bool id_is(const std::uint8_t* p, std::string_view id)
{
    return std::memcmp(p, id.data(), id.size()) == 0;
}

std::string printable_id(std::span<const std::uint8_t> id)
{
    std::string s;
    for (const auto c : id)
        s += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    return s;
}

// Common checks once the format is known.
std::optional<PcmScan> check_format(PcmLayout& l)
{
    if (l.channels == 0)
        return not_parsable("no channels");
    if (l.bytes_per_sample < 1 || l.bytes_per_sample > 4)
        return reject(FallbackReason::BitDepth, std::format("{}-byte samples", l.bytes_per_sample));
    if (l.valid_bits < 4 || l.valid_bits > 32 || l.valid_bits > 8u * l.bytes_per_sample)
        return reject(FallbackReason::BitDepth, std::format("{} bits per sample", l.valid_bits));
    if (l.sample_rate == 0 || l.sample_rate > max_flac_sample_rate)
        return reject(FallbackReason::SampleRate, std::format("{} Hz", l.sample_rate));
    return std::nullopt;
}

// Checks the audio chunk and fills frames/padding; then validates that blocks tile the file.
std::optional<PcmScan> finish_layout(Scanner& sc, PcmLayout& l, std::uint64_t container_end, std::uint64_t pad_align)
{
    if (l.audio_size % l.frame_bytes() != 0)
        return not_parsable("audio data ends with a partial sample frame");
    l.frames = l.audio_size / l.frame_bytes();
    if (l.frames >= max_flac_frames)
        return reject(FallbackReason::TooManySamples, std::format("{} samples per channel", l.frames));
    const auto audio_end = l.audio_offset + l.audio_size;
    l.audio_padding = pad_align > 1 ? (pad_align - (audio_end % pad_align)) % pad_align : 0;
    // The pad byte(s) may be missing when the audio chunk ends the file.
    const auto after_audio = l.audio_header_index() + 1 < l.blocks.size() ? l.blocks[l.audio_header_index() + 1].offset : container_end;
    if (audio_end + l.audio_padding != after_audio)
    {
        if (after_audio == audio_end && after_audio == container_end)
            l.audio_padding = 0;
        else
            return not_parsable("unexpected bytes after the audio data");
    }
    if (l.audio_padding > 0)
    {
        auto pad = sc.read(audio_end, static_cast<std::size_t>(l.audio_padding));
        if (!pad)
            return not_parsable("cannot read padding");
        if (std::ranges::any_of(*pad, [](std::uint8_t b) { return b != 0; }))
            return not_parsable("non-zero padding after the audio data");
    }
    l.trailing_offset = container_end;
    l.trailing_size = sc.size() - container_end;
    l.file_size = sc.size();

    std::uint64_t pos = 0;
    for (const auto& b : l.blocks)
    {
        if (b.offset != pos)
            return not_parsable("chunks do not cover the file");
        if (b.length > max_foreign_block)
            return reject(FallbackReason::ChunkTooLarge, std::format("{}-byte chunk at offset {}", b.length, b.offset));
        pos = b.offset + b.length;
        if (b.audio_header)
        {
            if (pos != l.audio_offset)
                return not_parsable("audio does not follow its chunk header");
            pos += l.audio_size + l.audio_padding;
        }
    }
    if (pos != container_end)
        return not_parsable("chunks do not cover the container");
    if (l.trailing_size > max_foreign_block)
        return reject(FallbackReason::ChunkTooLarge, std::format("{} trailing bytes", l.trailing_size));
    return std::nullopt;
}

struct WaveFormat
{
    std::uint16_t tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits = 0;
    std::uint16_t valid_bits = 0;
};

std::optional<PcmScan> parse_wave_format(std::span<const std::uint8_t> body, WaveFormat& f)
{
    if (body.size() < 16)
        return not_parsable("fmt chunk too small");
    f.tag = le16(body.data());
    f.channels = le16(body.data() + 2);
    f.rate = le32(body.data() + 4);
    f.block_align = le16(body.data() + 12);
    f.bits = le16(body.data() + 14);
    f.valid_bits = f.bits;
    if (f.tag == wave_format_extensible)
    {
        if (body.size() < 40)
            return not_parsable("WAVE_FORMAT_EXTENSIBLE fmt chunk too small");
        f.valid_bits = le16(body.data() + 18);
        if (f.valid_bits == 0)
            f.valid_bits = f.bits;
        if (std::memcmp(body.data() + 26, subformat_tail.data(), subformat_tail.size()) != 0)
            return reject(FallbackReason::UnsupportedEncoding, "unknown WAVE_FORMAT_EXTENSIBLE subformat");
        f.tag = le16(body.data() + 24);
    }
    if (f.tag == wave_format_float)
        return reject(FallbackReason::FloatSamples, "IEEE float samples");
    if (f.tag != wave_format_pcm)
        return reject(FallbackReason::UnsupportedEncoding, std::format("WAVE format 0x{:04x}", f.tag));
    return std::nullopt;
}

std::optional<PcmScan> apply_wave_format(const WaveFormat& f, PcmLayout& l)
{
    l.channels = f.channels;
    l.sample_rate = f.rate;
    l.bytes_per_sample = static_cast<std::uint16_t>((f.bits + 7) / 8);
    l.valid_bits = f.valid_bits;
    l.big_endian = false;
    l.unsigned_samples = l.bytes_per_sample == 1;
    if (auto bad = check_format(l))
        return bad;
    if (f.block_align != l.frame_bytes())
        return not_parsable(std::format("block align {} does not match {} channels of {} bytes", f.block_align, l.channels, l.bytes_per_sample));
    return std::nullopt;
}

Result<PcmScan> scan_riff(Scanner& sc, bool rf64)
{
    PcmLayout l;
    l.container = rf64 ? PcmContainer::Rf64 : PcmContainer::Wav;
    auto head = sc.read(0, 12);
    if (!head)
        return std::unexpected(head.error());
    std::uint64_t container_end = 8ull + le32(head->data() + 4);
    std::optional<std::uint64_t> ds64_data_size;
    l.blocks.push_back({ 0, 12, false });

    std::uint64_t pos = 12;
    std::optional<WaveFormat> fmt;
    bool have_data = false;
    bool first_chunk = true;
    while (pos < container_end)
    {
        if (container_end - pos < 8)
            return not_parsable("truncated chunk header");
        auto h = sc.read(pos, 8);
        if (!h)
            return not_parsable("chunk header beyond end of file");
        std::uint64_t size = le32(h->data() + 4);
        const bool is_data = id_is(h->data(), "data");
        if (rf64 && first_chunk)
        {
            if (!id_is(h->data(), "ds64") || size < 28)
                return not_parsable("RF64 file without a ds64 chunk");
            auto body = sc.read(pos + 8, 28);
            if (!body)
                return not_parsable("cannot read ds64");
            container_end = 8 + le64(body->data());
            ds64_data_size = le64(body->data() + 8);
        }
        first_chunk = false;
        if (is_data && rf64 && size == 0xFFFFFFFFu && ds64_data_size)
            size = *ds64_data_size;
        if (is_data)
        {
            if (have_data)
                return not_parsable("more than one data chunk");
            if (!fmt)
                return not_parsable("data chunk before fmt chunk");
            have_data = true;
            l.blocks.push_back({ pos, 8, true });
            l.audio_offset = pos + 8;
            l.audio_size = size;
            if (l.audio_offset + size > container_end)
                return not_parsable("data chunk extends past the end of the container");
            pos = l.audio_offset + size + (size & 1);
            if (pos > container_end && l.audio_offset + size == container_end)
                pos = container_end;
            continue;
        }
        const auto total = 8 + size + (size & 1);
        auto length = total;
        if (pos + total > container_end)
        {
            if (pos + 8 + size == container_end)
                length = 8 + size;
            else
                return not_parsable(std::format("chunk '{}' extends past the end of the container", printable_id({ h->data(), 4 })));
        }
        if (id_is(h->data(), "fmt "))
        {
            if (fmt)
                return not_parsable("more than one fmt chunk");
            if (size > 4096)
                return not_parsable("fmt chunk too large");
            auto body = sc.read(pos + 8, static_cast<std::size_t>(size));
            if (!body)
                return not_parsable("cannot read fmt chunk");
            WaveFormat f;
            if (auto bad = parse_wave_format(*body, f))
                return *bad;
            if (auto bad = apply_wave_format(f, l))
                return *bad;
            fmt = f;
        }
        l.blocks.push_back({ pos, length, false });
        pos += length;
    }
    if (!fmt)
        return not_parsable("no fmt chunk");
    if (!have_data)
        return not_parsable("no data chunk");
    if (container_end > sc.size())
        return not_parsable("container size exceeds the file size");
    if (auto bad = finish_layout(sc, l, container_end, 2))
        return *bad;
    PcmScan s;
    s.recognized = true;
    s.layout = std::move(l);
    return s;
}

std::optional<std::uint32_t> extended_to_rate(const std::uint8_t* p)
{
    const int exponent = ((p[0] & 0x7F) << 8) | p[1];
    const std::uint64_t mantissa = be64(p + 2);
    if ((p[0] & 0x80) != 0 || mantissa == 0)
        return std::nullopt;
    const int shift = exponent - 16383 - 63;
    if (shift > 0 || shift < -63)
        return std::nullopt;
    const auto s = static_cast<unsigned>(-shift);
    if (s > 0 && (mantissa & ((1ull << s) - 1)) != 0)
        return std::nullopt;
    const auto value = mantissa >> s;
    if (value > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

Result<PcmScan> scan_aiff(Scanner& sc, bool aifc)
{
    PcmLayout l;
    l.container = aifc ? PcmContainer::Aifc : PcmContainer::Aiff;
    l.big_endian = true;
    auto head = sc.read(0, 12);
    if (!head)
        return std::unexpected(head.error());
    const std::uint64_t container_end = 8ull + be32(head->data() + 4);
    if (container_end > sc.size())
        return not_parsable("container size exceeds the file size");
    l.blocks.push_back({ 0, 12, false });

    std::uint64_t pos = 12;
    bool have_comm = false;
    bool have_ssnd = false;
    std::uint32_t comm_frames = 0;
    while (pos < container_end)
    {
        if (container_end - pos < 8)
            return not_parsable("truncated chunk header");
        auto h = sc.read(pos, 8);
        if (!h)
            return not_parsable("chunk header beyond end of file");
        const std::uint64_t size = be32(h->data() + 4);
        if (id_is(h->data(), "SSND"))
        {
            if (have_ssnd)
                return not_parsable("more than one SSND chunk");
            if (!have_comm)
                return not_parsable("SSND chunk before COMM chunk");
            if (size < 8)
                return not_parsable("SSND chunk too small");
            auto body = sc.read(pos + 8, 8);
            if (!body)
                return not_parsable("cannot read SSND");
            if (be32(body->data()) != 0)
                return not_parsable("SSND data offset is not zero");
            have_ssnd = true;
            l.blocks.push_back({ pos, 16, true });
            l.audio_offset = pos + 16;
            l.audio_size = static_cast<std::uint64_t>(comm_frames) * l.frame_bytes();
            if (size - 8 != l.audio_size)
                return not_parsable("SSND size does not match the COMM frame count");
            if (l.audio_offset + l.audio_size > container_end)
                return not_parsable("SSND chunk extends past the end of the container");
            pos = l.audio_offset + l.audio_size + (size & 1);
            if (pos > container_end && l.audio_offset + l.audio_size == container_end)
                pos = container_end;
            continue;
        }
        auto length = 8 + size + (size & 1);
        if (pos + length > container_end)
        {
            if (pos + 8 + size == container_end)
                length = 8 + size;
            else
                return not_parsable(std::format("chunk '{}' extends past the end of the container", printable_id({ h->data(), 4 })));
        }
        if (id_is(h->data(), "COMM"))
        {
            if (have_comm)
                return not_parsable("more than one COMM chunk");
            if (size < 18 || size > 4096)
                return not_parsable("COMM chunk has an unexpected size");
            auto body = sc.read(pos + 8, static_cast<std::size_t>(size));
            if (!body)
                return not_parsable("cannot read COMM");
            const auto* b = body->data();
            l.channels = be16(b);
            comm_frames = be32(b + 2);
            const auto bits = be16(b + 6);
            const auto rate = extended_to_rate(b + 8);
            if (!rate)
                return reject(FallbackReason::SampleRate, "sample rate is not a whole number");
            l.sample_rate = *rate;
            l.valid_bits = bits;
            l.bytes_per_sample = static_cast<std::uint16_t>((bits + 7) / 8);
            if (aifc)
            {
                if (size < 22)
                    return not_parsable("AIFF-C COMM chunk too small");
                if (id_is(b + 18, "sowt"))
                    l.big_endian = false;
                else if (id_is(b + 18, "fl32") || id_is(b + 18, "FL32") || id_is(b + 18, "fl64") || id_is(b + 18, "FL64"))
                    return reject(FallbackReason::FloatSamples, "floating-point AIFF-C");
                else if (!id_is(b + 18, "NONE") && !id_is(b + 18, "twos"))
                    return reject(FallbackReason::UnsupportedEncoding, std::format("AIFF-C compression '{}'", printable_id({ b + 18, 4 })));
            }
            if (auto bad = check_format(l))
                return *bad;
            have_comm = true;
        }
        l.blocks.push_back({ pos, length, false });
        pos += length;
    }
    if (!have_comm)
        return not_parsable("no COMM chunk");
    if (!have_ssnd)
    {
        if (comm_frames != 0)
            return not_parsable("no SSND chunk");
        return not_parsable("no SSND chunk (no audio)");
    }
    if (auto bad = finish_layout(sc, l, container_end, 2))
        return *bad;
    PcmScan s;
    s.recognized = true;
    s.layout = std::move(l);
    return s;
}

Result<PcmScan> scan_caf(Scanner& sc)
{
    PcmLayout l;
    l.container = PcmContainer::Caf;
    l.blocks.push_back({ 0, 8, false });
    std::uint64_t pos = 8;
    bool have_desc = false;
    bool have_data = false;
    const auto end = sc.size();
    while (pos < end)
    {
        if (end - pos < 12)
            return not_parsable("truncated chunk header");
        auto h = sc.read(pos, 12);
        if (!h)
            return std::unexpected(h.error());
        const auto raw_size = static_cast<std::int64_t>(be64(h->data() + 4));
        if (id_is(h->data(), "data"))
        {
            if (have_data)
                return not_parsable("more than one data chunk");
            if (!have_desc)
                return not_parsable("data chunk before desc chunk");
            std::uint64_t size = 0;
            if (raw_size == -1)
                size = end - pos - 12;
            else if (raw_size < 4)
                return not_parsable("data chunk too small");
            else
                size = static_cast<std::uint64_t>(raw_size);
            if (pos + 12 + size > end)
                return not_parsable("data chunk extends past the end of the file");
            have_data = true;
            l.blocks.push_back({ pos, 16, true });
            l.audio_offset = pos + 16;
            l.audio_size = size - 4;
            pos += 12 + size;
            continue;
        }
        if (raw_size < 0)
            return not_parsable("negative chunk size");
        const auto size = static_cast<std::uint64_t>(raw_size);
        if (size > end - pos - 12)
            return not_parsable(std::format("chunk '{}' extends past the end of the file", printable_id({ h->data(), 4 })));
        if (id_is(h->data(), "desc"))
        {
            if (have_desc)
                return not_parsable("more than one desc chunk");
            if (size != 32)
                return not_parsable("desc chunk has an unexpected size");
            auto body = sc.read(pos + 12, 32);
            if (!body)
                return std::unexpected(body.error());
            const auto* b = body->data();
            const auto rate = std::bit_cast<double>(be64(b));
            const auto flags = be32(b + 12);
            const auto bytes_per_packet = be32(b + 16);
            const auto frames_per_packet = be32(b + 20);
            l.channels = static_cast<std::uint16_t>(std::min<std::uint32_t>(be32(b + 24), 0xFFFF));
            const auto bits = be32(b + 28);
            if (!id_is(b + 8, "lpcm"))
                return reject(FallbackReason::UnsupportedEncoding, std::format("CAF format '{}'", printable_id({ b + 8, 4 })));
            if (flags & 1u)
                return reject(FallbackReason::FloatSamples, "floating-point CAF");
            if (!(rate > 0) || rate != std::floor(rate) || rate > max_flac_sample_rate)
                return reject(FallbackReason::SampleRate, std::format("{} Hz", rate));
            l.sample_rate = static_cast<std::uint32_t>(rate);
            l.big_endian = (flags & 2u) == 0;
            l.valid_bits = static_cast<std::uint16_t>(std::min<std::uint32_t>(bits, 64));
            l.bytes_per_sample = l.channels ? static_cast<std::uint16_t>(std::min<std::uint32_t>(bytes_per_packet / l.channels, 64)) : 0;
            if (frames_per_packet != 1)
                return not_parsable("CAF packets with more than one frame");
            if (auto bad = check_format(l))
                return *bad;
            if (bytes_per_packet != l.frame_bytes() || l.bytes_per_sample != (bits + 7) / 8)
                return not_parsable("CAF packet size does not match the sample format");
            have_desc = true;
        }
        l.blocks.push_back({ pos, 12 + size, false });
        pos += 12 + size;
    }
    if (!have_desc)
        return not_parsable("no desc chunk");
    if (!have_data)
        return not_parsable("no data chunk");
    if (auto bad = finish_layout(sc, l, end, 1))
        return *bad;
    PcmScan s;
    s.recognized = true;
    s.layout = std::move(l);
    return s;
}

Result<PcmScan> scan_w64(Scanner& sc)
{
    PcmLayout l;
    l.container = PcmContainer::Wave64;
    auto head = sc.read(0, 40);
    if (!head)
        return std::unexpected(head.error());
    const std::uint64_t container_end = le64(head->data() + 16);
    if (container_end > sc.size() || container_end < 40)
        return not_parsable("container size does not match the file size");
    l.blocks.push_back({ 0, 40, false });
    std::uint64_t pos = 40;
    std::optional<WaveFormat> fmt;
    bool have_data = false;
    const auto align8 = [](std::uint64_t v) -> std::uint64_t { return (v + 7) & ~std::uint64_t{ 7 }; };
    while (pos < container_end)
    {
        if (container_end - pos < 24)
            return not_parsable("truncated chunk header");
        auto h = sc.read(pos, 24);
        if (!h)
            return not_parsable("chunk header beyond end of file");
        const auto size = le64(h->data() + 16);
        if (size < 24 || size > container_end - pos)
            return not_parsable("chunk size out of range");
        if (std::memcmp(h->data(), w64_data.data(), 16) == 0)
        {
            if (have_data)
                return not_parsable("more than one data chunk");
            if (!fmt)
                return not_parsable("data chunk before fmt chunk");
            have_data = true;
            l.blocks.push_back({ pos, 24, true });
            l.audio_offset = pos + 24;
            l.audio_size = size - 24;
            pos = std::min(container_end, pos + align8(size));
            continue;
        }
        const auto length = std::min(container_end - pos, align8(size));
        if (std::memcmp(h->data(), w64_fmt.data(), 16) == 0)
        {
            if (fmt)
                return not_parsable("more than one fmt chunk");
            if (size - 24 > 4096)
                return not_parsable("fmt chunk too large");
            auto body = sc.read(pos + 24, static_cast<std::size_t>(size - 24));
            if (!body)
                return not_parsable("cannot read fmt chunk");
            WaveFormat f;
            if (auto bad = parse_wave_format(*body, f))
                return *bad;
            if (auto bad = apply_wave_format(f, l))
                return *bad;
            fmt = f;
        }
        l.blocks.push_back({ pos, length, false });
        pos += length;
    }
    if (!fmt)
        return not_parsable("no fmt chunk");
    if (!have_data)
        return not_parsable("no data chunk");
    if (auto bad = finish_layout(sc, l, container_end, 8))
        return *bad;
    PcmScan s;
    s.recognized = true;
    s.layout = std::move(l);
    return s;
}

}

std::string_view container_name(PcmContainer c)
{
    switch (c)
    {
        case PcmContainer::Wav:
            return "WAV";
        case PcmContainer::Rf64:
            return "RF64";
        case PcmContainer::Aiff:
            return "AIFF";
        case PcmContainer::Aifc:
            return "AIFF-C";
        case PcmContainer::Caf:
            return "CAF";
        case PcmContainer::Wave64:
            return "Wave64";
    }
    return "unknown";
}

std::optional<std::array<std::uint8_t, 4>> PcmLayout::standard_application_id() const
{
    switch (container)
    {
        case PcmContainer::Wav:
        case PcmContainer::Rf64:
            return std::array<std::uint8_t, 4>{ 'r', 'i', 'f', 'f' };
        case PcmContainer::Aiff:
        case PcmContainer::Aifc:
            return std::array<std::uint8_t, 4>{ 'a', 'i', 'f', 'f' };
        case PcmContainer::Wave64:
            return std::array<std::uint8_t, 4>{ 'w', '6', '4', ' ' };
        case PcmContainer::Caf:
            return std::nullopt;
    }
    return std::nullopt;
}

std::size_t PcmLayout::audio_header_index() const
{
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        if (blocks[i].audio_header)
            return i;
    }
    return blocks.size();
}

std::uint64_t PcmLayout::non_audio_bytes() const
{
    std::uint64_t n = trailing_size;
    for (const auto& b : blocks)
        n += b.length;
    return n;
}

Result<PcmScan> scan_pcm(IChunkedStream& in)
{
    const auto size = in.size();
    if (!size || !in.seekable())
        return PcmScan{};
    Scanner sc(in, *size);
    if (*size < 12)
        return PcmScan{};
    auto head = sc.read(0, 12);
    if (!head)
        return std::unexpected(head.error());
    const auto* h = head->data();
    if (id_is(h, "RIFF") && id_is(h + 8, "WAVE"))
        return scan_riff(sc, false);
    if ((id_is(h, "RF64") || id_is(h, "BW64")) && id_is(h + 8, "WAVE"))
        return scan_riff(sc, true);
    if (id_is(h, "FORM") && id_is(h + 8, "AIFF"))
        return scan_aiff(sc, false);
    if (id_is(h, "FORM") && id_is(h + 8, "AIFC"))
        return scan_aiff(sc, true);
    if (id_is(h, "caff") && be16(h + 4) == 1)
        return scan_caf(sc);
    if (*size >= 40 && std::memcmp(h, w64_riff.data(), 12) == 0)
    {
        auto full = sc.read(0, 40);
        if (!full)
            return std::unexpected(full.error());
        if (std::memcmp(full->data(), w64_riff.data(), 16) == 0 && std::memcmp(full->data() + 24, w64_wave.data(), 16) == 0)
            return scan_w64(sc);
    }
    return PcmScan{};
}

void decode_samples(const PcmLayout& layout, std::span<const std::uint8_t> in, std::span<std::int32_t> out)
{
    const auto width = layout.bytes_per_sample;
    const std::size_t n = std::min(out.size(), in.size() / width);
    const auto* p = in.data();
    for (std::size_t i = 0; i < n; ++i, p += width)
    {
        std::uint32_t v = 0;
        if (layout.big_endian)
        {
            for (std::size_t b = 0; b < width; ++b)
                v = (v << 8) | p[b];
        }
        else
        {
            for (std::size_t b = width; b-- > 0;)
                v = (v << 8) | p[b];
        }
        const unsigned shift = 32u - 8u * width;
        if (layout.unsigned_samples)
            v ^= 1u << (8u * width - 1);
        out[i] = static_cast<std::int32_t>(v << shift) >> shift;
    }
}

void decode_channel(const PcmLayout& layout, std::span<const std::uint8_t> in, std::uint16_t channel, std::span<std::int32_t> out)
{
    const auto width = layout.bytes_per_sample;
    const auto stride = layout.frame_bytes();
    const std::size_t n = std::min(out.size(), in.size() / stride);
    const auto* p = in.data() + static_cast<std::size_t>(channel) * width;
    const unsigned shift = 32u - 8u * width;
    for (std::size_t i = 0; i < n; ++i, p += stride)
    {
        std::uint32_t v = 0;
        if (layout.big_endian)
        {
            for (std::size_t b = 0; b < width; ++b)
                v = (v << 8) | p[b];
        }
        else
        {
            for (std::size_t b = width; b-- > 0;)
                v = (v << 8) | p[b];
        }
        if (layout.unsigned_samples)
            v ^= 1u << (8u * width - 1);
        out[i] = static_cast<std::int32_t>(v << shift) >> shift;
    }
}

void encode_samples(const PcmLayout& layout, std::span<const std::int32_t> in, std::span<std::uint8_t> out)
{
    const auto width = layout.bytes_per_sample;
    const std::size_t n = std::min(in.size(), out.size() / width);
    auto* p = out.data();
    for (std::size_t i = 0; i < n; ++i, p += width)
    {
        auto v = static_cast<std::uint32_t>(in[i]);
        if (layout.unsigned_samples)
            v ^= 1u << (8u * width - 1);
        if (layout.big_endian)
        {
            for (std::size_t b = width; b-- > 0;)
            {
                p[b] = static_cast<std::uint8_t>(v);
                v >>= 8;
            }
        }
        else
        {
            for (std::size_t b = 0; b < width; ++b)
            {
                p[b] = static_cast<std::uint8_t>(v);
                v >>= 8;
            }
        }
    }
}

std::span<const std::uint8_t> md5_bytes(const PcmLayout& layout, std::span<const std::uint8_t> in, std::vector<std::uint8_t>& scratch)
{
    if (!layout.big_endian && !layout.unsigned_samples)
        return in;
    const auto width = layout.bytes_per_sample;
    scratch.resize(in.size() - in.size() % width);
    for (std::size_t i = 0; i + width <= in.size(); i += width)
    {
        for (std::size_t b = 0; b < width; ++b)
            scratch[i + b] = layout.big_endian ? in[i + width - 1 - b] : in[i + b];
        if (layout.unsigned_samples)
            scratch[i + width - 1] ^= 0x80;
    }
    return scratch;
}

}
