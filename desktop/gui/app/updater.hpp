// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <memory>

// Application updates (desktop UI spec, "Updates"): Sparkle on macOS and WinSparkle on Windows,
// both loaded at run time from the installed app. Unavailable on Linux (package manager or
// AppImage) and when the library is not bundled (development builds).
class Updater
{
public:
    Updater() = default;
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    Updater(Updater&&) = delete;
    Updater& operator=(Updater&&) = delete;
    virtual ~Updater() = default;

    [[nodiscard]] virtual bool available() const { return false; }
    virtual void check_now() {}
    virtual void set_automatic(bool enabled) { (void) enabled; }

    // The platform updater, started with automatic checks on or off; a no-op updater if the
    // library cannot be loaded.
    static std::unique_ptr<Updater> create(bool automatic);
};
