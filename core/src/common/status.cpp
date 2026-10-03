// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/status.hpp"

namespace zp
{

std::string_view status_name(Status s) noexcept
{
    switch (s)
    {
        case Status::Ok:
            return "OK";
        case Status::IoError:
            return "IO_ERROR";
        case Status::SourceChanged:
            return "SOURCE_CHANGED";
        case Status::ConflictsUnresolved:
            return "CONFLICTS_UNRESOLVED";
        case Status::UnsafePath:
            return "UNSAFE_PATH";
        case Status::UnsupportedMethod:
            return "UNSUPPORTED_METHOD";
        case Status::CrcMismatch:
            return "CRC_MISMATCH";
        case Status::HashMismatch:
            return "HASH_MISMATCH";
        case Status::CorruptArchive:
            return "CORRUPT_ARCHIVE";
        case Status::Cancelled:
            return "CANCELLED";
        case Status::InvalidArgument:
            return "INVALID_ARGUMENT";
        case Status::InvalidName:
            return "INVALID_NAME";
        case Status::NameCollision:
            return "NAME_COLLISION";
        case Status::DecisionRequired:
            return "DECISION_REQUIRED";
        case Status::IncompleteGroup:
            return "INCOMPLETE_GROUP";
        case Status::Internal:
            return "INTERNAL";
    }
    return "UNKNOWN";
}

}
