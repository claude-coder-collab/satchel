// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/input_source.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace zp
{

Result<std::unique_ptr<IChunkedStream>> InputItem::open() const
{
    if (!opener)
        return fail(Status::InvalidArgument, std::format("'{}' cannot be opened", source_path));
    return opener();
}

Result<Snapshot> InputItem::current() const
{
    if (!restat)
        return snapshot();
    return restat();
}

std::int64_t InputItem::mtime_seconds() const
{
    auto s = mtime_ns / 1'000'000'000;
    if (mtime_ns % 1'000'000'000 < 0)
        --s;
    return s;
}

namespace
{

InputItem make_fs_item(const std::filesystem::path& p, std::string archive_path, const FileStat& st)
{
    InputItem item;
    item.source_path = path_to_utf8(p);
    item.archive_path = std::move(archive_path);
    item.kind = st.kind;
    item.size = st.size;
    item.mtime_ns = st.mtime_ns;
    item.unix_mode = st.unix_mode;
    if (st.kind == ItemKind::File)
    {
        item.opener = [p]() -> Result<std::unique_ptr<IChunkedStream>> {
            auto f = FileStream::open(p, FileMode::Read);
            if (!f)
                return std::unexpected(f.error());
            return std::unique_ptr<IChunkedStream>(std::move(*f));
        };
    }
    item.restat = [p]() -> Result<Snapshot> {
        auto s = stat_no_follow(p);
        if (!s)
            return std::unexpected(s.error());
        return Snapshot{ s->size, s->mtime_ns };
    };
    return item;
}

VoidResult walk(const std::filesystem::path& dir, const std::string& prefix, std::vector<InputItem>& out)
{
    std::error_code ec;
    std::vector<std::pair<std::string, std::filesystem::path>> children;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::none, ec), end; !ec && it != end; it.increment(ec))
        children.emplace_back(path_to_utf8(it->path().filename()), it->path());
    if (ec)
        return fail(Status::IoError, std::format("cannot list '{}': {}", path_to_utf8(dir), ec.message()));
    std::ranges::sort(children, {}, &std::pair<std::string, std::filesystem::path>::first);

    for (const auto& [name, child] : children)
    {
        auto st = stat_no_follow(child);
        if (!st)
            return std::unexpected(st.error());
        if (st->kind == ItemKind::Other)
            continue;
        auto archive_path = prefix;
        archive_path += '/';
        archive_path += name;
        out.push_back(make_fs_item(child, archive_path, *st));
        if (st->kind == ItemKind::Directory)
        {
            if (auto r = walk(child, archive_path, out); !r)
                return r;
        }
    }
    return {};
}

}

Result<std::vector<InputItem>> FilesystemInputSource::enumerate()
{
    std::vector<InputItem> items;
    for (const auto& raw_root : roots_)
    {
        const auto root = raw_root.lexically_normal();
        auto name_path = root.filename();
        if (name_path.empty())
            name_path = root.parent_path().filename();
        const auto name = path_to_utf8(name_path);
        if (name.empty() || name == "." || name == "..")
            return fail(Status::InvalidArgument, std::format("cannot archive '{}' without a name", path_to_utf8(raw_root)));
        auto st = stat_no_follow(root);
        if (!st)
            return std::unexpected(st.error());
        if (st->kind == ItemKind::Other)
            return fail(Status::InvalidArgument, std::format("'{}' is not a regular file or directory", path_to_utf8(root)));
        items.push_back(make_fs_item(root, name, *st));
        if (st->kind == ItemKind::Directory)
        {
            if (auto r = walk(root, name, items); !r)
                return std::unexpected(r.error());
        }
    }
    return items;
}

void MemoryInputSource::add_file(std::string archive_path, std::vector<std::uint8_t> data, std::int64_t mtime_ns, std::uint32_t mode)
{
    auto buf = std::make_shared<std::shared_ptr<const std::vector<std::uint8_t>>>(std::make_shared<const std::vector<std::uint8_t>>(std::move(data)));
    entries_.push_back({ std::move(archive_path), ItemKind::File, std::move(buf), std::make_shared<std::int64_t>(mtime_ns), mode });
}

void MemoryInputSource::add_directory(std::string archive_path, std::int64_t mtime_ns, std::uint32_t mode)
{
    entries_.push_back({ std::move(archive_path), ItemKind::Directory, nullptr, std::make_shared<std::int64_t>(mtime_ns), mode });
}

void MemoryInputSource::add_symlink(std::string archive_path)
{
    entries_.push_back({ std::move(archive_path), ItemKind::Symlink, nullptr, std::make_shared<std::int64_t>(0), 0777 });
}

void MemoryInputSource::replace_data(const std::string& archive_path, std::vector<std::uint8_t> data, std::int64_t mtime_ns)
{
    for (auto& e : entries_)
    {
        if (e.archive_path == archive_path && e.data)
        {
            *e.data = std::make_shared<const std::vector<std::uint8_t>>(std::move(data));
            *e.mtime_ns = mtime_ns;
            return;
        }
    }
}

Result<std::vector<InputItem>> MemoryInputSource::enumerate()
{
    std::vector<InputItem> items;
    items.reserve(entries_.size());
    for (const auto& e : entries_)
    {
        InputItem item;
        item.source_path = "memory:" + e.archive_path;
        item.archive_path = e.archive_path;
        item.kind = e.kind;
        item.mtime_ns = *e.mtime_ns;
        item.unix_mode = e.mode;
        if (e.data)
        {
            item.size = (*e.data)->size();
            auto data = e.data;
            item.opener = [data]() -> Result<std::unique_ptr<IChunkedStream>> {
                return std::make_unique<SharedBufferStream>(*data);
            };
            auto mtime = e.mtime_ns;
            item.restat = [data, mtime]() -> Result<Snapshot> {
                return Snapshot{ (*data)->size(), *mtime };
            };
        }
        items.push_back(std::move(item));
    }
    return items;
}

}
