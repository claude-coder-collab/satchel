// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/stream.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zp
{

// Exposes an IChunkedStream to minizip-ng through its mz_stream vtable. Small writes are
// buffered; the first I/O error is kept so it can be reported with its message.
class MinizipStreamAdapter
{
public:
    explicit MinizipStreamAdapter(IChunkedStream& inner);
    MinizipStreamAdapter(const MinizipStreamAdapter&) = delete;
    MinizipStreamAdapter& operator=(const MinizipStreamAdapter&) = delete;
    MinizipStreamAdapter(MinizipStreamAdapter&&) = delete;
    MinizipStreamAdapter& operator=(MinizipStreamAdapter&&) = delete;
    ~MinizipStreamAdapter();

    // The mz_stream* handed to mz_zip_open.
    void* handle();
    VoidResult flush();
    // Overwrites bytes already written at an absolute offset (seekable output only), then
    // returns to the end of the output.
    VoidResult patch(std::uint64_t offset, std::span<const std::uint8_t> bytes);
    // Further writes are dropped (used to release minizip state after a failed build).
    void discard_writes();
    [[nodiscard]] const std::optional<Error>& error() const { return error_; }
    Error error_or(Status s, std::string message) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::optional<Error> error_;

    friend struct AdapterCallbacks;
};

// minizip-ng zip handle with RAII cleanup.
class MinizipHandle
{
public:
    MinizipHandle();
    MinizipHandle(const MinizipHandle&) = delete;
    MinizipHandle& operator=(const MinizipHandle&) = delete;
    MinizipHandle(MinizipHandle&&) = delete;
    MinizipHandle& operator=(MinizipHandle&&) = delete;
    ~MinizipHandle();

    void* get() { return handle_; }
    // Calls mz_zip_close (writes the central directory in write mode).
    std::int32_t close();

private:
    void* handle_ = nullptr;
    bool open_ = false;
    friend class ArchiveReader;
    friend class ZipWriter;
};

}
