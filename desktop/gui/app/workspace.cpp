// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#include "workspace.hpp"

#include "app.hpp"
#include "main_window.hpp"
#include "settings_store.hpp"
#include "simple_window.hpp"

#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>

Workspace::Workspace(App& app, bool full) :
    QObject(&app),
    app_(app),
    jobs_(app.settings().threads)
{
    if (full)
        full_ = new MainWindow(*this);
    else
        simple_ = new SimpleWindow(*this);
}

Workspace::~Workspace()
{
    delete simple_;
    delete full_;
}

QWidget* Workspace::window() const
{
    return full_ ? static_cast<QWidget*>(full_) : simple_;
}

void Workspace::show()
{
    auto* w = window();
    w->show();
    w->raise();
    w->activateWindow();
    app_.set_active(*this);
}

bool Workspace::visible() const
{
    return window()->isVisible();
}

bool Workspace::busy() const
{
    return jobs_.busy() || (full_ && full_->session().busy());
}

bool Workspace::dirty() const
{
    return full_ && full_->dirty();
}

bool Workspace::pristine() const
{
    if (jobs_.busy())
        return false;
    return full_ ? full_->pristine() : simple_->pristine();
}

JobsOnClose ask_jobs_on_close(QWidget* parent)
{
    const auto text = [](const char* source) { return QCoreApplication::translate("Workspace", source); };
    QMessageBox box(QMessageBox::Question, text(QT_TRANSLATE_NOOP("Workspace", "A job is running")), text(QT_TRANSLATE_NOOP("Workspace", "This window has a job that is not finished. Cancel it, or keep it running in the background?")), QMessageBox::NoButton, parent);
    const auto* cancel = box.addButton(text(QT_TRANSLATE_NOOP("Workspace", "Cancel Job")), QMessageBox::DestructiveRole);
    auto* keep = box.addButton(text(QT_TRANSLATE_NOOP("Workspace", "Keep Running")), QMessageBox::AcceptRole);
    auto* stay = box.addButton(text(QT_TRANSLATE_NOOP("Workspace", "Don't Close")), QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.setEscapeButton(stay);
    box.exec();
    if (box.clickedButton() == cancel)
        return JobsOnClose::Cancel;
    return box.clickedButton() == keep ? JobsOnClose::Keep : JobsOnClose::StayOpen;
}

QKeySequence new_window_shortcut()
{
#if defined(Q_OS_MACOS)
    return { Qt::CTRL | Qt::ALT | Qt::Key_N };
#else
    return { Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_N };
#endif
}
