// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "app.hpp"
#include "main_window.hpp"
#include "settings_store.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFileOpenEvent>
#include <QTimer>

#include <filesystem>
#include <print>
#include <utility>

namespace
{

// macOS delivers files dropped on the Dock icon as QFileOpenEvent.
class FileOpenFilter : public QObject
{
public:
    explicit FileOpenFilter(App& app) :
        app_(app)
    {
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::FileOpen)
        {
            if (const auto* open = dynamic_cast<QFileOpenEvent*>(event))
                pending_ << open->file();
            QTimer::singleShot(100, this, [this] {
                if (!pending_.isEmpty())
                    app_.open_paths(std::exchange(pending_, {}));
            });
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    App& app_;
    QStringList pending_;
};

}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setApplicationName("Satchel");
    QApplication::setOrganizationName("Venn Audio");
    QApplication::setOrganizationDomain("venn-audio.invalid");
    QApplication::setApplicationVersion(QString::fromUtf8(zp_version()));

    QCommandLineParser parser;
    parser.setApplicationDescription("Satchel — lossless media packaging as standard zip archives");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption simple("simple", "Start in Simple mode");
    QCommandLineOption full("full", "Start in Full mode");
    QCommandLineOption compress("compress", "Compress the given files into one archive (file-manager action)");
    QCommandLineOption extract("extract", "Extract each given zip archive (file-manager action)");
    QCommandLineOption smoke("smoke-test", "Open the archive in Full mode, report the time to list it, and quit", "archive");
    parser.addOptions({ simple, full, compress, extract, smoke });
    parser.addPositionalArgument("files", "Zip archives to extract, or files to compress (Simple mode rule)");
    parser.process(qapp);

    App app;
    FileOpenFilter filter(app);
    qapp.installEventFilter(&filter);

    if (parser.isSet(smoke))
    {
        QElapsedTimer timer;
        timer.start();
        app.show_full(parser.value(smoke));
        const auto ms = timer.elapsed();
        const auto rows = app.full()->visible_rows();
        app.show_simple();
        std::println("listed {} rows in {} ms", rows, ms);
        return rows > 0 ? 0 : 1;
    }

    const auto files = parser.positionalArguments();
    if (files.size() == 1 && parser.isSet(full) && satchel_gui::is_zip(std::filesystem::path(files.front().toStdU16String())))
        app.show_full(files.front());
    else if (!files.isEmpty())
        app.open_paths(files, parser.isSet(compress) ? satchel_gui::Intent::Compress : parser.isSet(extract) ? satchel_gui::Intent::Extract
                                                                                                             : satchel_gui::Intent::Auto);
    else if (parser.isSet(full) || (!parser.isSet(simple) && app.settings().start_in_last_mode && last_mode() == "full"))
        app.show_full();
    else
        app.show_simple();
    return QApplication::exec();
}
