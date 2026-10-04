// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// libFuzzer: open an arbitrary archive, plan an extraction and verify it to the null sink.
#include "io/stream.hpp"
#include "io/zip/extractor.hpp"
#include "io/zip/output_sink.hpp"
#include "io/zip/reader.hpp"
#include "pipeline/context.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static zp::Context context(2, 64ull << 20);
    zp::MemoryStream in(std::vector<std::uint8_t>(data, data + size));
    auto reader = zp::ArchiveReader::open(in);
    if (!reader)
        return 0;
    (*reader)->probe_flac();
    zp::NullOutputSink sink;
    zp::ArchiveExtractor extractor(**reader, sink, context, { .restore_wav = true, .include_readme = true, .overwrite = zp::OverwritePolicy::Replace });
    auto plan = extractor.plan_extraction({});
    if (!plan)
        return 0;
    zp::ProgressSink progress;
    extractor.execute(*plan, progress);
    return 0;
}
