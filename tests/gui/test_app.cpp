// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Automated UI smoke tests (desktop UI spec, "Testing"): both modes launch, a 100,000-entry
// archive opens within the target, and a Simple-mode drop compresses end to end.
#include "app.hpp"
#include "main_window.hpp"
#include "simple_window.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <format>

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
        QCOMPARE(app.full()->visible_rows(), 100000);
        qInfo("opened 100,000 entries in %lld ms", static_cast<long long>(ms));
        // The spec target is 1 s on a desktop; shared CI runners get some slack.
        QVERIFY2(ms < 3000, qPrintable(QString("took %1 ms").arg(ms)));
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
        QCOMPARE(outcome.output, dir_.filePath("notes.txt.zip"));
        QVERIFY(QFile::exists(outcome.output));

        // Dropping the zip extracts it next to itself.
        app.simple()->handle({ outcome.output });
        QVERIFY(finished.wait(20000));
        const auto extracted = finished.takeFirst().at(2).value<JobOutcome>();
        QCOMPARE(extracted.status, ZP_OK);
        // "notes.txt" exists as a file, so the folder gets a suffix instead of clashing.
        QCOMPARE(extracted.output, dir_.filePath("notes.txt 2"));
        QVERIFY(QFile::exists(dir_.filePath("notes.txt 2/notes.txt")));
    }

private:
    QTemporaryDir dir_;
};

QTEST_MAIN(AppTest)
#include "test_app.moc"
