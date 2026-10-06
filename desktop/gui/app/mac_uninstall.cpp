// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "mac_uninstall.hpp"

#include "uninstall.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

bool uninstall_available()
{
    return true;
}

void run_uninstall_dialog(QWidget* parent)
{
    QMessageBox box(QMessageBox::Warning, QObject::tr("Uninstall Satchel"), QObject::tr("Remove Satchel, its Quick Look extension and Finder services?"), QMessageBox::NoButton, parent);
    box.setInformativeText(QObject::tr("The app is moved to the Trash. Your archives are not touched."));
    auto* all = box.addButton(QObject::tr("Uninstall and Delete Settings"), QMessageBox::DestructiveRole);
    auto* keep = box.addButton(QObject::tr("Uninstall and Keep Settings"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != all && box.clickedButton() != keep)
        return;

    satchel_macos::UninstallOptions options;
    options.home = QDir::homePath().toStdString();
    options.apps = satchel_macos::default_app_paths(options.home);
    options.apps.emplace_back(QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..").toStdString());
    options.keep_settings = box.clickedButton() == keep;
    options.quit_running = false;

    const auto report = satchel_macos::uninstall(options, satchel_macos::run_process);
    if (!report.failed.empty())
    {
        QStringList lines;
        for (const auto& line : report.failed)
            lines << QString::fromStdString(line);
        QMessageBox::critical(parent, QObject::tr("Uninstall incomplete"), QObject::tr("These steps failed:\n%1").arg(lines.join('\n')));
        return;
    }
    QApplication::quit();
}

void add_uninstall_action(QMenu* menu, QWidget* parent)
{
    if (!uninstall_available())
        return;
    auto* action = menu->addAction(QObject::tr("Uninstall Satchel…"));
    action->setMenuRole(QAction::ApplicationSpecificRole);
    QObject::connect(action, &QAction::triggered, parent, [parent] { run_uninstall_dialog(parent); });
}
