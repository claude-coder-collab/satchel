// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/stream.hpp"
#include "io/zip/plan.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zp
{

enum class PcmContainer : std::uint8_t {
    Wav,
    Rf64,
    Aiff,
    Aifc,
    Caf,
    Wave64,
};

std::string_view container_name(PcmContainer c);

// A byte range of the original file stored verbatim as one foreign metadata record.
struct ForeignBlock
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool audio_header = false;
};

// Result of a header-only walk: the file is exactly
//   blocks before the audio header | audio header | samples | zero padding | blocks after | trailing
struct PcmLayout
{
    PcmContainer container = PcmContainer::Wav;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bytes_per_sample = 0; // container width: FLAC encodes 8 * bytes_per_sample bits
    std::uint16_t valid_bits = 0;
    bool big_endian = false;
    bool unsigned_samples = false; // 8-bit WAV / Wave64
    std::uint64_t frames = 0;
    std::vector<ForeignBlock> blocks;
    std::uint64_t audio_offset = 0;
    std::uint64_t audio_size = 0;
    std::uint64_t audio_padding = 0;
    std::uint64_t trailing_offset = 0;
    std::uint64_t trailing_size = 0;
    std::uint64_t file_size = 0;

    [[nodiscard]] std::uint32_t frame_bytes() const { return static_cast<std::uint32_t>(channels) * bytes_per_sample; }
    [[nodiscard]] std::uint32_t flac_bits() const { return 8u * bytes_per_sample; }
    // Standard foreign metadata application ID ("riff", "aiff", "w64 "); none for CAF.
    [[nodiscard]] std::optional<std::array<std::uint8_t, 4>> standard_application_id() const;
    [[nodiscard]] std::size_t audio_header_index() const;
    [[nodiscard]] std::uint64_t non_audio_bytes() const;
};

struct PcmScan
{
    bool recognized = false; // the file uses one of the supported PCM containers
    std::optional<PcmLayout> layout;
    std::optional<FallbackReason> reason;
    std::string detail;
};

// Largest foreign record that fits one FLAC metadata block (2^24 - 1 minus the 4-byte ID).
inline constexpr std::uint64_t max_foreign_block = (1u << 24) - 1 - 4;
inline constexpr std::uint64_t max_flac_frames = 1ull << 36;
inline constexpr std::uint32_t max_flac_sample_rate = 1048575;

// Reads only headers (seeks over audio). I/O errors are returned as errors; malformed or
// unsupported files come back as recognized with a fallback reason.
Result<PcmScan> scan_pcm(IChunkedStream& in);

// Container samples -> signed integers (interleaved).
void decode_samples(const PcmLayout& layout, std::span<const std::uint8_t> in, std::span<std::int32_t> out);
// One channel of interleaved container samples -> signed integers.
void decode_channel(const PcmLayout& layout, std::span<const std::uint8_t> in, std::uint16_t channel, std::span<std::int32_t> out);
// Signed integers -> container samples.
void encode_samples(const PcmLayout& layout, std::span<const std::int32_t> in, std::span<std::uint8_t> out);
// Container samples -> FLAC MD5 input (signed little-endian, bytes_per_sample wide). Returns
// `in` itself when the container already uses that form.
std::span<const std::uint8_t> md5_bytes(const PcmLayout& layout, std::span<const std::uint8_t> in, std::vector<std::uint8_t>& scratch);

}
