// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace zp
{

enum class ZipMethod : std::uint16_t {
    Store = 0,
    Deflate = 8,
};

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::uint8_t> data);
std::uint32_t crc32_combine(std::uint32_t crc1, std::uint32_t crc2, std::uint64_t len2);

// Encodes one fixed-size segment of an entry independently of all others, so segments can run
// on any thread and the concatenated output is identical at every thread count.
class SegmentEncoder
{
public:
    SegmentEncoder() = default;
    SegmentEncoder(const SegmentEncoder&) = delete;
    SegmentEncoder& operator=(const SegmentEncoder&) = delete;
    SegmentEncoder(SegmentEncoder&&) = delete;
    SegmentEncoder& operator=(SegmentEncoder&&) = delete;
    virtual ~SegmentEncoder() = default;

    [[nodiscard]] virtual ZipMethod method() const = 0;
    [[nodiscard]] virtual std::size_t segment_size() const = 0;
    // Bytes of input preceding the segment that encode() needs as history.
    [[nodiscard]] virtual std::size_t history_size() const = 0;
    virtual Result<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t> history, std::vector<std::uint8_t>&& input, bool last) const = 0;
};

class CodecRegistry
{
public:
    static constexpr std::size_t store_segment_size = 4u << 20;
    static constexpr std::size_t deflate_segment_size = 1u << 20;
    static constexpr std::size_t deflate_history_size = 32u << 10;

    static std::unique_ptr<SegmentEncoder> make_encoder(ZipMethod method, int level);
    static bool can_decode(std::uint16_t method);
    // Wraps a stream of raw entry bytes into a stream of decompressed bytes.
    static Result<std::unique_ptr<IChunkedStream>> make_decoder(std::uint16_t method, std::unique_ptr<IChunkedStream> raw);
};

}
