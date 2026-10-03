// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/input_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zp
{

using Uuid = std::array<std::uint8_t, 16>;

struct PcmLayout;

inline constexpr std::string_view readme_file_name = "README - How to restore original audio.txt";

enum class PlanCodec : std::uint8_t {
    General, // deflate, or store when the sample saves too little
    Flac, // PCM with <= 8 channels -> one FLAC file
    FlacMono, // one member of a > 8 channel group
    Generated, // the readme
    Kept, // editor: existing entry copied raw
};

enum class FallbackReason : std::uint8_t {
    NotParsable,
    FloatSamples,
    UnsupportedEncoding,
    BitDepth,
    SampleRate,
    TooManySamples,
    ChunkTooLarge,
    NotSeekable,
};

struct PlannerOptions
{
    bool flac_enabled = true;
    int deflate_level = 6;
    int flac_level = 5;
};

struct PlanEntry
{
    InputItem item;
    std::string output_name;
    PlanCodec codec = PlanCodec::General;
    std::optional<Uuid> group_id;
    std::optional<std::uint16_t> channel_index;
    std::optional<FallbackReason> flac_fallback_reason;
    std::string flac_fallback_detail;
    Snapshot snapshot;
    // FLAC entries: header scan of the source, and the extension restored on extraction.
    std::shared_ptr<const PcmLayout> pcm;
    std::uint16_t channel_count = 0;
    std::string original_extension;
    // Editor: a kept entry that is a restorable FLAC file made by this library.
    bool kept_restorable = false;
    std::string kept_restored_name;
    // Editor: index of the source entry in the archive being edited (codec Kept).
    std::optional<std::size_t> kept_index;

    [[nodiscard]] bool is_directory() const { return item.kind == ItemKind::Directory; }
    [[nodiscard]] bool is_flac() const { return codec == PlanCodec::Flac || codec == PlanCodec::FlacMono; }
    // Converted audio: new FLAC entries and restorable FLAC entries kept by the editor.
    [[nodiscard]] bool is_converted() const { return is_flac() || kept_restorable; }
    // Archive path of the file restored from this FLAC entry (or group).
    [[nodiscard]] std::string restored_name() const;
};

enum class ConflictKind : std::uint8_t {
    Collision, // names equal after NFC + case folding, or a file used as a folder
    InvalidName, // name cannot be stored (not UTF-8, absolute, '..', ...)
};

struct Conflict
{
    ConflictKind kind = ConflictKind::Collision;
    std::string collision_key;
    std::vector<std::size_t> entries;
    std::string detail;
};

enum class WarningKind : std::uint8_t {
    SymlinkSkipped,
    FlacFallback,
};

struct Warning
{
    WarningKind kind = WarningKind::SymlinkSkipped;
    std::string source_path;
    std::string detail;
};

enum class ResolutionAction : std::uint8_t {
    Rename,
    Skip,
    DisableFlac,
};

struct Resolution
{
    std::size_t entry = 0;
    ResolutionAction action = ResolutionAction::Skip;
    std::string new_name;
};

struct ArchivePlan
{
    std::vector<PlanEntry> entries;
    std::vector<Conflict> conflicts;
    std::vector<Warning> warnings;
    PlannerOptions options;

    [[nodiscard]] bool executable() const { return conflicts.empty(); }
    [[nodiscard]] std::uint64_t total_input_bytes() const;
};

std::string_view fallback_reason_text(FallbackReason r);

// "dir/take1.flac" or "dir/take1_ch03.flac" -> "dir/take1"
std::string flac_stem(std::string_view output_name, PlanCodec codec);
std::string mono_member_name(std::string_view stem, std::uint16_t channel, std::uint16_t channels);

}
