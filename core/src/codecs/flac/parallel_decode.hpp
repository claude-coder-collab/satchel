// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/flac/flac_format.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace zp::flac
{

// Cuts a fixed-blocksize FLAC frame stream into frames. A boundary is accepted only where a frame
// header starts with a valid CRC-8, the next frame number, and the stream's sample rate and sample
// size. The decoder then checks every frame's CRC-16, so a sync pattern inside audio data that
// passes these checks makes decoding fail; it can never produce wrong output.
class FrameSplitter
{
public:
    static constexpr std::size_t max_frame_bytes = 16u << 20;

    explicit FrameSplitter(std::uint64_t first_number = 0) :
        next_number_(first_number)
    {
    }

    VoidResult feed(std::span<const std::uint8_t> bytes);
    // End of input: the remaining bytes must be one complete frame.
    VoidResult finish();

    // Complete frames found so far (concatenated), removed from the splitter.
    struct Frames
    {
        std::vector<std::uint8_t> bytes;
        std::size_t count = 0;
    };
    Frames take();
    [[nodiscard]] std::size_t ready() const { return ready_.count; }

private:
    VoidResult scan(bool at_end);

    std::vector<std::uint8_t> buf_;
    std::size_t start_ = 0; // offset of the current (incomplete) frame in buf_
    std::size_t scan_ = 0; // where the search for the next frame header resumes
    std::uint64_t next_number_;
    bool checked_first_ = false;
    std::uint16_t format_ = 0;
    Frames ready_;
};

struct ParallelDecodeOptions
{
    std::size_t threads = 2;
    std::size_t frames_per_batch = 64;
    std::size_t read_size = 1u << 20;
};

// Decodes the frames that follow the metadata of a FLAC stream (`frames` is positioned at the
// first frame) on worker threads. `convert` runs on the workers and turns each batch's interleaved
// samples into output bytes; `emit` receives the batches in order on the calling thread. Returns
// the number of samples per channel decoded.
Result<std::uint64_t> decode_parallel(IChunkedStream& frames, const StreamInfo& info, const ParallelDecodeOptions& options, const std::function<void(std::span<const std::int32_t> interleaved, std::vector<std::uint8_t>& out)>& convert, const std::function<VoidResult(std::span<const std::uint8_t>)>& emit);

}
