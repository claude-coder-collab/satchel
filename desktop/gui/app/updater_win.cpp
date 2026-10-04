// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "updater.hpp"

#include <windows.h>

namespace
{

class WinSparkleUpdater final : public Updater
{
public:
    explicit WinSparkleUpdater(HMODULE dll) :
        dll_(dll)
    {
    }
    WinSparkleUpdater(const WinSparkleUpdater&) = delete;
    WinSparkleUpdater& operator=(const WinSparkleUpdater&) = delete;
    WinSparkleUpdater(WinSparkleUpdater&&) = delete;
    WinSparkleUpdater& operator=(WinSparkleUpdater&&) = delete;
    ~WinSparkleUpdater() override
    {
        if (auto cleanup = function<void (*)()>("win_sparkle_cleanup"))
            cleanup();
        FreeLibrary(dll_);
    }

    bool start(bool automatic)
    {
        auto set_url = function<void (*)(const char*)>("win_sparkle_set_appcast_url");
        auto set_key = function<int (*)(const char*)>("win_sparkle_set_eddsa_public_key");
        auto init = function<void (*)()>("win_sparkle_init");
        if (!set_url || !set_key || !init)
            return false;
        set_url(ZP_UPDATE_FEED_URL);
        if (set_key(ZP_UPDATE_PUBLIC_KEY) == 0)
            return false;
        set_automatic(automatic);
        init();
        return true;
    }

    [[nodiscard]] bool available() const override { return true; }

    void check_now() override
    {
        if (auto check = function<void (*)()>("win_sparkle_check_update_with_ui"))
            check();
    }

    void set_automatic(bool enabled) override
    {
        if (auto set = function<void (*)(int)>("win_sparkle_set_automatic_check_for_updates"))
            set(enabled ? 1 : 0);
    }

private:
    template <typename F>
    F function(const char* name) const
    {
        return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll_, name)));
    }

    HMODULE dll_;
};

}

std::unique_ptr<Updater> Updater::create(bool automatic)
{
    HMODULE dll = LoadLibraryExW(L"WinSparkle.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!dll)
        return std::make_unique<Updater>();
    auto updater = std::make_unique<WinSparkleUpdater>(dll);
    if (!updater->start(automatic))
        return std::make_unique<Updater>();
    return updater;
}
