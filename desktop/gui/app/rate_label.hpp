// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "labels.hpp"

#include <QWidget>

class QLabel;

// Speed and time left of a running job, "12.3 MB/s · about 1:05 left", in two columns of fixed
// width so that neither moves as the other's digits change.
class RateLabel : public QWidget
{
public:
    explicit RateLabel(QWidget* parent = nullptr);
    // Empties the label and starts timing afresh at the next report.
    void reset();
    void report(std::uint64_t done, std::uint64_t total);
    [[nodiscard]] QString text() const;

private:
    ProgressRate rate_;
    QLabel* speed_ = nullptr;
    QLabel* separator_ = nullptr;
    QLabel* time_ = nullptr;
};
