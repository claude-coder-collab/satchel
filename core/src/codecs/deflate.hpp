// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/codec.hpp"

#include <memory>
#include <vector>

namespace zp
{

// pigz-style segment: the previous 32 KiB of input is the preset dictionary; every segment but
// the last ends with a sync flush, so the concatenation is one valid raw deflate stream.
class DeflateSegmentEncoder final : public SegmentEncoder
{
public:
    explicit DeflateSegmentEncoder(int level) :
        level_(level)
    {
    }

    [[nodiscard]] ZipMethod method() const override { return ZipMethod::Deflate; }
    [[nodiscard]] std::size_t segment_size() const override { return CodecRegistry::deflate_segment_size; }
    [[nodiscard]] std::size_t history_size() const override { return CodecRegistry::deflate_history_size; }
    Result<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t> history, std::vector<std::uint8_t>&& input, bool last) const override;

private:
    int level_;
};

// Raw deflate in one call, for small buffers (store heuristic sample, private metadata).
Result<std::vector<std::uint8_t>> deflate_buffer(std::span<const std::uint8_t> input, int level);
Result<std::vector<std::uint8_t>> inflate_buffer(std::span<const std::uint8_t> input, std::size_t expected_size);

class InflateStream final : public IChunkedStream
{
public:
    static Result<std::unique_ptr<IChunkedStream>> create(std::unique_ptr<IChunkedStream> raw);
    InflateStream(const InflateStream&) = delete;
    InflateStream& operator=(const InflateStream&) = delete;
    InflateStream(InflateStream&&) = delete;
    InflateStream& operator=(InflateStream&&) = delete;
    ~InflateStream() override;

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override;
    [[nodiscard]] std::uint64_t tell() const override { return produced_; }

private:
    InflateStream() = default;

    struct State;
    std::unique_ptr<IChunkedStream> raw_;
    std::unique_ptr<State> state_;
    std::vector<std::uint8_t> in_;
    std::uint64_t produced_ = 0;
    bool finished_ = false;
    bool raw_eof_ = false;
};

}
