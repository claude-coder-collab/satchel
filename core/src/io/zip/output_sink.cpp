// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/output_sink.hpp"

#include "io/file_system.hpp"

#include <format>

namespace zp
{

namespace
{

class FilesystemOutputFile final : public OutputFile
{
public:
    FilesystemOutputFile(std::unique_ptr<AtomicFileStream> stream, std::filesystem::path target) :
        stream_(std::move(stream)),
        target_(std::move(target))
    {
    }

    VoidResult write(std::span<const std::uint8_t> data) override { return stream_->write_all(data); }

    VoidResult commit(std::int64_t mtime, std::optional<std::uint32_t> unix_mode) override
    {
        if (auto r = stream_->commit(); !r)
            return r;
        if (unix_mode)
        {
            if (auto r = set_unix_mode(target_, *unix_mode); !r)
                return r;
        }
        return set_file_mtime(target_, mtime * 1'000'000'000);
    }

private:
    std::unique_ptr<AtomicFileStream> stream_;
    std::filesystem::path target_;
};

class NullOutputFile final : public OutputFile
{
public:
    VoidResult write(std::span<const std::uint8_t>) override { return {}; }
    VoidResult commit(std::int64_t, std::optional<std::uint32_t>) override { return {}; }
};

}

class MemoryOutputFile final : public OutputFile
{
public:
    MemoryOutputFile(MemoryOutputSink& sink, std::string path) :
        sink_(sink),
        path_(std::move(path))
    {
    }

    VoidResult write(std::span<const std::uint8_t> data) override
    {
        data_.insert(data_.end(), data.begin(), data.end());
        return {};
    }

    VoidResult commit(std::int64_t mtime, std::optional<std::uint32_t> unix_mode) override
    {
        std::lock_guard lock(sink_.mutex_);
        sink_.files_[path_] = { std::move(data_), mtime, unix_mode };
        return {};
    }

private:
    MemoryOutputSink& sink_;
    std::string path_;
    std::vector<std::uint8_t> data_;
};

std::filesystem::path FilesystemOutputSink::full(const std::string& path) const
{
    return root_ / path_from_utf8(path);
}

VoidResult FilesystemOutputSink::check_no_links(const std::string& path, bool include_leaf)
{
    std::string prefix;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const auto pos = path.find('/', start);
        const bool leaf = pos == std::string::npos;
        if (leaf && !include_leaf)
            break;
        prefix = path.substr(0, leaf ? path.size() : pos);
        const auto st = stat_no_follow(full(prefix));
        if (st && st->kind == ItemKind::Symlink)
            return fail(Status::UnsafePath, std::format("'{}' would be written through the existing link '{}'", path, prefix));
        if (leaf)
            break;
        start = pos + 1;
    }
    return {};
}

bool FilesystemOutputSink::exists(const std::string& path)
{
    return static_cast<bool>(stat_no_follow(full(path)));
}

VoidResult FilesystemOutputSink::make_directory(const std::string& path)
{
    std::lock_guard lock(mutex_);
    if (auto r = check_no_links(path, true); !r)
        return r;
    std::error_code ec;
    std::filesystem::create_directories(full(path), ec);
    if (ec)
        return fail(Status::IoError, std::format("cannot create folder '{}': {}", path, ec.message()));
    return {};
}

VoidResult FilesystemOutputSink::set_directory_attributes(const std::string& path, std::int64_t mtime, std::optional<std::uint32_t> unix_mode)
{
    const auto p = full(path);
    if (unix_mode)
    {
        if (auto r = set_unix_mode(p, *unix_mode | 0700u); !r)
            return r;
    }
    return set_file_mtime(p, mtime * 1'000'000'000);
}

Result<std::unique_ptr<OutputFile>> FilesystemOutputSink::create_file(const std::string& path, bool replace)
{
    const auto target = full(path);
    {
        std::lock_guard lock(mutex_);
        if (auto r = check_no_links(path, true); !r)
            return std::unexpected(r.error());
        if (!replace && stat_no_follow(target))
            return fail(Status::IoError, std::format("'{}' already exists", path));
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec)
            return fail(Status::IoError, std::format("cannot create folder for '{}': {}", path, ec.message()));
    }
    auto stream = AtomicFileStream::create(target, false);
    if (!stream)
        return std::unexpected(stream.error());
    return std::make_unique<FilesystemOutputFile>(std::move(*stream), target);
}

bool MemoryOutputSink::exists(const std::string& path)
{
    std::lock_guard lock(mutex_);
    return files_.contains(path) || dirs_.contains(path);
}

VoidResult MemoryOutputSink::make_directory(const std::string& path)
{
    std::lock_guard lock(mutex_);
    dirs_.try_emplace(path);
    return {};
}

VoidResult MemoryOutputSink::set_directory_attributes(const std::string& path, std::int64_t mtime, std::optional<std::uint32_t> unix_mode)
{
    std::lock_guard lock(mutex_);
    auto& d = dirs_[path];
    d.mtime = mtime;
    d.unix_mode = unix_mode;
    return {};
}

Result<std::unique_ptr<OutputFile>> MemoryOutputSink::create_file(const std::string& path, bool replace)
{
    {
        std::lock_guard lock(mutex_);
        if (!replace && files_.contains(path))
            return fail(Status::IoError, std::format("'{}' already exists", path));
    }
    return std::make_unique<MemoryOutputFile>(*this, path);
}

std::map<std::string, MemoryOutputSink::File> MemoryOutputSink::files() const
{
    std::lock_guard lock(mutex_);
    return files_;
}

std::map<std::string, MemoryOutputSink::File> MemoryOutputSink::directories() const
{
    std::lock_guard lock(mutex_);
    return dirs_;
}

Result<std::unique_ptr<OutputFile>> NullOutputSink::create_file(const std::string&, bool)
{
    return std::make_unique<NullOutputFile>();
}

}
