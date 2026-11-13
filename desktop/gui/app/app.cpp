// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#include "app.hpp"

#include "updater.hpp"

#include "main_window.hpp"
#include "settings_store.hpp"
#include "simple_window.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{

QString identity(const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

}

App::App(QObject* parent) :
    QObject(parent),
    settings_(load_settings())
{
    QApplication::setQuitOnLastWindowClosed(false);
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!now)
            return;
        for (auto* w : workspaces_)
        {
            if (now->window() == w->window())
                set_active(*w);
        }
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive || workspaces_.isEmpty())
            return;
        if (std::ranges::none_of(workspaces_, &Workspace::visible))
            (active_ ? active_ : workspaces_.last())->show();
    });
}

App::~App()
{
    qDeleteAll(std::exchange(workspaces_, {}));
}

void App::save()
{
    save_settings(settings_);
    if (updater_)
        updater_->set_automatic(settings_.check_updates);
}

void App::start_updates()
{
    if (!updater_)
        updater_ = Updater::create(settings_.check_updates);
}

Workspace& App::add_workspace(bool full)
{
    auto* workspace = new Workspace(*this, full);
    connect(&workspace->jobs(), &JobQueue::finished, this, &App::settle_later, Qt::QueuedConnection);
    workspaces_ << workspace;
    return *workspace;
}

void App::set_active(Workspace& workspace)
{
    active_ = &workspace;
    set_last_mode(workspace.is_full() ? "full" : "simple");
}

Workspace& App::new_window(bool full)
{
    constexpr int step = 28;
    const QWidget* anchor = active_ && active_->visible() ? active_->window() : nullptr;
    auto& workspace = add_workspace(full);
    if (anchor)
    {
        auto pos = anchor->pos() + QPoint(step, step);
        if (const auto* screen = anchor->screen())
        {
            const auto area = screen->availableGeometry();
            const auto size = workspace.window()->size();
            if (pos.x() + size.width() > area.right() || pos.y() + size.height() > area.bottom())
                pos = area.topLeft() + QPoint(step, step);
        }
        workspace.window()->move(pos);
    }
    workspace.show();
    return workspace;
}

Workspace& App::switch_mode(Workspace& from)
{
    const bool replace = from.pristine();
    const auto pos = from.window()->pos();
    active_ = &from;
    auto& to = new_window(!from.is_full());
    if (replace)
    {
        to.window()->move(pos);
        from.window()->close();
    }
    return to;
}

MainWindow& App::show_archive(const QString& archive)
{
    if (auto* shown = window_showing(archive))
    {
        shown->raise();
        shown->activateWindow();
        return *shown;
    }
    auto* workspace = active_ && active_->is_full() && active_->pristine() ? active_ : &new_window(true);
    workspace->show();
    workspace->full()->open_archive(archive);
    return *workspace->full();
}

MainWindow* App::window_showing(const QString& archive) const
{
    const auto wanted = identity(archive);
    for (auto* w : workspaces_)
    {
        if (w->is_full() && w->visible() && !w->full()->archive_path().isEmpty() && identity(w->full()->archive_path()) == wanted)
            return w->full();
    }
    return nullptr;
}

void App::open_paths(const QStringList& paths, satchel_gui::Intent intent)
{
    if (intent == satchel_gui::Intent::Auto && active_ && active_->is_full() && active_->visible() && paths.size() == 1 && satchel_gui::is_zip(std::filesystem::path(paths.front().toStdU16String())))
    {
        show_archive(paths.front());
        return;
    }
    const auto free = [](const Workspace* w) { return !w->is_full() && w->simple()->idle(); };
    auto* workspace = active_ && free(active_) ? active_ : nullptr;
    for (auto it = workspaces_.crbegin(); !workspace && it != workspaces_.crend(); ++it)
    {
        if (free(*it))
            workspace = *it;
    }
    if (!workspace)
        workspace = &new_window(false);
    workspace->show();
    workspace->simple()->handle(paths, intent);
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

void App::settle_later()
{
    QTimer::singleShot(0, this, &App::settle);
}

void App::settle()
{
    using namespace std::chrono_literals;
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget())
    {
        QTimer::singleShot(250ms, this, &App::settle);
        return;
    }
    const bool had_any = !workspaces_.isEmpty();
    for (auto* w : QList(workspaces_))
    {
        if (w->visible() || w->busy())
            continue;
        if (w->dirty())
        {
            w->show();
            continue;
        }
        workspaces_.removeOne(w);
        if (active_ == w)
            active_ = nullptr;
        delete w;
    }
    if (had_any && workspaces_.isEmpty())
        QApplication::quit();
}

bool App::hold_quit()
{
    QApplication::closeAllWindows();
    if (std::ranges::any_of(workspaces_, &Workspace::visible))
        return true;
    if (std::ranges::any_of(workspaces_, &Workspace::busy))
    {
        settle_later();
        return true;
    }
    return false;
}

bool App::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::Quit && hold_quit())
    {
        event->ignore();
        return true;
    }
    return QObject::eventFilter(watched, event);
}
