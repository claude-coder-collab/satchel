// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "logic.hpp"
#include "zp_cpp.hpp"

#include <QElapsedTimer>
#include <QString>

#include <cstdint>
#include <string>

// Translatable display text for values the Qt-free logic library describes in English.
namespace labels
{

// "Store", "Deflate", "FLAC", "FLAC multi-mono", "Unsupported", "Symlink", "Other", "Folder";
// other text is shown as is.
QString method(const std::string& key);
// A row's method column ("FLAC channel 3/16" for multi-mono members).
QString row_method(const satchel_gui::Row& row);
// A row's name ("take.wav — 16 channels" for multi-mono groups).
QString row_name(const satchel_gui::Row& row);
// "NAME — N channels".
QString group_label(const QString& name, int channels);
// "4.2 GB → 2.3 GB, 45% smaller".
QString savings(std::uint64_t input, std::uint64_t output, bool ratio_known = true);
// Planned method of an entry about to be written: "FLAC", "Deflate", "Store", "Readme", "Folder".
// `stored`: sampling predicts the build will store this general entry.
QString plan_method(const zpp::PlanEntry& e, bool stored = false);
// Speed ("12.3 MB/s") and time left ("about 1:05 left", minutes:seconds) for `done` bytes in
// `seconds` with `left` to go; both empty until there is half a second of progress to judge by.
struct Rate
{
    QString speed;
    QString time;
};
Rate rate(std::uint64_t done, std::uint64_t left, double seconds);

}

// Speed and time left of a running job, from its progress reports. Timing starts at the first
// report after reset() and again whenever progress goes back (a job's next pass, e.g. verifying).
class ProgressRate
{
public:
    void reset();
    [[nodiscard]] labels::Rate rate(std::uint64_t done, std::uint64_t total);

private:
    QElapsedTimer timer_;
    std::uint64_t first_ = 0;
    std::uint64_t last_ = 0;
};
