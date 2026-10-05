// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "labels.hpp"

#include <QCoreApplication>

#include <array>
#include <cstdlib>

namespace labels
{

namespace
{

constexpr std::array<const char*, 8> method_keys{
    QT_TRANSLATE_NOOP("Method", "Store"),
    QT_TRANSLATE_NOOP("Method", "Deflate"),
    QT_TRANSLATE_NOOP("Method", "FLAC"),
    QT_TRANSLATE_NOOP("Method", "FLAC multi-mono"),
    QT_TRANSLATE_NOOP("Method", "Unsupported"),
    QT_TRANSLATE_NOOP("Method", "Symlink"),
    QT_TRANSLATE_NOOP("Method", "Other"),
    QT_TRANSLATE_NOOP("Method", "Folder"),
};

QString size_text(std::uint64_t bytes)
{
    return QString::fromStdString(satchel_gui::human_size(bytes));
}

}

QString method(const std::string& key)
{
    for (const auto* k : method_keys)
    {
        if (key == k)
            return QCoreApplication::translate("Method", k);
    }
    return QString::fromStdString(key);
}

QString row_method(const satchel_gui::Row& row)
{
    if (row.channel_index > 0)
        return QCoreApplication::translate("Method", "FLAC channel %1/%2").arg(row.channel_index).arg(row.channel_count);
    return method(row.method);
}

QString group_label(const QString& name, int channels)
{
    return QCoreApplication::translate("Archive", "%1 — %2 channels").arg(name).arg(channels);
}

QString row_name(const satchel_gui::Row& row)
{
    if (row.channel_count > 0 && row.channel_index == 0)
        return group_label(QString::fromStdString(row.group_name), row.channel_count);
    return QString::fromStdString(row.name);
}

QString savings(std::uint64_t input, std::uint64_t output)
{
    const auto saved = satchel_gui::percent_saved(input, output);
    return saved >= 0 ? QCoreApplication::translate("Archive", "%1 → %2, %3% smaller").arg(size_text(input), size_text(output)).arg(saved)
                      : QCoreApplication::translate("Archive", "%1 → %2, %3% larger").arg(size_text(input), size_text(output)).arg(std::abs(saved));
}

}
