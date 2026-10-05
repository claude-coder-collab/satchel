// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "simple_window.hpp"

#include "app.hpp"
#include "dialogs.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace
{

std::filesystem::path fs_path(const QString& s)
{
    return { s.toStdU16String() };
}

QString q_path(const std::filesystem::path& p)
{
    return QString::fromStdU16String(p.u16string());
}

bool exists(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

}

void reveal_in_file_manager(const QString& path)
{
#if defined(Q_OS_MACOS)
    QProcess::startDetached("open", { "-R", path });
#elif defined(Q_OS_WIN)
    QProcess::startDetached("explorer", { "/select,", QDir::toNativeSeparators(path) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

SimpleWindow::SimpleWindow(App& app) :
    app_(app)
{
    setWindowTitle(tr("Satchel"));
    setAcceptDrops(true);
    resize(420, 300);
    auto* layout = new QVBoxLayout(this);
    auto* menu_bar = new QMenuBar(this);
    auto* mode_menu = menu_bar->addMenu(tr("&View"));
    auto* to_full = mode_menu->addAction(tr("Switch to Full Mode"));
    to_full->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    connect(to_full, &QAction::triggered, this, [this] { app_.toggle_mode(); });
    layout->setMenuBar(menu_bar);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_);

    auto* idle = new QWidget(pages_);
    auto* idle_layout = new QVBoxLayout(idle);
    hint_ = new QLabel(tr("Drop files here to compress them,\nor drop a .zip to extract it."), idle);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setStyleSheet("QLabel { border: 2px dashed palette(mid); border-radius: 12px; padding: 40px; }");
    idle_layout->addWidget(hint_, 1);
    pages_->addWidget(idle);

    auto* run = new QWidget(pages_);
    auto* run_layout = new QVBoxLayout(run);
    running_ = new QLabel(run);
    running_->setWordWrap(true);
    bar_ = new QProgressBar(run);
    bar_->setRange(0, 1000);
    rate_ = new QLabel(run);
    auto* cancel = new QPushButton(tr("Cancel"), run);
    connect(cancel, &QPushButton::clicked, this, [this] { app_.jobs().cancel(current_); });
    run_layout->addStretch();
    run_layout->addWidget(running_);
    run_layout->addWidget(bar_);
    run_layout->addWidget(rate_);
    run_layout->addWidget(cancel, 0, Qt::AlignRight);
    run_layout->addStretch();
    pages_->addWidget(run);

    auto* done = new QWidget(pages_);
    auto* done_layout = new QVBoxLayout(done);
    result_ = new QLabel(done);
    result_->setWordWrap(true);
    result_->setStyleSheet("font-size: 15px; font-weight: 600;");
    details_ = new QLabel(done);
    details_->setWordWrap(true);
    auto* buttons = new QHBoxLayout;
    reveal_ = new QPushButton(done);
    retry_ = new QPushButton(tr("Retry"), done);
    auto* full = new QPushButton(tr("Open in Full mode"), done);
    auto* again = new QPushButton(tr("Done"), done);
    connect(reveal_, &QPushButton::clicked, this, [this] { reveal_in_file_manager(last_output_); });
    connect(retry_, &QPushButton::clicked, this, [this] { handle(last_input_); });
    connect(full, &QPushButton::clicked, this, [this] {
        app_.show_full(last_output_.endsWith(".zip", Qt::CaseInsensitive) ? last_output_ : QString());
    });
    connect(again, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(0); });
    buttons->addWidget(reveal_);
    buttons->addWidget(retry_);
    buttons->addWidget(full);
    buttons->addStretch();
    buttons->addWidget(again);
    done_layout->addStretch();
    done_layout->addWidget(result_);
    done_layout->addWidget(details_);
    done_layout->addLayout(buttons);
    done_layout->addStretch();
    pages_->addWidget(done);

    connect(&app_.jobs(), &JobQueue::started, this, &SimpleWindow::on_started);
    connect(&app_.jobs(), &JobQueue::progress, this, &SimpleWindow::on_progress);
    connect(&app_.jobs(), &JobQueue::finished, this, &SimpleWindow::on_finished);
}

void SimpleWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void SimpleWindow::dropEvent(QDropEvent* e)
{
    QStringList paths;
    for (const auto& url : e->mimeData()->urls())
    {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    handle(paths);
}

void SimpleWindow::handle(const QStringList& paths, satchel_gui::Intent intent)
{
    if (paths.isEmpty())
        return;
    last_input_ = paths;
    std::vector<std::filesystem::path> items;
    for (const auto& p : paths)
        items.push_back(fs_path(p));
    const auto action = satchel_gui::drop_action(items, intent);
    const auto first_new_job = mine_.size();
    QStringList selected;
    for (const auto& p : satchel_gui::action_items(items, action))
        selected << q_path(p);
    switch (action)
    {
        case satchel_gui::DropAction::ExtractEach:
            for (const auto& p : selected)
                start_extract(p);
            break;
        case satchel_gui::DropAction::CompressAll:
            start_compress(selected);
            break;
        case satchel_gui::DropAction::None:
            break;
    }
    if (intent != satchel_gui::Intent::Auto)
    {
        for (auto i = first_new_job; i < mine_.size(); ++i)
            quit_when_clean_.insert(mine_[i]);
    }
}

void SimpleWindow::start_compress(const QStringList& paths)
{
    const auto& s = app_.settings();
    std::vector<std::filesystem::path> items;
    for (const auto& p : paths)
        items.push_back(fs_path(p));
    const auto output = q_path(satchel_gui::compress_output(items, s.simple_output_folder, exists));
    std::shared_ptr<zpp::Plan> plan;
    try
    {
        plan = PlanReviewDialog::make_plan(paths, s);
    } catch (const zpp::Error& e)
    {
        QMessageBox::critical(this, tr("Cannot compress"), QString::fromUtf8(e.what()));
        return;
    }
    if (!zp_plan_executable(plan->get()))
    {
        ResolutionSheet sheet(plan->get(), this);
        if (sheet.exec() != QDialog::Accepted || !sheet.apply())
            return;
    }
    const auto settings = s;
    mine_ << app_.jobs().enqueue(tr("Compressing to %1").arg(QFileInfo(output).fileName()), [plan, output, settings](JobContext& jc) {
        auto o = compress_job(jc, plan->get(), output, settings);
        if (o.status == ZP_OK && settings.verify_after_build)
        {
            auto v = verify_job(jc, output);
            o.summary += QString(" · %1").arg(v.summary);
            o.details << v.details;
        }
        return o;
    });
}

void SimpleWindow::start_extract(const QString& archive)
{
    const auto dest = q_path(satchel_gui::extract_output(fs_path(archive), app_.settings().simple_output_folder, exists));
    const auto settings = app_.settings();
    mine_ << app_.jobs().enqueue(tr("Extracting %1").arg(QFileInfo(archive).fileName()), [archive, dest, settings](JobContext& jc) {
        return extract_job(jc, archive, dest, settings, {}, false);
    });
}

void SimpleWindow::on_started(int id, const QString& title)
{
    if (!mine_.contains(id))
        return;
    current_ = id;
    timer_.start();
    running_->setText(title);
    bar_->setValue(0);
    rate_->clear();
    pages_->setCurrentIndex(1);
}

void SimpleWindow::on_progress(int id, quint64 done, quint64 total)
{
    if (id != current_)
        return;
    bar_->setValue(total ? static_cast<int>(1000.0 * static_cast<double>(done) / static_cast<double>(total)) : 0);
    const double seconds = static_cast<double>(timer_.elapsed()) / 1000.0;
    if (seconds > 0.5 && done > 0)
    {
        const double rate = static_cast<double>(done) / seconds;
        const auto left = static_cast<long long>(static_cast<double>(total - done) / rate);
        rate_->setText(tr("%1/s · about %2:%3 left")
                .arg(QString::fromStdString(satchel_gui::human_size(static_cast<std::uint64_t>(rate))))
                .arg(left / 60)
                .arg(left % 60, 2, 10, QChar('0')));
    }
}

void SimpleWindow::on_finished(int id, const QString& title, const JobOutcome& outcome)
{
    if (!mine_.removeOne(id))
        return;
    const bool quit_if_clean = quit_when_clean_.remove(id);
    last_output_ = outcome.output;
    const bool ok = outcome.status == ZP_OK || outcome.status == ZP_SOURCE_CHANGED;
    result_->setText(ok ? outcome.summary : (outcome.status == ZP_CANCELLED ? tr("Cancelled") : tr("Failed: %1").arg(outcome.message)));
    details_->setText(outcome.details.mid(0, 8).join('\n') + (outcome.details.size() > 8 ? tr("\n… and %1 more").arg(outcome.details.size() - 8) : QString()));
    reveal_->setText(
#if defined(Q_OS_MACOS)
        tr("Reveal in Finder")
#else
        tr("Show in folder")
#endif
    );
    reveal_->setVisible(ok);
    retry_->setVisible(!ok && outcome.status != ZP_CANCELLED);
    if (quit_if_clean && outcome.status == ZP_OK && outcome.details.isEmpty() && mine_.isEmpty() && !app_.jobs().busy() && !(app_.full() && app_.full()->isVisible()))
    {
        QApplication::quit();
        return;
    }
    pages_->setCurrentIndex(2);
    if (!isActiveWindow())
        app_.notify(title, ok ? outcome.summary : outcome.message);
}

void SimpleWindow::closeEvent(QCloseEvent* e)
{
    if (app_.jobs().busy())
    {
        const auto answer = QMessageBox::question(this, tr("A job is running"), tr("Cancel the running job, or keep it running in the background?"), QMessageBox::Discard | QMessageBox::Ignore | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
        {
            e->ignore();
            return;
        }
        if (answer == QMessageBox::Discard)
            app_.jobs().cancel(current_);
    }
    e->accept();
}
