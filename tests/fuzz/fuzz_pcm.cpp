// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// libFuzzer: the PCM container scanner, and the FLAC metadata reader, on arbitrary bytes.
#include "codecs/flac/flac_format.hpp"
#include "codecs/pcm/pcm_container.hpp"
#include "io/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    {
        zp::MemoryStream in(std::vector<std::uint8_t>(data, data + size));
        auto scan = zp::scan_pcm(in);
        if (scan && scan->layout)
        {
            const auto& l = *scan->layout;
            std::uint64_t pos = 0;
            for (const auto& b : l.blocks)
            {
                if (b.offset != pos)
                    __builtin_trap();
                pos = b.offset + b.length + (b.audio_header ? l.audio_size + l.audio_padding : 0);
            }
            if (pos + l.trailing_size != size)
                __builtin_trap();
        }
    }
    {
        zp::MemoryStream in(std::vector<std::uint8_t>(data, data + size));
        auto header = zp::flac::read_header(in);
        if (header && header->project)
            (void) zp::flac::unpack_private(header->project->private_data);
    }
    return 0;
}
