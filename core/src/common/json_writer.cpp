// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/json_writer.hpp"

#include <format>

namespace zp
{

void JsonWriter::separator()
{
    if (after_key_)
    {
        after_key_ = false;
        return;
    }
    if (!first_.empty())
    {
        if (!first_.back())
            out_ += ',';
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::begin_object()
{
    separator();
    out_ += '{';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object()
{
    out_ += '}';
    first_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::begin_array()
{
    separator();
    out_ += '[';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array()
{
    out_ += ']';
    first_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view k)
{
    value(k);
    out_ += ':';
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view v)
{
    separator();
    out_ += '"';
    for (const char c : v)
    {
        switch (c)
        {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out_ += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                else
                    out_ += c;
        }
    }
    out_ += '"';
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t v)
{
    separator();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t v)
{
    separator();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(bool v)
{
    separator();
    out_ += v ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null()
{
    separator();
    out_ += "null";
    return *this;
}

}
