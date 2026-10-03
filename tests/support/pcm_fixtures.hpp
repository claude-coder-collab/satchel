// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zp::test
{

// Deterministic, compressible test audio (sine mix + noise), as interleaved container bytes.
std::vector<std::uint8_t> audio_bytes(std::uint64_t frames, std::uint16_t channels, std::uint16_t bytes_per_sample, bool big_endian, bool unsigned_8bit, std::uint32_t seed);

struct ExtraChunk
{
    std::string id; // 4 characters (RIFF/AIFF/CAF)
    std::vector<std::uint8_t> body;
    bool after_audio = false;
};

struct WavSpec
{
    std::uint16_t channels = 2;
    std::uint32_t rate = 48000;
    std::uint16_t bits = 16;
    std::uint16_t container_bits = 0; // 0: same as bits
    std::uint64_t frames = 10000;
    bool extensible = false;
    std::uint16_t format_tag = 1; // 1 PCM, 3 float, other: unsupported
    std::vector<ExtraChunk> chunks;
    std::vector<std::uint8_t> trailing;
    bool placeholder_sizes = false; // RIFF and data sizes 0xFFFFFFFF
    std::uint32_t seed = 1;
};

std::vector<std::uint8_t> make_wav(const WavSpec& spec);
std::vector<std::uint8_t> make_rf64(const WavSpec& spec);
std::vector<std::uint8_t> make_w64(const WavSpec& spec);

struct AiffSpec
{
    std::uint16_t channels = 2;
    std::uint32_t rate = 48000;
    std::uint16_t bits = 16;
    std::uint64_t frames = 10000;
    bool aifc = false;
    std::string compression = "NONE"; // AIFF-C: NONE, sowt, twos, fl32, ...
    std::vector<ExtraChunk> chunks;
    std::uint32_t seed = 1;
};

std::vector<std::uint8_t> make_aiff(const AiffSpec& spec);

struct CafSpec
{
    std::uint16_t channels = 2;
    double rate = 48000;
    std::uint16_t bits = 16;
    std::uint64_t frames = 10000;
    bool little_endian = false;
    bool floating = false;
    bool unknown_data_size = false; // data chunk size -1
    std::uint32_t seed = 1;
};

std::vector<std::uint8_t> make_caf(const CafSpec& spec);

std::vector<std::uint8_t> bext_chunk(const std::string& description, const std::string& originator, std::uint64_t time_reference);
std::vector<std::uint8_t> ixml_chunk(const std::string& project, const std::string& scene, const std::string& take, const std::vector<std::pair<int, std::string>>& tracks);

}
