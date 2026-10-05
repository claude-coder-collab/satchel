// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "main_window.hpp"

#include "labels.hpp"

#include "updater.hpp"

#include "app.hpp"
#include "dialogs.hpp"
#include "settings_store.hpp"
#include "simple_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QMimeData>
#include <QProgressBar>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>

namespace
{

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString esc(const QString& s)
{
    return s.toHtmlEscaped();
}

QString size_text(std::uint64_t n)
{
    return qs(satchel_gui::human_size(n));
}

QString row_html(const QString& k, const QString& v)
{
    return QString("<tr><td style='color:palette(mid);padding-right:12px'>%1</td><td>%2</td></tr>").arg(esc(k), esc(v));
}

}

// Tree view that drags entries out as files (extracted to a temporary folder first).
class ArchiveView : public QTreeView
{
public:
    std::function<QString(const std::vector<std::size_t>&)> extract;

    using QTreeView::QTreeView;

protected:
    void startDrag(Qt::DropActions) override
    {
        std::vector<std::size_t> entries;
        QStringList tops;
        for (const auto& i : selectionModel()->selectedRows())
        {
            for (const auto& v : i.data(ArchiveModel::EntriesRole).toList())
                entries.push_back(static_cast<std::size_t>(v.toULongLong()));
            tops << i.data(Qt::DisplayRole).toString();
        }
        if (entries.empty() || !extract)
            return;
        const auto dir = extract(entries);
        if (dir.isEmpty())
            return;
        QList<QUrl> urls;
        for (const auto& name : QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
            urls << QUrl::fromLocalFile(QDir(dir).filePath(name));
        auto* mime = new QMimeData;
        mime->setUrls(urls);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
};

MainWindow::MainWindow(App& app) :
    app_(app)
{
    setWindowTitle(tr("Satchel"));
    setAcceptDrops(true);
    resize(1200, 760);
    build_ui();
    connect(&app_.jobs(), &JobQueue::started, this, &MainWindow::on_started);
    connect(&app_.jobs(), &JobQueue::progress, this, &MainWindow::on_progress);
    connect(&app_.jobs(), &JobQueue::finished, this, &MainWindow::on_finished);
    show_empty();
}

void MainWindow::build_ui()
{
    const auto action = [&](const QString& text, const QKeySequence& key, auto slot) {
        auto* a = new QAction(text, this);
        a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    auto* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->addAction(action(tr("New Archive"), QKeySequence::New, [this] { new_archive(); }));
    file_menu->addAction(action(tr("Open…"), QKeySequence::Open, [this] {
        const auto f = QFileDialog::getOpenFileName(this, tr("Open archive"), {}, tr("Zip archives (*.zip);;All files (*)"));
        if (!f.isEmpty())
            open_archive(f);
    }));
    file_menu->addSeparator();
    auto* settings = action(tr("Settings…"), QKeySequence::Preferences, [this] {
        SettingsDialog d(app_.settings(), this);
        if (d.exec() == QDialog::Accepted)
        {
            app_.settings() = d.settings();
            app_.save();
        }
    });
    settings->setMenuRole(QAction::PreferencesRole);
    file_menu->addAction(settings);

    auto* archive_menu = menuBar()->addMenu(tr("&Archive"));
    archive_menu->addAction(action(tr("Add Files"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [this] { add_files(); }));
    archive_menu->addSeparator();
    archive_menu->addAction(action(tr("Extract All"), QKeySequence(), [this] { extract(false); }));
    archive_menu->addAction(action(tr("Extract Selected"), QKeySequence(Qt::CTRL | Qt::Key_E), [this] { extract(true); }));
    archive_menu->addAction(action(tr("Verify"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), [this] { verify(); }));
    archive_menu->addSeparator();
    archive_menu->addAction(action(tr("Rename"), QKeySequence(Qt::Key_F2), [this] { rename_selected(); }));
    archive_menu->addAction(action(tr("Delete"), QKeySequence::Delete, [this] { delete_selected(); }));

    tree_action_ = action(tr("Tree View"), QKeySequence(), [this] { model_->set_tree(tree_action_->isChecked()); });
    tree_action_->setCheckable(true);
    auto* view_menu = menuBar()->addMenu(tr("&View"));
    view_menu->addAction(tree_action_);
    view_menu->addAction(action(tr("Switch to Simple Mode"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M), [this] { app_.toggle_mode(); }));
    if (app_.updater() && app_.updater()->available())
    {
        auto* help_menu = menuBar()->addMenu(tr("&Help"));
        auto* check = help_menu->addAction(tr("Check for Updates…"));
        check->setMenuRole(QAction::ApplicationSpecificRole);
        connect(check, &QAction::triggered, this, [this] { app_.updater()->check_now(); });
    }

    model_ = new ArchiveModel(this);
    filter_ = new ArchiveFilter(this);
    filter_->setSourceModel(model_);
    filter_->setSortRole(ArchiveModel::SortRole);
    filter_->setRecursiveFilteringEnabled(false);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    auto* filters = new QHBoxLayout;
    search_ = new QLineEdit(central);
    search_->setPlaceholderText(tr("Search names"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search"));
    auto* find = new QAction(this);
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered, search_, [this] { search_->setFocus(); });
    addAction(find);
    method_filter_ = new QComboBox(central);
    method_filter_->addItem(tr("All methods"), QString());
    method_filter_->addItem(tr("Store"), "store");
    method_filter_->addItem(tr("Deflate"), "deflate");
    method_filter_->addItem(tr("FLAC"), "flac");
    method_filter_->addItem(tr("FLAC multi-mono"), "flac-mono");
    method_filter_->setAccessibleName(tr("Method filter"));
    restorable_only_ = new QCheckBox(tr("Restorable only"), central);
    filters->addWidget(search_, 1);
    filters->addWidget(method_filter_);
    filters->addWidget(restorable_only_);
    layout->addLayout(filters);
    connect(search_, &QLineEdit::textChanged, filter_, &ArchiveFilter::set_text);
    connect(method_filter_, &QComboBox::currentIndexChanged, this, [this] { filter_->set_method(method_filter_->currentData().toString()); });
    connect(restorable_only_, &QCheckBox::toggled, filter_, &ArchiveFilter::set_restorable_only);

    center_ = new QStackedWidget(central);
    auto* empty = new QWidget(center_);
    auto* empty_layout = new QVBoxLayout(empty);
    auto* drop = new QLabel(tr("Drop a .zip here to open it, or files to create a new archive."), empty);
    drop->setAlignment(Qt::AlignCenter);
    drop->setStyleSheet("QLabel { border: 2px dashed palette(mid); border-radius: 12px; padding: 40px; }");
    recent_ = new QListWidget(empty);
    recent_->setAccessibleName(tr("Recent archives"));
    connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { open_archive(item->data(Qt::UserRole).toString()); });
    empty_layout->addWidget(drop, 1);
    empty_layout->addWidget(new QLabel(tr("Recent archives"), empty));
    empty_layout->addWidget(recent_, 1);
    center_->addWidget(empty);

    auto* split = new QSplitter(center_);
    view_ = new ArchiveView(split);
    view_->setModel(filter_);
    view_->header()->setSortIndicator(-1, Qt::AscendingOrder);
    view_->setSortingEnabled(true);
    view_->setUniformRowHeights(true);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setDragEnabled(true);
    view_->setAlternatingRowColors(true);
    view_->setAccessibleName(tr("Archive entries"));
    view_->header()->setStretchLastSection(false);
    view_->header()->setSectionResizeMode(ArchiveModel::Name, QHeaderView::Stretch);
    view_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->extract = [this](const std::vector<std::size_t>& e) { return extract_to_temp(e); };
    connect(view_->header(), &QHeaderView::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu;
        for (int c = 1; c < ArchiveModel::ColumnCount; ++c)
        {
            auto* a = menu.addAction(model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
            a->setCheckable(true);
            a->setChecked(!view_->header()->isSectionHidden(c));
            connect(a, &QAction::toggled, this, [this, c](bool on) {
                view_->header()->setSectionHidden(c, !on);
                QSettings().setValue("browser/header", view_->header()->saveState());
            });
        }
        menu.exec(view_->header()->mapToGlobal(pos));
    });
    view_->header()->restoreState(QSettings().value("browser/header").toByteArray());
    connect(view_, &QTreeView::doubleClicked, this, &MainWindow::open_entry);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::update_inspector);
    inspector_ = new QTextBrowser(split);
    inspector_->setAccessibleName(tr("Inspector"));
    inspector_->setOpenLinks(false);
    split->addWidget(view_);
    split->addWidget(inspector_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    center_->addWidget(split);
    layout->addWidget(center_, 1);
    setCentralWidget(central);

    summary_ = new QLabel(this);
    statusBar()->addWidget(summary_, 1);
    job_bar_ = new QProgressBar(this);
    job_bar_->setRange(0, 1000);
    job_bar_->setMaximumWidth(220);
    job_bar_->hide();
    statusBar()->addPermanentWidget(job_bar_);
    jobs_label_ = new QLabel(tr("No jobs"), this);
    statusBar()->addPermanentWidget(jobs_label_);

    auto* dock = new QDockWidget(tr("Jobs"), this);
    dock->setObjectName("jobs-dock");
    jobs_list_ = new QListWidget(dock);
    jobs_list_->setAccessibleName(tr("Jobs"));
    connect(jobs_list_, &QListWidget::itemActivated, this, [](QListWidgetItem* item) {
        const auto out = item->data(Qt::UserRole).toString();
        if (!out.isEmpty())
            reveal_in_file_manager(out);
    });
    auto* cancel = new QAction(tr("Cancel Job"), jobs_list_);
    connect(cancel, &QAction::triggered, this, [this] {
        if (auto* item = jobs_list_->currentItem())
            app_.jobs().cancel(item->data(Qt::UserRole + 1).toInt());
    });
    jobs_list_->addAction(cancel);
    jobs_list_->setContextMenuPolicy(Qt::ActionsContextMenu);
    dock->setWidget(jobs_list_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->hide();
    view_menu->addAction(dock->toggleViewAction());
}

void MainWindow::show_empty()
{
    center_->setCurrentIndex(0);
    inspector_->clear();
    summary_->setText(tr("No archive open"));
    update_recent();
}

void MainWindow::update_recent()
{
    recent_->clear();
    for (const auto& p : QSettings().value("recent").toStringList())
    {
        auto* item = new QListWidgetItem(QFileInfo(p).fileName() + "  —  " + QFileInfo(p).absolutePath(), recent_);
        item->setData(Qt::UserRole, p);
    }
}

int MainWindow::visible_rows() const
{
    return filter_->rowCount();
}

void MainWindow::open_archive(const QString& path)
{
    try
    {
        zpp::Stream stream(zpp::not_null(zp_stream_open_file(path.toStdString().c_str())));
        zpp::Reader reader(zpp::not_null(zp_reader_open(app_context(), stream.get())));
        reader_ = std::move(reader);
        stream_ = std::move(stream);
    } catch (const zpp::Error& e)
    {
        QMessageBox::critical(this, tr("Cannot open"), tr("%1\n\n%2").arg(path, QString::fromUtf8(e.what())));
        return;
    }
    archive_ = path;
    verification_.clear();
    verified_at_.clear();
    auto recent = QSettings().value("recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    QSettings().setValue("recent", recent.mid(0, 10));
    reload();
}

void MainWindow::reload()
{
    entries_ = zpp::reader_entries(reader_.get());
    model_->set_entries(entries_);
    model_->set_verification(verification_);
    center_->setCurrentIndex(1);
    setWindowTitle(tr("%1 — Satchel").arg(QFileInfo(archive_).fileName()));
    update_summary();
    update_inspector();
}

void MainWindow::update_summary()
{
    std::uint64_t size = 0;
    std::uint64_t packed = 0;
    QMap<QString, int> methods;
    for (const auto& e : entries_)
    {
        size += e.uncompressed_size;
        packed += e.compressed_size;
        if (e.kind == ZP_KIND_FILE)
            methods[e.flac_channel ? "FLAC multi-mono" : e.flac_restorable ? "FLAC"
                    : e.method == ZP_METHOD_DEFLATE                        ? "Deflate"
                    : e.method == ZP_METHOD_STORE                          ? "Store"
                                                                           : "Other"]++;
    }
    std::array<char, 256> version{};
    const bool ours = zp_reader_get_app_version(reader_.get(), version.data(), version.size()) == ZP_OK;
    QStringList counts;
    for (auto it = methods.begin(); it != methods.end(); ++it)
        counts << QString("%1 %2").arg(it.value()).arg(labels::method(it.key().toStdString()));
    summary_->setText(tr("Created by %1 · %2 · %3 · Zip64 %4")
            .arg(ours ? QString::fromUtf8(version.data()) : tr("unknown tool"))
            .arg(labels::savings(size, packed))
            .arg(counts.join(", "))
            .arg(zp_reader_zip64(reader_.get()) ? tr("yes") : tr("no")));
}

std::vector<std::size_t> MainWindow::selected_entries() const
{
    std::vector<std::size_t> out;
    for (const auto& i : view_->selectionModel()->selectedRows())
    {
        for (const auto& v : i.data(ArchiveModel::EntriesRole).toList())
            out.push_back(static_cast<std::size_t>(v.toULongLong()));
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

void MainWindow::update_inspector()
{
    if (!reader_)
        return;
    const auto rows = view_->selectionModel()->selectedRows();
    const auto ids = selected_entries();
    if (ids.empty())
    {
        inspector_->setHtml(tr("<p style='color:gray'>Select an entry to see its details.</p>"));
        return;
    }
    if (rows.size() > 1)
    {
        std::uint64_t size = 0;
        std::uint64_t packed = 0;
        for (const auto i : ids)
        {
            size += entries_[i].uncompressed_size;
            packed += entries_[i].compressed_size;
        }
        inspector_->setHtml(QString("<h3>%1</h3><table>%2%3%4</table>")
                .arg(tr("%1 items selected").arg(rows.size()))
                .arg(row_html(tr("Entries"), QString::number(ids.size())))
                .arg(row_html(tr("Size"), size_text(size)))
                .arg(row_html(tr("Saved"), labels::savings(size, packed))));
        return;
    }
    const auto& e = entries_[ids.front()];
    QString html = QString("<h3>%1</h3><table>").arg(esc(rows.front().data(Qt::DisplayRole).toString()));
    html += row_html(tr("Name"), qs(e.name));
    html += row_html(tr("Size"), size_text(e.uncompressed_size));
    html += row_html(tr("Packed"), size_text(e.compressed_size));
    html += row_html(tr("Ratio"), QString("%1% saved").arg(satchel_gui::percent_saved(e.uncompressed_size, e.compressed_size)));
    html += row_html(tr("Method"), rows.front().sibling(rows.front().row(), ArchiveModel::Method).data().toString());
    html += row_html(tr("CRC-32"), QString("%1").arg(e.crc32, 8, 16, QChar('0')));
    html += row_html(tr("Modified"), QDateTime::fromSecsSinceEpoch(e.mtime).toString(Qt::ISODate));
    if (e.unix_mode)
        html += row_html(tr("Permissions"), QString::number(*e.unix_mode, 8));
    html += "</table>";

    char* json = e.flac_restorable || QFileInfo(qs(e.name)).suffix().compare("flac", Qt::CaseInsensitive) == 0 ? zp_reader_flac_describe(reader_.get(), e.index) : nullptr;
    if (json)
    {
        const auto doc = QJsonDocument::fromJson(QByteArray(json));
        zp_free(json);
        const auto f = doc.object();
        const auto rate = static_cast<std::uint32_t>(f["sample_rate"].toInteger());
        const auto samples = static_cast<std::uint64_t>(f["total_samples"].toInteger());
        const auto channels = ids.size() > 1 ? static_cast<int>(ids.size()) : f["channels"].toInt();
        html += "<h4>" + tr("Audio") + "</h4><table>";
        html += row_html(tr("Sample rate"), QString("%1 Hz").arg(rate));
        html += row_html(tr("Bit depth"), QString("%1-bit").arg(f["bits_per_sample"].toInt()));
        html += row_html(tr("Channels"), QString::number(channels));
        html += row_html(tr("Duration"), rate ? qs(satchel_gui::format_timecode(samples, rate)) : QString());
        if (f.contains("layout"))
            html += row_html(tr("Layout"), f["layout"].toString() == "standard" ? tr("standard") : f["layout"].toString() == "multi_mono" ? tr("multi-mono")
                                                                                                                                          : tr("private"));
        html += "</table>";
        if (f.contains("original_name"))
        {
            html += "<h4>" + tr("Original") + "</h4><table>";
            html += row_html(tr("File name"), f["original_name"].toString());
            html += row_html(tr("Container"), f["container"].toString());
            html += row_html(tr("SHA-256"), f["sha256"].toString());
            html += row_html(tr("Restorable with flac -d"), f["restorable_with_flac_tool"].toBool() ? tr("yes") : tr("no"));
            html += "</table>";
        }
        QMap<QString, QString> tags;
        QStringList tracks;
        for (const auto& t : f["tags"].toArray())
        {
            const auto kv = t.toArray();
            const auto key = kv[0].toString().toUpper();
            if (key.startsWith("TRACK_NAME"))
                tracks << QString("%1: %2").arg(key.mid(11).isEmpty() ? QString::number(e.flac_channel ? e.flac_channel->first : 1) : key.mid(11), kv[1].toString());
            else
                tags[key] = kv[1].toString();
        }
        if (!tags.isEmpty())
        {
            html += "<h4>" + tr("Production metadata") + "</h4><table>";
            if (tags.contains("TIME_REFERENCE") && rate)
                html += row_html(tr("Timecode"), qs(satchel_gui::format_timecode(tags["TIME_REFERENCE"].toULongLong(), rate, tags.value("TIMECODE_RATE").toStdString())));
            for (const auto* k : { "SCENE", "TAKE", "TAPE", "PROJECT", "NOTE", "CIRCLED", "ORIGINATOR", "DESCRIPTION", "TITLE", "ARTIST", "COMMENT" })
            {
                if (tags.contains(k))
                    html += row_html(QString(k).toLower(), tags[k]);
            }
            html += "</table>";
        }
        if (!tracks.isEmpty())
            html += "<h4>" + tr("Tracks") + "</h4><p>" + esc(tracks.join(" · ")) + "</p>";
        const auto chunks = f["chunks"].toArray();
        if (!chunks.isEmpty())
        {
            html += "<h4>" + tr("Chunks") + "</h4><table>";
            for (const auto& c : chunks)
            {
                const auto o = c.toObject();
                html += row_html(o["id"].toString() + (o["known"].toBool() ? "" : tr(" (unknown)")),
                    o["audio"].toBool() ? tr("audio") : QString("%1 bytes%2").arg(o["size"].toInteger()).arg(o["after_audio"].toBool() ? tr(", after audio") : QString()));
            }
            html += "</table>";
        }
    }
    html += "<h4>" + tr("Verification") + "</h4><p>";
    const auto v = verification_.value(static_cast<int>(e.index));
    html += v.isEmpty() ? tr("Not verified") : esc(QString("%1 (%2)").arg(v, verified_at_.value(static_cast<int>(e.index))));
    html += "</p>";
    inspector_->setHtml(html);
}

void MainWindow::create_blank_archive()
{
    auto path = QFileDialog::getSaveFileName(this, tr("Create blank archive"), {}, tr("Zip archives (*.zip)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += ".zip";
    static constexpr char empty_zip[22] = { 'P', 'K', 5, 6 };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(empty_zip, sizeof empty_zip) != sizeof empty_zip || !file.commit())
    {
        QMessageBox::critical(this, tr("Cannot create archive"), tr("%1\n\n%2").arg(path, file.errorString()));
        return;
    }
    open_archive(path);
}

void MainWindow::new_archive(const QStringList& given)
{
    QStringList inputs = given;
    if (inputs.isEmpty())
    {
        QMessageBox ask(QMessageBox::Question, tr("New archive"), tr("How do you want to start the new archive?"), QMessageBox::Cancel, this);
        ask.setInformativeText(tr("Blank archive: create an empty archive, then add files to it.\nCompress a folder: choose a folder and review how it will be compressed."));
        auto* blank = ask.addButton(tr("Blank Archive…"), QMessageBox::AcceptRole);
        auto* folder = ask.addButton(tr("Compress a Folder…"), QMessageBox::AcceptRole);
        ask.exec();
        if (ask.clickedButton() == blank)
        {
            create_blank_archive();
            return;
        }
        if (ask.clickedButton() == folder)
        {
            const auto dir = QFileDialog::getExistingDirectory(this, tr("Choose a folder to compress"));
            if (!dir.isEmpty())
                inputs << dir;
        }
        if (inputs.isEmpty())
            return;
    }
    std::vector<std::filesystem::path> items;
    items.reserve(static_cast<std::size_t>(inputs.size()));
    for (const auto& i : inputs)
        items.emplace_back(i.toStdU16String());
    const auto exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    };
    const auto output = QString::fromStdU16String(satchel_gui::compress_output(items, std::nullopt, exists).u16string());
    PlanReviewDialog review(inputs, output, app_.settings(), this);
    if (review.exec() != QDialog::Accepted)
        return;
    auto plan = review.take_plan();
    const auto out = review.output();
    const auto settings = review.settings();
    mine_ << app_.jobs().enqueue(tr("Compressing to %1").arg(QFileInfo(out).fileName()), [plan, out, settings](JobContext& jc) {
        auto o = compress_job(jc, plan->get(), out, settings);
        if (o.status == ZP_OK && settings.verify_after_build)
        {
            auto v = verify_job(jc, out);
            o.summary += QString(" · %1").arg(v.summary);
            o.details << v.details;
        }
        return o;
    });
}

void MainWindow::edit(const QString& title, const std::function<void(zp_editor_t*, const std::function<std::size_t(const std::string&)>&)>& op, const QStringList& add_inputs)
{
    if (archive_.isEmpty())
        return;
    const auto path = archive_;
    // Release our reader so the archive can be replaced, then do the work as a job.
    reader_.reset();
    stream_.reset();
    std::vector<std::string> adds;
    for (const auto& a : add_inputs)
        adds.push_back(a.toStdString());
    mine_ << app_.jobs().enqueue(title, [path, op, adds](JobContext& jc) {
        JobOutcome o;
        o.output = path;
        zpp::Stream in(zpp::not_null(zp_stream_open_file(path.toStdString().c_str())));
        zpp::Editor editor(zpp::not_null(zp_editor_open(app_context(), in.get())));
        zpp::Input input;
        const auto index_of = [&](const std::string& name) -> std::size_t {
            zpp::Plan view(zpp::not_null(zp_editor_plan(editor.get())));
            for (const auto& e : zpp::plan_entries(view.get()))
            {
                if (e.source_path == "archive:" + name)
                    return e.index;
            }
            throw zpp::Error(ZP_INVALID_ARGUMENT, "entry not found: " + name);
        };
        op(editor.get(), index_of);
        if (!adds.empty())
        {
            input = zpp::input_from_paths(adds);
            zpp::check(zp_editor_add(editor.get(), input.get()));
        }
        zpp::Plan view(zpp::not_null(zp_editor_plan(editor.get())));
        if (!zp_plan_executable(view.get()))
        {
            o.status = ZP_CONFLICTS_UNRESOLVED;
            for (const auto& c : zpp::plan_conflicts(view.get()))
                o.details << QString::fromStdString(c.key);
            o.message = QObject::tr("The change would create name collisions");
            return o;
        }
        zpp::Stream out(zpp::not_null(zp_stream_create_file(path.toStdString().c_str())));
        zpp::ProgressAdapter adapter{ jc.progress };
        o.status = zp_editor_commit(editor.get(), out.get(), zpp::ProgressAdapter::call, &adapter);
        o.message = QString::fromUtf8(zp_last_error());
        if (o.status == ZP_OK)
        {
            editor.reset();
            in.reset();
            zpp::check(zp_stream_commit(out.get()));
            o.summary = QObject::tr("Archive updated");
        }
        return o;
    });
}

void MainWindow::add_files(const QStringList& given)
{
    if (archive_.isEmpty())
    {
        new_archive(given);
        return;
    }
    QStringList inputs = given;
    if (inputs.isEmpty())
        inputs = QFileDialog::getOpenFileNames(this, tr("Add files to %1").arg(QFileInfo(archive_).fileName()));
    if (inputs.isEmpty())
        return;
    // Same review as a new archive, for names, methods and warnings of the additions.
    PlanReviewDialog review(inputs, archive_, app_.settings(), this);
    review.setWindowTitle(tr("Review the additions"));
    if (review.exec() != QDialog::Accepted)
        return;
    edit(tr("Adding to %1").arg(QFileInfo(archive_).fileName()), [](zp_editor_t*, const auto&) {}, inputs);
}

void MainWindow::delete_selected()
{
    const auto ids = selected_entries();
    if (ids.empty())
        return;
    if (QMessageBox::question(this, tr("Delete"), tr("Remove %1 entr(ies) from the archive?").arg(ids.size())) != QMessageBox::Yes)
        return;
    std::vector<std::string> names;
    names.reserve(ids.size());
    for (const auto i : ids)
        names.push_back(entries_[i].name);
    edit(tr("Deleting from %1").arg(QFileInfo(archive_).fileName()), [names](zp_editor_t* ed, const auto& index_of) {
        for (const auto& n : names)
            zpp::check(zp_editor_remove(ed, index_of(n)));
    });
}

void MainWindow::rename_selected()
{
    const auto rows = view_->selectionModel()->selectedRows();
    if (rows.size() != 1)
        return;
    const auto ids = selected_entries();
    const bool group = ids.size() > 1;
    const auto& first = entries_[ids.front()];
    const auto current = group ? qs(first.name).section('_', 0, -2) : qs(first.name);
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("Rename"), group ? tr("New name for the channel files (without _chNN.flac):") : tr("New name:"), QLineEdit::Normal, current, &ok);
    if (!ok || name.isEmpty() || name == current)
        return;
    std::vector<std::pair<std::string, std::string>> renames;
    for (const auto i : ids)
    {
        const auto& e = entries_[i];
        if (group && e.flac_channel)
        {
            const auto width = std::max<int>(2, static_cast<int>(QString::number(e.flac_channel->second).size()));
            renames.emplace_back(e.name, QString("%1_ch%2.flac").arg(name).arg(e.flac_channel->first, width, 10, QChar('0')).toStdString());
        }
        else
            renames.emplace_back(e.name, name.toStdString());
    }
    edit(tr("Renaming in %1").arg(QFileInfo(archive_).fileName()), [renames](zp_editor_t* ed, const auto& index_of) {
        for (const auto& [from, to] : renames)
            zpp::check(zp_editor_rename(ed, index_of(from), to.c_str()));
    });
}

void MainWindow::extract(bool selected)
{
    if (archive_.isEmpty())
        return;
    std::vector<std::size_t> selection;
    if (selected)
    {
        selection = selected_entries();
        if (selection.empty())
            return;
    }
    const auto dest = QFileInfo(archive_).absolutePath() + "/" + QFileInfo(archive_).completeBaseName();
    ExtractDialog dialog(archive_, dest, app_.settings(), selection, this);
    const auto choice = dialog.run();
    if (!choice)
        return;
    const auto path = archive_;
    mine_ << app_.jobs().enqueue(tr("Extracting %1").arg(QFileInfo(archive_).fileName()), [path, choice = *choice, selection](JobContext& jc) {
        return extract_job(jc, path, choice.destination, choice.settings, selection, choice.replace_existing);
    });
}

void MainWindow::verify()
{
    if (archive_.isEmpty())
        return;
    const auto path = archive_;
    mine_ << app_.jobs().enqueue(tr("Verifying %1").arg(QFileInfo(archive_).fileName()), [path](JobContext& jc) { return verify_job(jc, path); });
}

QString MainWindow::extract_to_temp(const std::vector<std::size_t>& entries)
{
    auto dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QString("/satchel-%1").arg(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(dir);
    QProgressDialog progress(tr("Preparing files…"), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    JobContext jc;
    std::atomic<bool> cancelled{ false };
    jc.cancelled = &cancelled;
    jc.progress = [&](std::uint64_t done, std::uint64_t total) {
        progress.setValue(total ? static_cast<int>(1000.0 * static_cast<double>(done) / static_cast<double>(total)) : 0);
        QApplication::processEvents();
        return !progress.wasCanceled();
    };
    try
    {
        const auto outcome = extract_job(jc, archive_, dir, app_.settings(), entries, true);
        if (outcome.status != ZP_OK)
        {
            QMessageBox::warning(this, tr("Cannot extract"), outcome.message);
            return {};
        }
    } catch (const zpp::Error& e)
    {
        QMessageBox::warning(this, tr("Cannot extract"), QString::fromUtf8(e.what()));
        return {};
    }
    return dir;
}

void MainWindow::open_entry(const QModelIndex& index)
{
    if (index.data(ArchiveModel::FolderRole).toBool())
        return;
    std::vector<std::size_t> entries;
    for (const auto& v : index.data(ArchiveModel::EntriesRole).toList())
        entries.push_back(static_cast<std::size_t>(v.toULongLong()));
    const auto dir = extract_to_temp(entries);
    if (dir.isEmpty())
        return;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext())
        QDesktopServices::openUrl(QUrl::fromLocalFile(it.next()));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls() && e->source() != view_)
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    QStringList paths;
    for (const auto& url : e->mimeData()->urls())
    {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    if (paths.isEmpty())
        return;
    if (archive_.isEmpty() && paths.size() == 1 && satchel_gui::is_zip(std::filesystem::path(paths.front().toStdU16String())))
        open_archive(paths.front());
    else
        add_files(paths);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (app_.jobs().busy())
    {
        const auto answer = QMessageBox::question(this, tr("A job is running"), tr("Cancel running jobs, or keep them running in the background?"), QMessageBox::Discard | QMessageBox::Ignore | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
        {
            e->ignore();
            return;
        }
        if (answer == QMessageBox::Discard)
        {
            for (const auto id : mine_)
                app_.jobs().cancel(id);
        }
    }
    e->accept();
}

void MainWindow::on_started(int id, const QString& title)
{
    auto* item = new QListWidgetItem(tr("Running: %1").arg(title), jobs_list_);
    item->setData(Qt::UserRole + 1, id);
    jobs_label_->setText(title);
    job_bar_->setValue(0);
    job_bar_->show();
}

void MainWindow::on_progress(int, quint64 done, quint64 total)
{
    job_bar_->setValue(total ? static_cast<int>(1000.0 * static_cast<double>(done) / static_cast<double>(total)) : 0);
}

void MainWindow::on_finished(int id, const QString& title, const JobOutcome& outcome)
{
    job_bar_->hide();
    jobs_label_->setText(app_.jobs().busy() ? tr("Jobs running") : tr("No jobs running"));
    const bool ok = outcome.status == ZP_OK || outcome.status == ZP_SOURCE_CHANGED;
    for (int i = 0; i < jobs_list_->count(); ++i)
    {
        auto* item = jobs_list_->item(i);
        if (item->data(Qt::UserRole + 1).toInt() == id)
        {
            item->setText(QString("%1 %2 — %3").arg(ok ? "✔" : "✖", title, ok ? outcome.summary : outcome.message));
            item->setData(Qt::UserRole, outcome.output);
            item->setToolTip(outcome.details.join('\n'));
        }
    }
    const bool was_mine = mine_.removeOne(id);
    if (!outcome.entry_status.isEmpty())
    {
        const auto when = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
        for (const auto& [entry, status] : outcome.entry_status)
        {
            verification_[entry] = status;
            verified_at_[entry] = when;
        }
        model_->set_verification(verification_);
        update_inspector();
    }
    // Reopen after an edit, or open a freshly built archive.
    if (was_mine && ok && !outcome.output.isEmpty() && outcome.output.endsWith(".zip", Qt::CaseInsensitive) && (archive_.isEmpty() || outcome.output == archive_))
        open_archive(outcome.output);
    else if (!reader_ && !archive_.isEmpty())
        open_archive(archive_);
    if (was_mine && !ok && outcome.status != ZP_CANCELLED)
    {
        QMessageBox box(QMessageBox::Warning, title, outcome.message, QMessageBox::Ok, this);
        box.setDetailedText(outcome.details.join('\n'));
        box.exec();
    }
    if (was_mine && !isActiveWindow())
        app_.notify(title, ok ? outcome.summary : outcome.message);
}
