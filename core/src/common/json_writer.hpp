// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zp
{

// Minimal streaming JSON writer (objects, arrays, strings, numbers, booleans, null).
class JsonWriter
{
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view k);
    JsonWriter& value(std::string_view v);
    JsonWriter& value(const char* v) { return value(std::string_view(v)); }
    JsonWriter& value(const std::string& v) { return value(std::string_view(v)); }
    JsonWriter& value(std::int64_t v);
    JsonWriter& value(std::uint64_t v);
    JsonWriter& value(int v) { return value(static_cast<std::int64_t>(v)); }
    JsonWriter& value(unsigned v) { return value(static_cast<std::uint64_t>(v)); }
    JsonWriter& value(bool v);
    JsonWriter& null();

    template <typename T>
    JsonWriter& field(std::string_view k, const T& v)
    {
        key(k);
        return value(v);
    }

    [[nodiscard]] const std::string& str() const { return out_; }

private:
    void separator();

    std::string out_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

}
