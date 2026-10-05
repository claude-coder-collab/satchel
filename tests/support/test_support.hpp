// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "io/input_source.hpp"
#include "io/zip/builder.hpp"
#include "io/zip/extractor.hpp"
#include "io/zip/planner.hpp"
#include "io/zip/reader.hpp"
#include "pipeline/context.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zp::test
{

std::vector<std::uint8_t> random_bytes(std::size_t n, std::uint32_t seed);
std::vector<std::uint8_t> text_bytes(std::string_view s);
// Compressible pseudo-text.
std::vector<std::uint8_t> text_like(std::size_t n, std::uint32_t seed);

class TempDir
{
public:
    TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir();

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    std::filesystem::path write(const std::string& rel, const std::vector<std::uint8_t>& data) const;
    std::filesystem::path mkdir(const std::string& rel) const;

private:
    std::filesystem::path path_;
};

struct Built
{
    std::vector<std::uint8_t> zip;
    BuildResult result;
};

Context& shared_context(int threads = 4);

ArchivePlan plan_of(InputSource& input, PlannerOptions options = {});
Built build(const ArchivePlan& plan, int threads = 4, bool seekable = true, std::uint64_t memory_budget = 0);
Built build(InputSource& input, PlannerOptions options = {}, int threads = 4, bool seekable = true);

struct Extracted
{
    std::map<std::string, MemoryOutputSink::File> files;
    std::map<std::string, MemoryOutputSink::File> dirs;
    ExtractResult result;
    ExtractionPlan plan;
};

Extracted extract_all(const std::vector<std::uint8_t>& zip, ExtractOptions options = {});

std::vector<std::uint8_t> read_file(const std::filesystem::path& p);
// Path of an external tool on PATH, if installed.
std::optional<std::string> find_tool(const std::string& name);
int run(const std::string& command);
// stdout of a command; nullopt if it fails.
std::optional<std::string> run_capture(const std::string& command);

// Hand-made archives for reader tests (third-party quirks, hostile names).
struct RawEntry
{
    std::string name;
    std::vector<std::uint8_t> data; // stored as-is
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint16_t version_made_by = 0x031E;
    std::uint32_t external_attributes = 0;
    std::vector<std::uint8_t> extra;
    std::optional<std::uint32_t> crc; // default: CRC of data
    std::optional<std::uint64_t> uncompressed; // default: data size
};

std::vector<std::uint8_t> craft_zip(const std::vector<RawEntry>& entries, const std::string& comment = {});

class CancelAfter final : public ProgressSink
{
public:
    explicit CancelAfter(int calls) :
        left_(calls)
    {
    }
    bool on_progress(std::uint64_t, std::uint64_t) override { return --left_ > 0; }

private:
    int left_;
};

}
