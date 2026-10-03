// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zp
{

class ByteWriter
{
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16le(std::uint16_t v);
    void u32le(std::uint32_t v);
    void u64le(std::uint64_t v);
    void u16be(std::uint16_t v);
    void u32be(std::uint32_t v);
    void bytes(std::span<const std::uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void str(std::string_view s) { out_.insert(out_.end(), s.begin(), s.end()); }

    [[nodiscard]] std::vector<std::uint8_t>& data() { return out_; }
    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
};

class ByteReader
{
public:
    explicit ByteReader(std::span<const std::uint8_t> in) :
        in_(in)
    {
    }

    std::optional<std::uint8_t> u8();
    std::optional<std::uint16_t> u16le();
    std::optional<std::uint32_t> u32le();
    std::optional<std::uint64_t> u64le();
    std::optional<std::uint16_t> u16be();
    std::optional<std::uint32_t> u32be();
    std::optional<std::span<const std::uint8_t>> bytes(std::size_t n);
    bool skip(std::size_t n);

    [[nodiscard]] std::size_t remaining() const { return in_.size() - pos_; }
    [[nodiscard]] std::size_t position() const { return pos_; }

private:
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
};

std::string base64_encode(std::span<const std::uint8_t> in);
std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view in);

inline std::span<const std::uint8_t> as_bytes(std::string_view s)
{
    return { reinterpret_cast<const std::uint8_t*>(s.data()), s.size() };
}

}
