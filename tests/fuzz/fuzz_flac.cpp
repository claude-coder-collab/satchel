// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// libFuzzer: treat the input as a standalone FLAC file; split its frames and restore it with
// parallel decoding.
#include "codecs/flac/parallel_decode.hpp"
#include "codecs/flac/restore.hpp"
#include "io/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::vector<std::uint8_t> bytes(data, data + size);
    zp::MemoryStream in(bytes);
    auto header = zp::flac::read_header(in);
    if (!header)
        return 0;
    if (header->metadata_size <= size)
    {
        zp::flac::FrameSplitter splitter;
        if (splitter.feed(std::span(bytes).subspan(static_cast<std::size_t>(header->metadata_size))))
            (void) splitter.finish();
    }
    for (const std::size_t threads : { std::size_t{ 1 }, std::size_t{ 3 } })
    {
        std::uint64_t written = 0;
        (void) zp::flac::restore(
            { *header }, [&](std::size_t) -> zp::Result<std::unique_ptr<zp::IChunkedStream>> { return std::make_unique<zp::MemoryStream>(bytes); }, [&](std::span<const std::uint8_t> b) -> zp::VoidResult {
                written += b.size();
                if (written > (64u << 20))
                    return zp::fail(zp::Status::InvalidArgument, "too large");
                return {}; }, threads
        );
    }
    return 0;
}
