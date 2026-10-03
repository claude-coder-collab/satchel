// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/stream.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace zp
{

enum class ItemKind : std::uint8_t {
    File,
    Directory,
    Symlink,
    Other,
};

struct FileStat
{
    ItemKind kind = ItemKind::Other;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint32_t unix_mode = 0;
};

std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& p);

// Never follows links. Windows junctions and other reparse points report ItemKind::Symlink.
Result<FileStat> stat_no_follow(const std::filesystem::path& p);

VoidResult set_file_mtime(const std::filesystem::path& p, std::int64_t mtime_ns);
VoidResult set_unix_mode(const std::filesystem::path& p, std::uint32_t mode);

enum class FileMode : std::uint8_t {
    Read,
    CreateTruncate,
    CreateNew,
};

class FileStream final : public IChunkedStream
{
public:
    static Result<std::unique_ptr<FileStream>> open(const std::filesystem::path& p, FileMode mode);
    ~FileStream() override;

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override;
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override;
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override;
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override;
    VoidResult reopen() override { return seek(0); }
    VoidResult flush() override;

    VoidResult sync();
    void close();
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    FileStream() = default;

    std::filesystem::path path_;
    std::uint64_t pos_ = 0;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// Writes to a temporary file next to the target; commit() syncs and atomically replaces the
// target. Destroying an uncommitted stream removes the temporary file.
class AtomicFileStream final : public IChunkedStream
{
public:
    static Result<std::unique_ptr<AtomicFileStream>> create(const std::filesystem::path& target, bool sync_on_commit = true);
    ~AtomicFileStream() override;

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override { return file_->read(buf, len); }
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override { return file_->write(buf, len); }
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override { return file_->seek(pos); }
    [[nodiscard]] std::uint64_t tell() const override { return file_->tell(); }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return file_->size(); }
    VoidResult flush() override { return file_->flush(); }

    VoidResult commit();
    void discard();
    [[nodiscard]] const std::filesystem::path& temp_path() const { return temp_; }

private:
    AtomicFileStream() = default;

    std::unique_ptr<FileStream> file_;
    std::filesystem::path target_;
    std::filesystem::path temp_;
    bool sync_ = true;
    bool done_ = false;
};

VoidResult atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);
VoidResult sync_directory_of(const std::filesystem::path& p);

}
