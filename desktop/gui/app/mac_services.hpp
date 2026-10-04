// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <QString>
#include <QStringList>

class App;

// Finder services "Compress with Satchel" and "Extract with Satchel" (Info.plist NSServices),
// forwarded to App::open_paths with the matching intent. No-ops outside macOS.
void install_mac_services(App& app);

// Sends `paths` through the same Objective-C entry point Finder uses (`message` is the NSMessage
// name, e.g. "compressFiles"). For tests; false if the message is unknown or not on macOS.
bool perform_mac_service(App& app, const QString& message, const QStringList& paths);
