// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "user_data.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <optional>

using satchel_gui::CleanupReport;

namespace
{

std::optional<bool> pending_include_temp;

void remove_path(const QString& path, CleanupReport& report)
{
    const QFileInfo info(path);
    const bool ok = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
    (ok ? report.removed : report.failed) << path;
}

#ifdef Q_OS_WIN
void remove_registry_group(const QString& parent, const QString& group, CleanupReport& report)
{
    QSettings key("HKEY_CURRENT_USER\\Software\\" + parent, QSettings::NativeFormat);
    if (!key.childGroups().contains(group))
        return;
    key.remove(group);
    key.sync();
    (key.status() == QSettings::NoError ? report.removed : report.failed) << "HKCU\\Software\\" + parent + "\\" + group;
}

void remove_registry_key_if_empty(const QString& name)
{
    QSettings key("HKEY_CURRENT_USER\\Software\\" + name, QSettings::NativeFormat);
    if (key.allKeys().isEmpty() && key.childGroups().isEmpty())
        QSettings("HKEY_CURRENT_USER\\Software", QSettings::NativeFormat).remove(name);
}
#endif

void clear_settings(CleanupReport& report)
{
    QSettings q;
    const auto location = q.fileName();
    q.clear();
    q.sync();
    const bool cleared = q.status() == QSettings::NoError;
#if defined(Q_OS_WIN)
    const auto organization = QCoreApplication::organizationName();
    remove_registry_group(organization, QCoreApplication::applicationName(), report);
    #ifdef ZP_VENDOR_NAME
    remove_registry_group(QString::fromUtf8(ZP_VENDOR_NAME) + "\\" + QCoreApplication::applicationName(), "WinSparkle", report);
    remove_registry_key_if_empty(QString::fromUtf8(ZP_VENDOR_NAME) + "\\" + QCoreApplication::applicationName());
    remove_registry_key_if_empty(QString::fromUtf8(ZP_VENDOR_NAME));
    #endif
    remove_registry_key_if_empty(organization);
    if (!cleared)
        report.failed << location;
#elif !defined(Q_OS_MACOS)
    if (cleared && QFile::exists(location))
    {
        remove_path(location, report);
        QDir().rmdir(QFileInfo(location).absolutePath());
    }
    else if (!cleared)
        report.failed << location;
#else
    if (!cleared)
        report.failed << location;
#endif
}

}

namespace satchel_gui
{

QStringList temp_leftovers(const QString& temp_dir)
{
    static const QRegularExpression folder(R"(^satchel-\d+$)");
    static const QRegularExpression spill(R"(^satchel-spill-.+\.flac$)");
    QStringList found;
    const QDir dir(temp_dir);
    for (const auto& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (folder.match(name).hasMatch())
            found << dir.filePath(name);
    for (const auto& name : dir.entryList(QDir::Files))
        if (spill.match(name).hasMatch())
            found << dir.filePath(name);
    return found;
}

CleanupReport clean_user_data(const QString& temp_dir, bool include_temp)
{
    CleanupReport report;
    clear_settings(report);
    if (include_temp)
        for (const auto& path : temp_leftovers(temp_dir))
            remove_path(path, report);
    return report;
}

}

bool user_data_cleanup_available()
{
#ifdef Q_OS_MACOS
    return false;
#else
    return true;
#endif
}

void request_user_data_cleanup(QWidget* parent)
{
    QMessageBox box(QMessageBox::Warning, QObject::tr("Delete Settings"), QObject::tr("Delete Satchel's settings and quit?"), QMessageBox::NoButton, parent);
    box.setInformativeText(QObject::tr("Settings, recent files and the saved window layout are reset. Your archives are not touched."));
    auto* with_temp = box.addButton(QObject::tr("Delete Settings and Temporary Files"), QMessageBox::DestructiveRole);
    auto* settings_only = box.addButton(QObject::tr("Delete Settings Only"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != with_temp && box.clickedButton() != settings_only)
        return;
    pending_include_temp = box.clickedButton() == with_temp;
    QApplication::quit();
}

void add_user_data_cleanup_action(QMenu* menu, QWidget* parent)
{
    if (!user_data_cleanup_available())
        return;
    auto* action = menu->addAction(QObject::tr("Delete Settings and Temporary Files…"));
    QObject::connect(action, &QAction::triggered, parent, [parent] { request_user_data_cleanup(parent); });
}

PendingCleanup::~PendingCleanup()
{
    if (!pending_include_temp)
        return;
    const auto report = satchel_gui::clean_user_data(QStandardPaths::writableLocation(QStandardPaths::TempLocation), *pending_include_temp);
    if (!report.failed.isEmpty())
        QMessageBox::warning(nullptr, QObject::tr("Delete Settings"), QObject::tr("These could not be deleted:\n%1").arg(report.failed.join('\n')));
}
