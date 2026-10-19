// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Automated UI smoke tests (desktop UI spec, "Testing"): both modes launch, a 100,000-entry
// archive opens within the target, and a Simple-mode drop compresses end to end.
#include "app.hpp"
#include "archive_model.hpp"
#include "dialogs.hpp"
#include "jobs.hpp"
#include "mac_services.hpp"
#include "mac_uninstall.hpp"
#include "main_window.hpp"
#include "settings_store.hpp"
#include "simple_window.hpp"
#include "user_data.hpp"
#include <QAction>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include <format>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr qint64 limit_ms = 15000;
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr qint64 limit_ms = 15000;
    #else
constexpr qint64 limit_ms = 3000;
    #endif
#else
constexpr qint64 limit_ms = 3000;
#endif

class AppTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("Venn Audio Test");
        QCoreApplication::setApplicationName("Satchel Test");
        QVERIFY(dir_.isValid());
    }

    void both_modes_launch()
    {
        App app;
        app.show_simple();
        QVERIFY(app.simple()->isVisible());
        app.show_full();
        QVERIFY(app.full()->isVisible());
        QVERIFY(!app.simple()->isVisible());
        app.toggle_mode();
        QVERIFY(app.simple()->isVisible());
    }

    void large_archive_opens_quickly()
    {
        const auto path = dir_.filePath("large.zip");
        {
            zpp::Input input(zpp::not_null(zp_input_memory()));
            for (int i = 0; i < 100000; ++i)
            {
                const auto name = std::format("d{:03}/f{:06}.txt", i / 1000, i);
                zpp::check(zp_input_memory_add_file(input.get(), name.c_str(), nullptr, 0, 0, 0644));
            }
            zpp::Plan plan(zpp::not_null(zp_plan_create(app_context(), input.get(), nullptr)));
            zpp::Stream out(zpp::not_null(zp_stream_create_file(path.toStdString().c_str())));
            zpp::check(zp_build(plan.get(), out.get(), nullptr, nullptr, nullptr, nullptr));
            zpp::check(zp_stream_commit(out.get()));
        }
        App app;
        QElapsedTimer timer;
        timer.start();
        app.show_full(path);
        const auto ms = timer.elapsed();
        QCOMPARE(app.full()->visible_rows(), 100);
        qInfo("opened 100,000 entries in %lld ms", static_cast<long long>(ms));
        // The spec target is 1 s on a desktop; shared CI runners and sanitizer builds get some slack.
        QVERIFY2(ms < limit_ms, qPrintable(QString("took %1 ms").arg(ms)));
    }

    void flac_ratio_needs_the_original_size()
    {
        zpp::EntryInfo text;
        text.index = 0;
        text.name = "a.txt";
        text.uncompressed_size = 1000;
        text.compressed_size = 400;
        text.supported = true;
        zpp::EntryInfo audio;
        audio.index = 1;
        audio.name = "b.flac";
        audio.uncompressed_size = 500;
        audio.compressed_size = 500;
        audio.supported = true;
        audio.flac_restorable = true;
        audio.flac_original_name = "b.wav";
        zpp::EntryInfo known = audio;
        known.index = 2;
        known.name = "c.flac";
        known.original_size = 2000;
        ArchiveModel model;
        model.set_entries({ text, audio, known });
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.index(0, ArchiveModel::Ratio).data().toString(), QString("60%"));
        QCOMPARE(model.index(1, ArchiveModel::Ratio).data().toString(), QString());
        QCOMPARE(model.index(2, ArchiveModel::Ratio).data().toString(), QString("75%"));
        QCOMPARE(model.index(2, ArchiveModel::Size).data().toString(), QString::fromStdString(satchel_gui::human_size(2000)));
    }

    void simple_mode_compresses_a_drop()
    {
        const auto input = dir_.filePath("notes.txt");
        {
            QFile f(input);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray("notes ").repeated(1000));
        }
        App app;
        app.show_simple();
        QSignalSpy finished(&app.jobs(), &JobQueue::finished);
        app.simple()->handle({ input });
        QVERIFY(finished.wait(20000));
        const auto outcome = finished.takeFirst().at(2).value<JobOutcome>();
        QCOMPARE(outcome.status, ZP_OK);
        QCOMPARE(QDir::fromNativeSeparators(outcome.output), dir_.filePath("notes.txt.zip"));
        QVERIFY(QFile::exists(outcome.output));

        // Dropping the zip extracts it next to itself.
        app.simple()->handle({ outcome.output });
        QVERIFY(finished.wait(20000));
        const auto extracted = finished.takeFirst().at(2).value<JobOutcome>();
        QCOMPARE(extracted.status, ZP_OK);
        // "notes.txt" exists as a file, so the folder gets a suffix instead of clashing.
        QCOMPARE(QDir::fromNativeSeparators(extracted.output), dir_.filePath("notes.txt 2"));
        QVERIFY(QFile::exists(dir_.filePath("notes.txt 2/notes.txt")));
    }

    void excludes_follow_the_settings()
    {
        QVERIFY(QDir(dir_.path()).mkpath("proj/sub"));
        for (const auto* name : { "proj/.DS_Store", "proj/kick.wav.asd", "proj/sub/keep.txt" })
        {
            QFile f(dir_.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        const QStringList inputs{ dir_.filePath("proj") };
        const auto excluded = [&](const satchel_gui::Settings& s) {
            QStringList out;
            for (const auto& x : zpp::plan_excluded(make_plan(inputs, s)->get()))
                out << QString::fromStdString(x.archive_path);
            return out;
        };
        satchel_gui::Settings s;
        QCOMPARE(excluded(s), QStringList{ "proj/.DS_Store" });
        s.exclude_daw_caches = true;
        QCOMPARE(excluded(s), (QStringList{ "proj/.DS_Store", "proj/kick.wav.asd" }));
        s.exclude_system = false;
        s.exclude_daw_caches = false;
        s.exclude_patterns = { "sub/" };
        QCOMPARE(excluded(s), QStringList{ "proj/sub" });
        QVERIFY(exclude_pattern_error({ "a/b" }).contains("a/b"));
        QVERIFY(exclude_pattern_error(exclude_patterns(s)).isEmpty());
    }

    void exclude_settings_persist_and_edit()
    {
        satchel_gui::Settings s;
        s.exclude_system = false;
        s.exclude_daw_caches = true;
        s.exclude_patterns = { "*.tmp", "Old Takes/" };
        save_settings(s);
        const auto loaded = load_settings();
        QCOMPARE(loaded.exclude_system, false);
        QCOMPARE(loaded.exclude_daw_caches, true);
        QVERIFY(loaded.exclude_patterns == s.exclude_patterns);
        save_settings({});

        ExcludeEditor editor(s);
        satchel_gui::Settings round;
        editor.apply_to(round);
        QCOMPARE(round.exclude_system, false);
        QCOMPARE(round.exclude_daw_caches, true);
        QVERIFY(round.exclude_patterns == s.exclude_patterns);
    }

    void finder_service_compresses_files()
    {
#ifdef Q_OS_MACOS
        const auto input = dir_.filePath("service.txt");
        {
            QFile f(input);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray("service ").repeated(500));
        }
        App app;
        QSignalSpy finished(&app.jobs(), &JobQueue::finished);
        QVERIFY(perform_mac_service(app, "compressFiles", { input }));
        QVERIFY(finished.wait(20000));
        const auto outcome = finished.takeFirst().at(2).value<JobOutcome>();
        QCOMPARE(outcome.status, ZP_OK);
        QCOMPARE(QDir::fromNativeSeparators(outcome.output), dir_.filePath("service.txt.zip"));
        QVERIFY(!perform_mac_service(app, "unknownMessage", { input }));
#else
        QSKIP("macOS only");
#endif
    }

    void uninstall_menu_item_exists_only_on_macos()
    {
        App app;
        app.show_simple();
        app.show_full();
        for (QWidget* window : { static_cast<QWidget*>(app.simple()), static_cast<QWidget*>(app.full()) })
        {
            int found = 0;
            for (auto* action : window->findChildren<QAction*>())
                found += action->text() == "Uninstall Satchel…";
            QCOMPARE(found, uninstall_available() ? 1 : 0);
        }
#ifdef Q_OS_MACOS
        QVERIFY(uninstall_available());
#endif
    }

    void cleanup_menu_item_exists_where_available()
    {
        App app;
        app.show_simple();
        app.show_full();
        for (QWidget* window : { static_cast<QWidget*>(app.simple()), static_cast<QWidget*>(app.full()) })
        {
            int found = 0;
            for (auto* action : window->findChildren<QAction*>())
                found += action->text() == "Delete Settings and Temporary Files…";
            QCOMPARE(found, user_data_cleanup_available() ? 1 : 0);
        }
    }

    void cleanup_deletes_settings_and_only_satchel_temp_files()
    {
        QTemporaryDir config;
        QTemporaryDir temp;
        QVERIFY(config.isValid() && temp.isValid());
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, config.path());
#endif
        QSettings().setValue("recent", QStringList{ "a.zip" });
        QSettings().sync();
        const QDir dir(temp.path());
        QVERIFY(dir.mkpath("satchel-1700000000000"));
        QVERIFY(QFile(dir.filePath("satchel-1700000000000/a.wav")).open(QIODevice::WriteOnly));
        QVERIFY(QFile(dir.filePath("satchel-spill-00ab12cd-0.flac")).open(QIODevice::WriteOnly));
        QVERIFY(QFile(dir.filePath("keep.txt")).open(QIODevice::WriteOnly));
        QVERIFY(dir.mkpath("satchel-notes"));
        QCOMPARE(satchel_gui::temp_leftovers(temp.path()).size(), 2);

        const auto settings_only = satchel_gui::clean_user_data(temp.path(), false);
        QVERIFY(settings_only.failed.isEmpty());
        QVERIFY(!QSettings().contains("recent"));
        QCOMPARE(satchel_gui::temp_leftovers(temp.path()).size(), 2);

        const auto leftovers = satchel_gui::temp_leftovers(temp.path());
        const auto all = satchel_gui::clean_user_data(temp.path(), true);
        QVERIFY(all.failed.isEmpty());
        for (const auto& path : leftovers)
            QVERIFY(all.removed.contains(path));
        QVERIFY(satchel_gui::temp_leftovers(temp.path()).isEmpty());
        QVERIFY(dir.exists("keep.txt"));
        QVERIFY(dir.exists("satchel-notes"));
    }

    void full_mode_creates_then_edits_an_archive()
    {
        QDir root(dir_.filePath("full"));
        QVERIFY(root.mkpath("src/docs"));
        for (const auto* name : { "src/a.txt", "src/docs/b.txt", "src/docs/c.txt" })
        {
            QFile f(root.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(name);
        }
        const auto target = root.filePath("made.zip");
        App app;
        app.show_full();
        auto* w = app.full();
        auto& session = w->session();
        const auto settled = [&] { return !session.busy() && session.snapshot(); };

        w->start_new_archive(target, { root.filePath("src/a.txt"), root.filePath("src/docs") });
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 20000);
        QVERIFY(session.snapshot()->is_new);
        QCOMPARE(session.snapshot()->view.summary.added, std::size_t{ 3 });
        QVERIFY(!QFile::exists(target));
        QVERIFY(w->windowTitle().startsWith("made.zip"));

        QSignalSpy finished(&app.jobs(), &JobQueue::finished);
        QVERIFY(w->apply_changes() != 0);
        QVERIFY(finished.wait(20000));
        QCOMPARE(finished.takeFirst().at(2).value<JobOutcome>().status, ZP_OK);
        QTRY_VERIFY_WITH_TIMEOUT(settled() && !session.snapshot()->is_new, 20000);
        QVERIFY(QFile::exists(target));
        QCOMPARE(w->archive_path(), target);
        QVERIFY(!session.snapshot()->has_changes);

        // Rename in place, delete, add into a folder: staged until Apply.
        auto& model = w->model();
        QModelIndex a;
        QModelIndex docs;
        for (int r = 0; r < model.rowCount(); ++r)
        {
            const auto idx = model.index(r, ArchiveModel::Name);
            if (idx.data(ArchiveModel::PathRole).toString() == "a.txt")
                a = idx;
            if (idx.data(ArchiveModel::PathRole).toString() == "docs")
                docs = idx;
        }
        QVERIFY(a.isValid() && docs.isValid());
        QVERIFY(model.flags(a) & Qt::ItemIsEditable);
        model.setData(a, "alpha.txt", Qt::EditRole);
        QTRY_VERIFY_WITH_TIMEOUT(settled() && session.snapshot()->has_changes, 20000);
        satchel_gui::Change remove{ satchel_gui::op::RemoveFolder{ "docs" } };
        session.apply(remove);
        QFile extra(root.filePath("extra.txt"));
        QVERIFY(extra.open(QIODevice::WriteOnly));
        extra.close();
        w->add_paths({ extra.fileName() }, "new");
        QTRY_VERIFY_WITH_TIMEOUT(settled() && session.snapshot()->view.summary.added == 1, 20000);
        const auto& sum = session.snapshot()->view.summary;
        QCOMPARE(sum.renamed, std::size_t{ 1 });
        QCOMPARE(sum.removed, std::size_t{ 2 });
        QVERIFY(w->isWindowModified());

        session.undo();
        QTRY_VERIFY_WITH_TIMEOUT(settled() && session.snapshot()->view.summary.added == 0, 20000);
        session.redo();
        QTRY_VERIFY_WITH_TIMEOUT(settled() && session.snapshot()->view.summary.added == 1, 20000);

        QVERIFY(w->apply_changes() != 0);
        QVERIFY(finished.wait(20000));
        QCOMPARE(finished.takeFirst().at(2).value<JobOutcome>().status, ZP_OK);
        QTRY_VERIFY_WITH_TIMEOUT(settled() && !session.snapshot()->has_changes, 20000);
        QStringList names;
        for (const auto& e : session.snapshot()->view.rows)
            names << QString::fromStdString(e.key.name);
        names.sort();
        QCOMPARE(names, (QStringList{ "alpha.txt", "new/extra.txt" }));
        QVERIFY(!w->isWindowModified());
    }

    void translations_are_embedded()
    {
#ifdef ZP_HAS_TRANSLATIONS
        QVERIFY(QFile::exists(":/i18n/satchel_en.qm"));
        QTranslator translator;
        QVERIFY(translator.load(QLocale(QLocale::English), "satchel", "_", ":/i18n"));
#else
        QSKIP("built without Qt Linguist tools");
#endif
    }

private:
    QTemporaryDir dir_;
};

QTEST_MAIN(AppTest)
#include "test_app.moc"
