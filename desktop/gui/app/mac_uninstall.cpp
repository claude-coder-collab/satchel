// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "mac_uninstall.hpp"

#include "uninstall.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStorageInfo>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>

namespace
{

constexpr std::string_view helper_flag = "--uninstall-after";

bool running(long long pid)
{
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

std::string applescript_string(std::string_view text)
{
    std::string out = "\"";
    for (const char ch : text)
    {
        if (ch == '"' || ch == '\\')
            out += '\\';
        out += ch;
    }
    return out + '"';
}

}

bool uninstall_available()
{
    return true;
}

QStringList uninstall_helper_arguments(const UninstallRequest& request)
{
    QStringList args{ QString::fromUtf8(helper_flag.data(), static_cast<qsizetype>(helper_flag.size())), QString::number(request.pid), QStringLiteral("--app"), QString::fromStdString(request.app) };
    if (request.keep_settings)
        args << QStringLiteral("--keep-settings");
    return args;
}

std::optional<UninstallRequest> parse_uninstall_helper(int argc, char** argv)
{
    if (argc < 3 || argv[1] != helper_flag)
        return std::nullopt;
    UninstallRequest r;
    char* end = nullptr;
    r.pid = std::strtoll(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || r.pid <= 0)
        return std::nullopt;
    for (int i = 3; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--keep-settings")
            r.keep_settings = true;
        else if (arg == "--app" && i + 1 < argc)
            r.app = argv[++i];
        else
            return std::nullopt;
    }
    return r;
}

int run_uninstall_helper(const UninstallRequest& request, std::chrono::milliseconds wait)
{
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (running(request.pid))
    {
        if (std::chrono::steady_clock::now() > deadline)
            return 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        return 1;
    satchel_macos::UninstallOptions options;
    options.home = home;
    options.apps = satchel_macos::default_app_paths(options.home);
    if (!request.app.empty())
        options.apps.emplace_back(request.app);
    options.keep_settings = request.keep_settings;
    options.quit_running = false;
    const auto report = satchel_macos::uninstall(options, satchel_macos::run_process);
    if (report.failed.empty())
        return 0;
    std::string lines;
    for (const auto& line : report.failed)
        lines += line + "\n";
    satchel_macos::run_process({ "osascript", "-e", "display alert \"Satchel was not fully uninstalled\" message " + applescript_string("These steps failed:\n" + lines) + " as warning" });
    return 1;
}

void run_uninstall_dialog(QWidget* parent)
{
    const auto bundle = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    if (QStorageInfo(bundle).isReadOnly())
    {
        QMessageBox::information(parent, QObject::tr("Uninstall Satchel"), QObject::tr("Satchel is running from a disk image or another read-only location, so there is nothing to uninstall here. Quit Satchel and eject the disk image instead."));
        return;
    }
    QMessageBox box(QMessageBox::Warning, QObject::tr("Uninstall Satchel"), QObject::tr("Remove Satchel, its Quick Look extension and Finder services?"), QMessageBox::NoButton, parent);
    box.setInformativeText(QObject::tr("Satchel quits, then it is moved to the Trash. Your archives are not touched."));
    auto* all = box.addButton(QObject::tr("Uninstall and Delete Settings"), QMessageBox::DestructiveRole);
    auto* keep = box.addButton(QObject::tr("Uninstall and Keep Settings"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != all && box.clickedButton() != keep)
        return;

    UninstallRequest request;
    request.pid = QCoreApplication::applicationPid();
    request.keep_settings = box.clickedButton() == keep;
    request.app = bundle.toStdString();
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), uninstall_helper_arguments(request)))
    {
        QMessageBox::critical(parent, QObject::tr("Cannot uninstall"), QObject::tr("The uninstaller could not be started. You can also run `satchel uninstall` in Terminal."));
        return;
    }
    QApplication::quit();
}

void add_uninstall_action(QMenu* menu, QWidget*)
{
    if (!uninstall_available())
        return;
    // The application menu is shared by every window and keeps the items it is given, so the
    // item is added once, from the first window that asks.
    static bool added = false;
    if (std::exchange(added, true))
        return;
    auto* action = new QAction(QObject::tr("Uninstall Satchel…"), qApp);
    action->setMenuRole(QAction::ApplicationSpecificRole);
    QObject::connect(action, &QAction::triggered, qApp, [] { run_uninstall_dialog(QApplication::activeWindow()); });
    menu->addAction(action);
}
