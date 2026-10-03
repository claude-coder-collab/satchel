// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace zp
{

class IChunkedStream
{
public:
    IChunkedStream() = default;
    IChunkedStream(const IChunkedStream&) = delete;
    IChunkedStream& operator=(const IChunkedStream&) = delete;
    IChunkedStream(IChunkedStream&&) = delete;
    IChunkedStream& operator=(IChunkedStream&&) = delete;
    virtual ~IChunkedStream() = default;

    // Returns fewer bytes than requested only at end of stream.
    virtual Result<std::size_t> read(std::uint8_t* buf, std::size_t len);
    // Writes everything or fails.
    virtual Result<std::size_t> write(const std::uint8_t* buf, std::size_t len);
    [[nodiscard]] virtual bool seekable() const { return false; }
    virtual VoidResult seek(std::uint64_t pos);
    [[nodiscard]] virtual std::uint64_t tell() const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> size() const { return std::nullopt; }
    virtual VoidResult reopen();
    virtual VoidResult flush() { return {}; }

    Result<std::size_t> read(std::span<std::uint8_t> buf) { return read(buf.data(), buf.size()); }
    VoidResult write_all(std::span<const std::uint8_t> buf);
    Result<std::size_t> read_full(std::span<std::uint8_t> buf);
};

class MemoryStream final : public IChunkedStream
{
public:
    MemoryStream() = default;
    explicit MemoryStream(std::vector<std::uint8_t> data) :
        data_(std::move(data))
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override;
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override;
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override;
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return data_.size(); }
    VoidResult reopen() override
    {
        pos_ = 0;
        return {};
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const { return data_; }
    std::vector<std::uint8_t>& data() { return data_; }

private:
    std::vector<std::uint8_t> data_;
    std::uint64_t pos_ = 0;
};

// Shares a read-only buffer; each instance has its own position.
class SharedBufferStream final : public IChunkedStream
{
public:
    explicit SharedBufferStream(std::shared_ptr<const std::vector<std::uint8_t>> data) :
        data_(std::move(data))
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override;
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override;
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return data_->size(); }
    VoidResult reopen() override
    {
        pos_ = 0;
        return {};
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> data_;
    std::uint64_t pos_ = 0;
};

// Hides seeking and size of the wrapped stream (non-seekable output such as stdout or a socket).
class NonSeekableStream final : public IChunkedStream
{
public:
    explicit NonSeekableStream(IChunkedStream& inner) :
        inner_(inner)
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override { return inner_.read(buf, len); }
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override { return inner_.write(buf, len); }
    [[nodiscard]] std::uint64_t tell() const override { return inner_.tell(); }
    VoidResult flush() override { return inner_.flush(); }

private:
    IChunkedStream& inner_;
};

}
