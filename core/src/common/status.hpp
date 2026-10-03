// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace zp
{

enum class Status : std::int32_t {
    Ok = 0,
    IoError = 1,
    SourceChanged = 2,
    ConflictsUnresolved = 3,
    UnsafePath = 4,
    UnsupportedMethod = 5,
    CrcMismatch = 6,
    HashMismatch = 7,
    CorruptArchive = 8,
    Cancelled = 9,
    InvalidArgument = 10,
    InvalidName = 11,
    NameCollision = 12,
    DecisionRequired = 13,
    IncompleteGroup = 14,
    Internal = 15,
};

std::string_view status_name(Status s) noexcept;

struct Error
{
    Status status = Status::Internal;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

inline std::unexpected<Error> fail(Status s, std::string message = {})
{
    return std::unexpected(Error{ s, std::move(message) });
}

}
