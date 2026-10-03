// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/file_system.hpp"
#include "io/stream.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace zp
{

struct Snapshot
{
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;

    bool operator==(const Snapshot&) const = default;
};

struct InputItem
{
    std::string source_path;
    std::string archive_path;
    ItemKind kind = ItemKind::File;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint32_t unix_mode = 0644;

    std::function<Result<std::unique_ptr<IChunkedStream>>()> opener;
    std::function<Result<Snapshot>()> restat;

    [[nodiscard]] Result<std::unique_ptr<IChunkedStream>> open() const;
    [[nodiscard]] Result<Snapshot> current() const;
    [[nodiscard]] Snapshot snapshot() const { return { size, mtime_ns }; }
    [[nodiscard]] std::int64_t mtime_seconds() const;
};

class InputSource
{
public:
    InputSource() = default;
    InputSource(const InputSource&) = delete;
    InputSource& operator=(const InputSource&) = delete;
    InputSource(InputSource&&) = delete;
    InputSource& operator=(InputSource&&) = delete;
    virtual ~InputSource() = default;

    virtual Result<std::vector<InputItem>> enumerate() = 0;
};

// Each root becomes a top-level entry named after its last path component; directories are
// walked recursively in byte order of their UTF-8 names. Links are never followed.
class FilesystemInputSource final : public InputSource
{
public:
    explicit FilesystemInputSource(std::vector<std::filesystem::path> roots) :
        roots_(std::move(roots))
    {
    }

    Result<std::vector<InputItem>> enumerate() override;

private:
    std::vector<std::filesystem::path> roots_;
};

// In-memory items, used by tests and bindings that hand over buffers.
class MemoryInputSource final : public InputSource
{
public:
    void add_file(std::string archive_path, std::vector<std::uint8_t> data, std::int64_t mtime_ns = 0, std::uint32_t mode = 0644);
    void add_directory(std::string archive_path, std::int64_t mtime_ns = 0, std::uint32_t mode = 0755);
    void add_symlink(std::string archive_path);
    // Changes a file's content after planning (to provoke SOURCE_CHANGED in tests).
    void replace_data(const std::string& archive_path, std::vector<std::uint8_t> data, std::int64_t mtime_ns);

    Result<std::vector<InputItem>> enumerate() override;

private:
    struct Entry
    {
        std::string archive_path;
        ItemKind kind;
        std::shared_ptr<std::shared_ptr<const std::vector<std::uint8_t>>> data;
        std::shared_ptr<std::int64_t> mtime_ns;
        std::uint32_t mode;
    };
    std::vector<Entry> entries_;
};

}
