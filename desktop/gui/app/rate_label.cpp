// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#include "rate_label.hpp"

#include <QHBoxLayout>
#include <QLabel>

RateLabel::RateLabel(QWidget* parent) :
    QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    speed_ = new QLabel(this);
    separator_ = new QLabel(this);
    time_ = new QLabel(this);
    speed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    time_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const auto widest = labels::rate((std::uint64_t{ 888 } << 20) + (std::uint64_t{ 1 } << 19), std::uint64_t{ 888 } << 40, 1.0);
    const auto metrics = fontMetrics();
    speed_->setFixedWidth(metrics.horizontalAdvance(widest.speed) + 2);
    time_->setFixedWidth(metrics.horizontalAdvance(widest.time) + 2);
    separator_->setFixedWidth(metrics.horizontalAdvance(QStringLiteral(" · ")));
    separator_->setAlignment(Qt::AlignCenter);
    layout->addWidget(speed_);
    layout->addWidget(separator_);
    layout->addWidget(time_);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void RateLabel::reset()
{
    rate_.reset();
    speed_->clear();
    separator_->clear();
    time_->clear();
}

void RateLabel::report(std::uint64_t done, std::uint64_t total)
{
    const auto rate = rate_.rate(done, total);
    if (rate.speed.isEmpty())
        return;
    speed_->setText(rate.speed);
    separator_->setText(QStringLiteral("·"));
    time_->setText(rate.time);
}

QString RateLabel::text() const
{
    return speed_->text().isEmpty() ? QString() : QStringLiteral("%1 · %2").arg(speed_->text(), time_->text());
}
