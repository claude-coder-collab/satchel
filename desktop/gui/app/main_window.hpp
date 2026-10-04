// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "archive_model.hpp"
#include "jobs.hpp"

#include <QHash>
#include <QMainWindow>
#include <QStringList>

#include <functional>
#include <vector>

class App;
class ArchiveView;
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QStackedWidget;
class QTextBrowser;

// Full mode: archive browser, Inspector, plan review before every build, editing, verify.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(App& app);

    void open_archive(const QString& path);
    [[nodiscard]] QString archive_path() const { return archive_; }
    [[nodiscard]] int visible_rows() const;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void build_ui();
    void new_archive(const QStringList& given = {});
    void add_files(const QStringList& given = {});
    void extract(bool selected);
    void verify();
    void delete_selected();
    void rename_selected();
    void open_entry(const QModelIndex& index);
    void reload();
    void update_inspector();
    void update_summary();
    void update_recent();
    void show_empty();
    // Applies an editor operation and rewrites the archive in place as a job.
    void edit(const QString& title, const std::function<void(zp_editor_t*, const std::function<std::size_t(const std::string&)>&)>& op, const QStringList& add_inputs = {});
    std::vector<std::size_t> selected_entries() const;
    QString extract_to_temp(const std::vector<std::size_t>& entries);

    void on_started(int id, const QString& title);
    void on_progress(int id, quint64 done, quint64 total);
    void on_finished(int id, const QString& title, const JobOutcome& outcome);

    App& app_;
    QString archive_;
    std::vector<zpp::EntryInfo> entries_;
    zpp::Stream stream_;
    zpp::Reader reader_;
    ArchiveModel* model_ = nullptr;
    ArchiveFilter* filter_ = nullptr;
    QStackedWidget* center_ = nullptr;
    ArchiveView* view_ = nullptr;
    QListWidget* recent_ = nullptr;
    QTextBrowser* inspector_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* method_filter_ = nullptr;
    QCheckBox* restorable_only_ = nullptr;
    QLabel* summary_ = nullptr;
    QListWidget* jobs_list_ = nullptr;
    QProgressBar* job_bar_ = nullptr;
    QLabel* jobs_label_ = nullptr;
    QHash<int, QString> verification_;
    QHash<int, QString> verified_at_;
    QList<int> mine_;
    QAction* tree_action_ = nullptr;
};
