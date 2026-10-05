// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/flac/flac_format.hpp"
#include "codecs/pcm/pcm_container.hpp"
#include "common/status.hpp"
#include "io/stream.hpp"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace zp::flac
{

// Receives the rebuilt file in order. Returning an error stops the restore.
using RestoreSink = std::function<VoidResult(std::span<const std::uint8_t>)>;

// Opens the audio stream of member k again from its start.
using MemberOpener = std::function<Result<std::unique_ptr<IChunkedStream>>(std::size_t member)>;

// Checks that the headers form one complete multi-mono group (or a single file) and returns
// the group's members in channel order.
Result<std::vector<std::size_t>> order_members(const std::vector<Header>& headers);

// The container layout described by the foreign records, for audio of `frames` frames.
Result<PcmLayout> layout_from_records(const std::vector<ForeignRecord>& records, std::uint32_t channels, std::uint32_t bytes_per_sample, std::uint64_t frames, std::uint64_t trailing_size);

// The size of the original file a FLAC file (layouts 0 and 2) or the lead member (channel 1) of a
// multi-mono group was made from, worked out from the stored metadata alone. `channels` is the
// channel count of the original file.
Result<std::uint64_t> original_file_size(const Header& lead, std::uint32_t channels);

// Rebuilds the original file from a FLAC file (layouts 0 and 2, or a file written by
// `flac --keep-foreign-metadata`) or from a multi-mono group. headers[k] belongs to member k
// (channel order). The SHA-256 of the output is checked when the project block has one; otherwise
// the FLAC MD5 signature is checked. With threads > 1 a single FLAC stream is decoded in parallel
// (batches of frames on worker threads); `sink` is always called from the calling thread.
VoidResult restore(const std::vector<Header>& headers, const MemberOpener& open, const RestoreSink& sink, std::size_t threads = 1);

}
