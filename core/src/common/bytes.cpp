// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/bytes.hpp"

#include <array>

namespace zp
{

void ByteWriter::u16le(std::uint16_t v)
{
    u8(static_cast<std::uint8_t>(v));
    u8(static_cast<std::uint8_t>(v >> 8));
}

void ByteWriter::u32le(std::uint32_t v)
{
    u16le(static_cast<std::uint16_t>(v));
    u16le(static_cast<std::uint16_t>(v >> 16));
}

void ByteWriter::u64le(std::uint64_t v)
{
    u32le(static_cast<std::uint32_t>(v));
    u32le(static_cast<std::uint32_t>(v >> 32));
}

void ByteWriter::u16be(std::uint16_t v)
{
    u8(static_cast<std::uint8_t>(v >> 8));
    u8(static_cast<std::uint8_t>(v));
}

void ByteWriter::u32be(std::uint32_t v)
{
    u16be(static_cast<std::uint16_t>(v >> 16));
    u16be(static_cast<std::uint16_t>(v));
}

std::optional<std::uint8_t> ByteReader::u8()
{
    if (remaining() < 1)
        return std::nullopt;
    return in_[pos_++];
}

std::optional<std::uint16_t> ByteReader::u16le()
{
    if (remaining() < 2)
        return std::nullopt;
    const auto v = static_cast<std::uint16_t>(in_[pos_] | (in_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
}

std::optional<std::uint32_t> ByteReader::u32le()
{
    auto lo = u16le();
    if (!lo)
        return std::nullopt;
    auto hi = u16le();
    if (!hi)
        return std::nullopt;
    return static_cast<std::uint32_t>(*lo) | (static_cast<std::uint32_t>(*hi) << 16);
}

std::optional<std::uint64_t> ByteReader::u64le()
{
    const auto lo = u32le();
    if (!lo)
        return std::nullopt;
    const auto hi = u32le();
    if (!hi)
        return std::nullopt;
    return static_cast<std::uint64_t>(*lo) | (static_cast<std::uint64_t>(*hi) << 32);
}

std::optional<std::uint16_t> ByteReader::u16be()
{
    if (remaining() < 2)
        return std::nullopt;
    const auto v = static_cast<std::uint16_t>((in_[pos_] << 8) | in_[pos_ + 1]);
    pos_ += 2;
    return v;
}

std::optional<std::uint32_t> ByteReader::u32be()
{
    const auto hi = u16be();
    if (!hi)
        return std::nullopt;
    const auto lo = u16be();
    if (!lo)
        return std::nullopt;
    return (static_cast<std::uint32_t>(*hi) << 16) | *lo;
}

std::optional<std::span<const std::uint8_t>> ByteReader::bytes(std::size_t n)
{
    if (remaining() < n)
        return std::nullopt;
    auto s = in_.subspan(pos_, n);
    pos_ += n;
    return s;
}

bool ByteReader::skip(std::size_t n)
{
    if (remaining() < n)
        return false;
    pos_ += n;
    return true;
}

namespace
{
constexpr std::string_view b64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64_encode(std::span<const std::uint8_t> in)
{
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8) | in[i + 2];
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += b64_alphabet[(v >> 6) & 63];
        out += b64_alphabet[v & 63];
    }
    if (const auto rest = in.size() - i; rest > 0)
    {
        std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
        if (rest == 2)
            v |= static_cast<std::uint32_t>(in[i + 1]) << 8;
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += rest == 2 ? b64_alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view in)
{
    std::array<int, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < b64_alphabet.size(); ++i)
        table[static_cast<unsigned char>(b64_alphabet[i])] = static_cast<int>(i);
    if (in.size() % 4 != 0)
        return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4)
    {
        std::uint32_t v = 0;
        int pad = 0;
        for (std::size_t j = 0; j < 4; ++j)
        {
            const char c = in[i + j];
            if (c == '=')
            {
                if (i + 4 != in.size() || j < 2)
                    return std::nullopt;
                ++pad;
                v <<= 6;
                continue;
            }
            if (pad > 0)
                return std::nullopt;
            const int d = table[static_cast<unsigned char>(c)];
            if (d < 0)
                return std::nullopt;
            v = (v << 6) | static_cast<std::uint32_t>(d);
        }
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        if (pad < 2)
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        if (pad < 1)
            out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

}
