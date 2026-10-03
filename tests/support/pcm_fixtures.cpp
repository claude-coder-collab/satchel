// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "pcm_fixtures.hpp"

#include "common/bytes.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <random>

namespace zp::test
{

namespace
{

constexpr std::array<std::uint8_t, 16> w64_riff = { 'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 };
constexpr std::array<std::uint8_t, 16> w64_wave = { 'w', 'a', 'v', 'e', 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A };
constexpr std::array<std::uint8_t, 12> w64_tail = { 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A };

void riff_chunk(ByteWriter& w, std::string_view id, std::span<const std::uint8_t> body)
{
    w.str(id);
    w.u32le(static_cast<std::uint32_t>(body.size()));
    w.bytes(body);
    if (body.size() & 1)
        w.u8(0);
}

void aiff_chunk(ByteWriter& w, std::string_view id, std::span<const std::uint8_t> body)
{
    w.str(id);
    w.u32be(static_cast<std::uint32_t>(body.size()));
    w.bytes(body);
    if (body.size() & 1)
        w.u8(0);
}

void w64_chunk(ByteWriter& w, std::string_view id, std::span<const std::uint8_t> body)
{
    w.str(id);
    w.bytes(w64_tail);
    w.u64le(24 + body.size());
    w.bytes(body);
    while (w.data().size() % 8)
        w.u8(0);
}

std::vector<std::uint8_t> wave_fmt(const WavSpec& s)
{
    const std::uint16_t cbits = s.container_bits ? s.container_bits : static_cast<std::uint16_t>((s.bits + 7) / 8 * 8);
    const std::uint16_t align = static_cast<std::uint16_t>(s.channels * cbits / 8);
    ByteWriter f;
    f.u16le(s.extensible ? 0xFFFE : s.format_tag);
    f.u16le(s.channels);
    f.u32le(s.rate);
    f.u32le(s.rate * align);
    f.u16le(align);
    f.u16le(cbits);
    if (s.extensible)
    {
        f.u16le(22);
        f.u16le(s.bits);
        f.u32le(s.channels == 2 ? 3u : 0u);
        f.u16le(s.format_tag);
        const std::array<std::uint8_t, 14> tail = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        f.bytes(tail);
    }
    return f.take();
}

std::vector<std::uint8_t> wav_audio(const WavSpec& s)
{
    const std::uint16_t bytes = s.container_bits ? static_cast<std::uint16_t>(s.container_bits / 8) : static_cast<std::uint16_t>((s.bits + 7) / 8);
    if (s.format_tag == 3)
    {
        std::vector<std::uint8_t> out;
        for (std::uint64_t i = 0; i < s.frames * s.channels; ++i)
        {
            const float v = static_cast<float>(std::sin(static_cast<double>(i) * 0.01) * 0.5);
            const auto u = std::bit_cast<std::uint32_t>(v);
            for (int b = 0; b < 4; ++b)
                out.push_back(static_cast<std::uint8_t>(u >> (8 * b)));
        }
        return out;
    }
    return audio_bytes(s.frames, s.channels, bytes, false, bytes == 1, s.seed);
}

std::vector<std::uint8_t> extended_rate(std::uint32_t rate)
{
    std::vector<std::uint8_t> out(10, 0);
    int exponent = 16383 + 63;
    std::uint64_t mantissa = rate;
    while ((mantissa & (1ull << 63)) == 0)
    {
        mantissa <<= 1;
        --exponent;
    }
    out[0] = static_cast<std::uint8_t>(exponent >> 8);
    out[1] = static_cast<std::uint8_t>(exponent);
    for (int i = 0; i < 8; ++i)
        out[static_cast<std::size_t>(2 + i)] = static_cast<std::uint8_t>(mantissa >> (56 - 8 * i));
    return out;
}

}

std::vector<std::uint8_t> audio_bytes(std::uint64_t frames, std::uint16_t channels, std::uint16_t bytes_per_sample, bool big_endian, bool unsigned_8bit, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(frames * channels * bytes_per_sample));
    const double full = std::ldexp(1.0, 8 * bytes_per_sample - 1) - 1;
    for (std::uint64_t f = 0; f < frames; ++f)
    {
        for (std::uint16_t c = 0; c < channels; ++c)
        {
            const double t = static_cast<double>(f);
            const double v = 0.4 * std::sin(t * (0.01 + 0.003 * c)) + 0.1 * std::sin(t * 0.17) + 0.002 * (static_cast<double>(rng() % 1000) / 500.0 - 1.0);
            auto s = static_cast<std::int64_t>(v * full);
            auto u = static_cast<std::uint32_t>(s);
            if (unsigned_8bit)
                u ^= 0x80;
            for (std::uint16_t b = 0; b < bytes_per_sample; ++b)
            {
                const auto shift = big_endian ? 8 * (bytes_per_sample - 1 - b) : 8 * b;
                out.push_back(static_cast<std::uint8_t>(u >> shift));
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> make_wav(const WavSpec& s)
{
    ByteWriter body;
    body.str("WAVE");
    riff_chunk(body, "fmt ", wave_fmt(s));
    for (const auto& c : s.chunks)
    {
        if (!c.after_audio)
            riff_chunk(body, c.id, c.body);
    }
    const auto audio = wav_audio(s);
    body.str("data");
    body.u32le(s.placeholder_sizes ? 0xFFFFFFFFu : static_cast<std::uint32_t>(audio.size()));
    body.bytes(audio);
    if (audio.size() & 1)
        body.u8(0);
    for (const auto& c : s.chunks)
    {
        if (c.after_audio)
            riff_chunk(body, c.id, c.body);
    }
    ByteWriter w;
    w.str("RIFF");
    w.u32le(s.placeholder_sizes ? 0xFFFFFFFFu : static_cast<std::uint32_t>(body.data().size()));
    w.bytes(body.data());
    w.bytes(s.trailing);
    return w.take();
}

std::vector<std::uint8_t> make_rf64(const WavSpec& s)
{
    const auto audio = wav_audio(s);
    ByteWriter rest;
    riff_chunk(rest, "fmt ", wave_fmt(s));
    for (const auto& c : s.chunks)
    {
        if (!c.after_audio)
            riff_chunk(rest, c.id, c.body);
    }
    rest.str("data");
    rest.u32le(0xFFFFFFFFu);
    rest.bytes(audio);
    if (audio.size() & 1)
        rest.u8(0);
    for (const auto& c : s.chunks)
    {
        if (c.after_audio)
            riff_chunk(rest, c.id, c.body);
    }
    const std::uint64_t riff_size = 4 + 8 + 28 + rest.data().size();
    ByteWriter w;
    w.str("RF64");
    w.u32le(0xFFFFFFFFu);
    w.str("WAVE");
    w.str("ds64");
    w.u32le(28);
    w.u64le(riff_size);
    w.u64le(audio.size());
    w.u64le(s.frames);
    w.u32le(0);
    w.bytes(rest.data());
    w.bytes(s.trailing);
    return w.take();
}

std::vector<std::uint8_t> make_w64(const WavSpec& s)
{
    ByteWriter body;
    w64_chunk(body, "fmt ", wave_fmt(s));
    for (const auto& c : s.chunks)
    {
        if (!c.after_audio)
            w64_chunk(body, c.id, c.body);
    }
    w64_chunk(body, "data", wav_audio(s));
    for (const auto& c : s.chunks)
    {
        if (c.after_audio)
            w64_chunk(body, c.id, c.body);
    }
    ByteWriter w;
    w.bytes(w64_riff);
    w.u64le(40 + body.data().size());
    w.bytes(w64_wave);
    w.bytes(body.data());
    return w.take();
}

std::vector<std::uint8_t> make_aiff(const AiffSpec& s)
{
    const std::uint16_t bytes = static_cast<std::uint16_t>((s.bits + 7) / 8);
    ByteWriter comm;
    comm.u16be(s.channels);
    comm.u32be(static_cast<std::uint32_t>(s.frames));
    comm.u16be(s.bits);
    comm.bytes(extended_rate(s.rate));
    if (s.aifc)
    {
        comm.str(s.compression);
        comm.u8(0);
        comm.u8(0);
    }
    ByteWriter body;
    body.str(s.aifc ? "AIFC" : "AIFF");
    if (s.aifc)
    {
        const std::array<std::uint8_t, 4> ver = { 0xA2, 0x80, 0x51, 0x40 };
        aiff_chunk(body, "FVER", ver);
    }
    aiff_chunk(body, "COMM", comm.data());
    for (const auto& c : s.chunks)
    {
        if (!c.after_audio)
            aiff_chunk(body, c.id, c.body);
    }
    ByteWriter ssnd;
    ssnd.u32be(0);
    ssnd.u32be(0);
    ssnd.bytes(audio_bytes(s.frames, s.channels, bytes, !(s.aifc && s.compression == "sowt"), false, s.seed));
    aiff_chunk(body, "SSND", ssnd.data());
    for (const auto& c : s.chunks)
    {
        if (c.after_audio)
            aiff_chunk(body, c.id, c.body);
    }
    ByteWriter w;
    w.str("FORM");
    w.u32be(static_cast<std::uint32_t>(body.data().size()));
    w.bytes(body.data());
    return w.take();
}

std::vector<std::uint8_t> make_caf(const CafSpec& s)
{
    const std::uint16_t bytes = static_cast<std::uint16_t>((s.bits + 7) / 8);
    ByteWriter w;
    w.str("caff");
    w.u16be(1);
    w.u16be(0);
    w.str("desc");
    w.u32be(0);
    w.u32be(32);
    w.u32be(static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(s.rate) >> 32));
    w.u32be(static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(s.rate)));
    w.str("lpcm");
    w.u32be((s.floating ? 1u : 0u) | (s.little_endian ? 2u : 0u));
    w.u32be(static_cast<std::uint32_t>(s.channels) * bytes);
    w.u32be(1);
    w.u32be(s.channels);
    w.u32be(s.bits);
    w.str("info");
    const std::string info = std::string("\0\0\0\1title\0CAF test\0", 19);
    w.u32be(0);
    w.u32be(static_cast<std::uint32_t>(info.size()));
    w.str(info);
    const auto audio = audio_bytes(s.frames, s.channels, bytes, !s.little_endian, false, s.seed);
    w.str("data");
    if (s.unknown_data_size)
    {
        w.u32be(0xFFFFFFFFu);
        w.u32be(0xFFFFFFFFu);
    }
    else
    {
        w.u32be(0);
        w.u32be(static_cast<std::uint32_t>(audio.size() + 4));
    }
    w.u32be(0);
    w.bytes(audio);
    return w.take();
}

std::vector<std::uint8_t> bext_chunk(const std::string& description, const std::string& originator, std::uint64_t time_reference)
{
    std::vector<std::uint8_t> b(602, 0);
    std::memcpy(b.data(), description.data(), std::min<std::size_t>(description.size(), 256));
    std::memcpy(b.data() + 256, originator.data(), std::min<std::size_t>(originator.size(), 32));
    std::memcpy(b.data() + 288, "REF-0001", 8);
    std::memcpy(b.data() + 320, "2026-09-30", 10);
    std::memcpy(b.data() + 330, "12:34:56", 8);
    for (int i = 0; i < 8; ++i)
        b[338 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(time_reference >> (8 * i));
    return b;
}

std::vector<std::uint8_t> ixml_chunk(const std::string& project, const std::string& scene, const std::string& take, const std::vector<std::pair<int, std::string>>& tracks)
{
    std::string xml = std::format("<?xml version=\"1.0\"?><BWFXML><PROJECT>{}</PROJECT><SCENE>{}</SCENE><TAKE>{}</TAKE><TRACK_LIST>", project, scene, take);
    for (const auto& [index, name] : tracks)
        xml += std::format("<TRACK><CHANNEL_INDEX>{}</CHANNEL_INDEX><NAME>{}</NAME></TRACK>", index, name);
    xml += "</TRACK_LIST></BWFXML>";
    return { xml.begin(), xml.end() };
}

}
