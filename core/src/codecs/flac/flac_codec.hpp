// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/flac/flac_format.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace zp::flac
{

struct EncoderSettings
{
    std::uint32_t channels = 2;
    std::uint32_t bits_per_sample = 16;
    std::uint32_t sample_rate = 48000;
    int level = 5;
};

struct EncodedSegment
{
    std::vector<std::uint8_t> bytes; // renumbered frames, concatenated
    std::uint32_t min_frame_size = 0;
    std::uint32_t max_frame_size = 0;
    std::uint32_t frame_count = 0;
};

// Encodes up to blocks_per_segment * block_size frames of interleaved samples with a fresh
// single-threaded libFLAC encoder. Metadata written by libFLAC is discarded; frames are
// renumbered starting at first_frame_number.
Result<EncodedSegment> encode_segment(const EncoderSettings& settings, std::span<const std::int32_t> interleaved, std::uint64_t first_frame_number);

std::string vendor_string();

// Pulls decoded audio from a FLAC stream (metadata is skipped by libFLAC). Checks the
// STREAMINFO MD5 when it is set.
class Decoder
{
public:
    static Result<std::unique_ptr<Decoder>> open(IChunkedStream& in);
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
    ~Decoder();

    // Next block of interleaved samples; empty at end of stream.
    Result<std::span<const std::int32_t>> next();
    [[nodiscard]] std::uint32_t channels() const { return channels_; }
    [[nodiscard]] std::uint32_t bits_per_sample() const { return bits_; }
    [[nodiscard]] std::uint64_t total_samples() const { return total_; }
    // Call after the last block: verifies the MD5 signature (if present).
    VoidResult finish();

private:
    Decoder() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint32_t channels_ = 0;
    std::uint32_t bits_ = 0;
    std::uint64_t total_ = 0;
    friend struct DecoderCallbacks;
};

}
