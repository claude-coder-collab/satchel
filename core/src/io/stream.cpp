// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/stream.hpp"

#include <algorithm>
#include <cstring>
#include <span>

namespace zp
{

Result<std::size_t> IChunkedStream::read(std::uint8_t*, std::size_t)
{
    return fail(Status::InvalidArgument, "stream is not readable");
}

Result<std::size_t> IChunkedStream::write(const std::uint8_t*, std::size_t)
{
    return fail(Status::InvalidArgument, "stream is not writable");
}

VoidResult IChunkedStream::seek(std::uint64_t)
{
    return fail(Status::InvalidArgument, "stream is not seekable");
}

VoidResult IChunkedStream::reopen()
{
    return fail(Status::InvalidArgument, "stream cannot be reopened");
}

VoidResult IChunkedStream::write_all(std::span<const std::uint8_t> buf)
{
    while (!buf.empty())
    {
        auto n = write(buf.data(), buf.size());
        if (!n)
            return std::unexpected(n.error());
        if (*n == 0)
            return fail(Status::IoError, "short write");
        buf = buf.subspan(*n);
    }
    return {};
}

Result<std::size_t> IChunkedStream::read_full(std::span<std::uint8_t> buf)
{
    std::size_t total = 0;
    while (total < buf.size())
    {
        auto n = read(buf.data() + total, buf.size() - total);
        if (!n)
            return std::unexpected(n.error());
        if (*n == 0)
            break;
        total += *n;
    }
    return total;
}

Result<std::size_t> MemoryStream::read(std::uint8_t* buf, std::size_t len)
{
    if (pos_ >= data_.size())
        return 0;
    const auto n = std::min<std::uint64_t>(len, data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, static_cast<std::size_t>(n));
    pos_ += n;
    return static_cast<std::size_t>(n);
}

Result<std::size_t> MemoryStream::write(const std::uint8_t* buf, std::size_t len)
{
    const auto end = pos_ + len;
    if (end > data_.size())
        data_.resize(static_cast<std::size_t>(end));
    if (len > 0)
        std::memcpy(data_.data() + pos_, buf, len);
    pos_ = end;
    return len;
}

VoidResult MemoryStream::seek(std::uint64_t pos)
{
    pos_ = pos;
    return {};
}

Result<std::size_t> SharedBufferStream::read(std::uint8_t* buf, std::size_t len)
{
    if (pos_ >= data_->size())
        return 0;
    const auto n = std::min<std::uint64_t>(len, data_->size() - pos_);
    std::memcpy(buf, data_->data() + pos_, static_cast<std::size_t>(n));
    pos_ += n;
    return static_cast<std::size_t>(n);
}

VoidResult SharedBufferStream::seek(std::uint64_t pos)
{
    pos_ = pos;
    return {};
}

}
