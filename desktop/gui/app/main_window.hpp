// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "archive_model.hpp"
#include "jobs.hpp"
#include "session_controller.hpp"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

#include <functional>
#include <optional>
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
class SessionBar;

// Full mode: archive browser and Inspector over a session of staged changes (new archive or
// edits to an open one), written in one step; extract and verify act on the archive on disk.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(App& app);

    void open_archive(const QString& path);
    // A new archive to be written to `target`, starting with `initial` at the top level.
    void start_new_archive(const QString& target, const QStringList& initial = {});
    // Stages files and folders under `folder` ("" = top level).
    void add_paths(const QStringList& paths, const QString& folder = {});
    // Create / Apply: returns the job id, or 0 if nothing can be written now.
    int apply_changes();

    [[nodiscard]] QString archive_path() const { return archive_; }
    [[nodiscard]] int visible_rows() const;
    [[nodiscard]] SessionController& session() { return *controller_; }
    [[nodiscard]] ArchiveModel& model() { return *model_; }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    // Where a browser entry comes from: a row of the session view and/or an archive entry.
    struct Ref
    {
        std::optional<std::size_t> row;
        std::optional<std::size_t> base;
    };

    void build_ui();
    void new_archive(const QStringList& given = {});
    void choose_and_add(bool folders);
    void drop_paths(const QStringList& paths, const QModelIndex& target);
    void extract(bool selected);
    void verify();
    void delete_selected();
    void rename_selected();
    void rename_entry(const QModelIndex& index, const QString& leaf);
    void move_selected();
    void new_folder();
    void save_as();
    // Leaves a new archive without creating it.
    void cancel_new();
    void open_entry(const QModelIndex& index);
    void show_empty();
    void update_recent();
    void update_inspector();
    void update_summary();
    void update_actions();
    void update_title();
    void on_session_updated();
    void load_base(const QString& path);
    void show_model(std::vector<BrowserEntry> entries, std::vector<Ref> refs);
    // Asks what to do with staged changes before `then`; false if the caller must not go on now.
    bool confirm_leave(const std::function<void()>& then);
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] satchel_gui::SessionOptions session_options() const;
    [[nodiscard]] std::vector<std::size_t> selected_positions() const;
    [[nodiscard]] std::vector<std::size_t> base_ids(const std::vector<std::size_t>& positions) const;
    [[nodiscard]] std::optional<satchel_gui::EntryKey> key_of(std::size_t position) const;
    [[nodiscard]] const satchel_gui::SessionRow* row_of(std::size_t position) const;
    [[nodiscard]] QString folder_for(const QModelIndex& index) const;
    [[nodiscard]] QStringList folders_in_session() const;
    QString extract_to_temp(const std::vector<std::size_t>& base);

    void on_started(int id, const QString& title);
    void on_progress(int id, quint64 done, quint64 total);
    void on_finished(int id, const QString& title, const JobOutcome& outcome);

    App& app_;
    SessionController* controller_ = nullptr;
    std::shared_ptr<const SessionSnapshot> snap_;
    int shown_generation_ = -1;
    bool base_stale_ = false;
    satchel_gui::Settings session_settings_;
    satchel_gui::ExistingEntries existing_ = satchel_gui::ExistingEntries::Ask;
    QString archive_; // the archive on disk (empty for a new archive)
    std::vector<zpp::EntryInfo> entries_; // its entries
    zpp::Stream stream_;
    zpp::Reader reader_;
    std::vector<Ref> refs_; // per browser entry
    QHash<std::size_t, std::size_t> base_to_row_;
    bool model_is_base_ = false;
    int commit_job_ = 0;
    std::function<void()> after_commit_;
    QString edit_after_update_;

    ArchiveModel* model_ = nullptr;
    ArchiveFilter* filter_ = nullptr;
    QStackedWidget* center_ = nullptr;
    ArchiveView* view_ = nullptr;
    SessionBar* bar_ = nullptr;
    QListWidget* recent_ = nullptr;
    QTextBrowser* inspector_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* method_filter_ = nullptr;
    QCheckBox* restorable_only_ = nullptr;
    QLabel* summary_ = nullptr;
    QListWidget* jobs_list_ = nullptr;
    QProgressBar* job_bar_ = nullptr;
    QLabel* jobs_label_ = nullptr;
    QHash<int, QString> verification_; // by archive entry
    QHash<int, QString> verified_at_;
    QList<int> mine_;
    QAction* tree_action_ = nullptr;
    QAction* show_deleted_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* go_ = nullptr;
    QAction* save_as_ = nullptr;
    QAction* discard_ = nullptr;
    QList<QAction*> edit_actions_;
    QList<QAction*> disk_actions_;
};
