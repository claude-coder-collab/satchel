// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "app.hpp"

#include "main_window.hpp"
#include "settings_store.hpp"
#include "simple_window.hpp"

#include <QApplication>
#include <QSystemTrayIcon>

App::App(QObject* parent) :
    QObject(parent),
    settings_(load_settings())
{
}

App::~App()
{
    delete simple_;
    delete full_;
}

void App::save()
{
    save_settings(settings_);
}

void App::show_simple()
{
    if (!simple_)
        simple_ = new SimpleWindow(*this);
    if (full_)
        full_->hide();
    simple_->show();
    simple_->raise();
    simple_->activateWindow();
    set_last_mode("simple");
}

void App::show_full(const QString& archive)
{
    if (!full_)
        full_ = new MainWindow(*this);
    if (simple_)
        simple_->hide();
    full_->show();
    full_->raise();
    full_->activateWindow();
    if (!archive.isEmpty())
        full_->open_archive(archive);
    set_last_mode("full");
}

void App::toggle_mode()
{
    if (full_ && full_->isVisible())
        show_simple();
    else
        show_full();
}

void App::open_paths(const QStringList& paths, satchel_gui::Intent intent)
{
    if (intent == satchel_gui::Intent::Auto && full_ && full_->isVisible() && paths.size() == 1 && satchel_gui::is_zip(std::filesystem::path(paths.front().toStdU16String())))
    {
        full_->open_archive(paths.front());
        return;
    }
    show_simple();
    simple_->handle(paths, intent);
}

void App::notify(const QString& title, const QString& text)
{
    static QSystemTrayIcon* tray = nullptr;
    if (QSystemTrayIcon::isSystemTrayAvailable())
    {
        if (!tray)
        {
            tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
            tray->show();
        }
        tray->showMessage(title, text);
    }
    QApplication::alert(nullptr);
}
