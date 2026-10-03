// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zp
{

class OutputFile
{
public:
    OutputFile() = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    OutputFile(OutputFile&&) = delete;
    OutputFile& operator=(OutputFile&&) = delete;
    // Destroying an uncommitted file discards it.
    virtual ~OutputFile() = default;

    virtual VoidResult write(std::span<const std::uint8_t> data) = 0;
    virtual VoidResult commit(std::int64_t mtime, std::optional<std::uint32_t> unix_mode) = 0;
};

// Destination of an extraction. Paths are relative, '/'-separated and already sanitized.
class OutputSink
{
public:
    OutputSink() = default;
    OutputSink(const OutputSink&) = delete;
    OutputSink& operator=(const OutputSink&) = delete;
    OutputSink(OutputSink&&) = delete;
    OutputSink& operator=(OutputSink&&) = delete;
    virtual ~OutputSink() = default;

    virtual bool exists(const std::string& path) = 0;
    virtual VoidResult make_directory(const std::string& path) = 0;
    virtual VoidResult set_directory_attributes(const std::string& path, std::int64_t mtime, std::optional<std::uint32_t> unix_mode) = 0;
    // replace = false fails if the path exists.
    virtual Result<std::unique_ptr<OutputFile>> create_file(const std::string& path, bool replace) = 0;
};

// Native filesystem. Files are written to a temporary name and renamed into place on commit,
// so a failed or replaced extraction never leaves a partial file. Refuses to write through a
// symbolic link that already exists under the destination.
class FilesystemOutputSink final : public OutputSink
{
public:
    explicit FilesystemOutputSink(std::filesystem::path root) :
        root_(std::move(root))
    {
    }

    bool exists(const std::string& path) override;
    VoidResult make_directory(const std::string& path) override;
    VoidResult set_directory_attributes(const std::string& path, std::int64_t mtime, std::optional<std::uint32_t> unix_mode) override;
    Result<std::unique_ptr<OutputFile>> create_file(const std::string& path, bool replace) override;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    VoidResult check_no_links(const std::string& path, bool include_leaf);
    std::filesystem::path full(const std::string& path) const;

    std::filesystem::path root_;
    std::mutex mutex_;
};

// Keeps extracted files in memory (tests, verification).
class MemoryOutputSink final : public OutputSink
{
public:
    struct File
    {
        std::vector<std::uint8_t> data;
        std::int64_t mtime = 0;
        std::optional<std::uint32_t> unix_mode;
    };

    bool exists(const std::string& path) override;
    VoidResult make_directory(const std::string& path) override;
    VoidResult set_directory_attributes(const std::string& path, std::int64_t mtime, std::optional<std::uint32_t> unix_mode) override;
    Result<std::unique_ptr<OutputFile>> create_file(const std::string& path, bool replace) override;

    // Pre-existing destination content for tests.
    void add_existing(const std::string& path) { files_[path] = {}; }

    std::map<std::string, File> files() const;
    std::map<std::string, File> directories() const;

private:
    friend class MemoryOutputFile;
    mutable std::mutex mutex_;
    std::map<std::string, File> files_;
    std::map<std::string, File> dirs_;
};

// Discards data; used to verify archives without writing.
class NullOutputSink final : public OutputSink
{
public:
    bool exists(const std::string&) override { return false; }
    VoidResult make_directory(const std::string&) override { return {}; }
    VoidResult set_directory_attributes(const std::string&, std::int64_t, std::optional<std::uint32_t>) override { return {}; }
    Result<std::unique_ptr<OutputFile>> create_file(const std::string& path, bool replace) override;
};

}
