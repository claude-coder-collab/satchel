// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include <QList>
#include <QPixmap>
#include <QString>

#include <functional>

class QWidget;

struct PromisedItem
{
    QString name;
    bool folder = false;
    // Runs on the main thread once the item is dropped, with the path the target wants it at.
    // Call `done` on the main thread with an error message, or an empty string on success.
    std::function<void(const QString& target, std::function<void(const QString& error)> done)> write;
};

// Starts a native drag of file promises from the mouse event being handled (macOS): nothing is
// written until a target accepts the drop and picks the destination. `ended` runs on the main
// thread when the drag finishes. False where unsupported or when no mouse event is current.
bool start_promise_drag(QWidget* source, QList<PromisedItem> items, const QPixmap& image, std::function<void()> ended);
