// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "dialogs.hpp"

#include "jobs.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

using satchel_gui::Settings;

namespace
{

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

std::filesystem::path to_path(const QString& s)
{
    return { s.toStdU16String() };
}

QString method_text(const zpp::PlanEntry& e)
{
    switch (e.codec)
    {
        case ZP_CODEC_FLAC:
            return QObject::tr("FLAC");
        case ZP_CODEC_FLAC_MONO:
            return QObject::tr("FLAC (channel %1 of %2)").arg(e.channel_index).arg(e.channel_count);
        case ZP_CODEC_GENERATED:
            return QObject::tr("Readme");
        default:
            break;
    }
    if (e.kind == ZP_KIND_DIRECTORY)
        return QObject::tr("Folder");
    if (e.fallback_reason >= 0)
        return QObject::tr("Stored");
    return QObject::tr("Deflate");
}

QString reason_text(const zpp::PlanEntry& e)
{
    if (e.fallback_reason >= 0)
        return QObject::tr("Stored: %1").arg(qs(e.fallback_detail));
    if (e.codec == ZP_CODEC_FLAC || e.codec == ZP_CODEC_FLAC_MONO)
        return QObject::tr("Restores to %1").arg(qs(e.restored_name));
    if (e.codec == ZP_CODEC_GENERATED)
        return QObject::tr("Explains how to restore the audio");
    return {};
}

}

std::shared_ptr<zpp::Plan> PlanReviewDialog::make_plan(const QStringList& inputs, const Settings& settings)
{
    std::vector<std::string> paths;
    for (const auto& i : inputs)
        paths.push_back(i.toStdString());
    auto input = zpp::input_from_paths(paths);
    zp_plan_options_t options{};
    zp_plan_options_init(&options);
    options.flac_enabled = settings.flac ? 1 : 0;
    options.deflate_level = settings.deflate_level;
    options.flac_level = settings.flac_level;
    auto plan = std::make_shared<zpp::Plan>(zpp::not_null(zp_plan_create(app_context(), input.get(), &options)));
    return plan;
}

PlanReviewDialog::PlanReviewDialog(QStringList inputs, const QString& output, Settings settings, QWidget* parent) :
    QDialog(parent),
    inputs_(std::move(inputs)),
    settings_(std::move(settings))
{
    setWindowTitle(tr("Review the new archive"));
    resize(900, 640);
    auto* layout = new QVBoxLayout(this);

    totals_ = new QLabel(this);
    layout->addWidget(totals_);

    issues_ = new QWidget(this);
    issues_layout_ = new QVBoxLayout(issues_);
    issues_layout_->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(issues_);
    scroll->setMaximumHeight(220);
    layout->addWidget(scroll);

    warnings_ = new QListWidget(this);
    warnings_->setMaximumHeight(90);
    warnings_->setAccessibleName(tr("Warnings"));
    layout->addWidget(warnings_);

    model_ = new QStandardItemModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAccessibleName(tr("Planned entries"));
    layout->addWidget(table_, 1);

    auto* options_toggle = new QToolButton(this);
    options_toggle->setText(tr("Options"));
    options_toggle->setCheckable(true);
    options_toggle->setArrowType(Qt::RightArrow);
    options_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* options = new QGroupBox(this);
    options->setVisible(false);
    auto* form = new QFormLayout(options);
    flac_ = new QCheckBox(tr("Convert PCM audio to FLAC"), options);
    flac_->setChecked(settings_.flac);
    deflate_ = new QSpinBox(options);
    deflate_->setRange(1, 9);
    deflate_->setValue(settings_.deflate_level);
    flac_level_ = new QSpinBox(options);
    flac_level_->setRange(0, 8);
    flac_level_->setValue(settings_.flac_level);
    form->addRow(flac_);
    form->addRow(tr("Deflate level"), deflate_);
    form->addRow(tr("FLAC level"), flac_level_);
    connect(options_toggle, &QToolButton::toggled, this, [options, options_toggle](bool on) {
        options->setVisible(on);
        options_toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    for (auto* w : { static_cast<QWidget*>(flac_), static_cast<QWidget*>(deflate_), static_cast<QWidget*>(flac_level_) })
    {
        if (auto* box = qobject_cast<QCheckBox*>(w))
            connect(box, &QCheckBox::toggled, this, &PlanReviewDialog::replan);
        if (auto* spin = qobject_cast<QSpinBox*>(w))
            connect(spin, &QSpinBox::editingFinished, this, &PlanReviewDialog::replan);
    }
    layout->addWidget(options_toggle);
    layout->addWidget(options);

    auto* out_row = new QHBoxLayout;
    output_ = new QLineEdit(output, this);
    output_->setAccessibleName(tr("Archive file"));
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto f = QFileDialog::getSaveFileName(this, tr("Save archive as"), output_->text(), tr("Zip archives (*.zip)"));
        if (!f.isEmpty())
            output_->setText(f);
    });
    out_row->addWidget(new QLabel(tr("Save as"), this));
    out_row->addWidget(output_, 1);
    out_row->addWidget(browse);
    layout->addLayout(out_row);

    auto* buttons = new QDialogButtonBox(this);
    auto* copy = buttons->addButton(tr("Copy as CLI command"), QDialogButtonBox::HelpRole);
    build_ = buttons->addButton(tr("Build"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(copy, &QPushButton::clicked, this, [this] {
        std::vector<std::filesystem::path> in;
        for (const auto& i : inputs_)
            in.push_back(to_path(i));
        QApplication::clipboard()->setText(qs(satchel_gui::cli_create_command(settings_, to_path(output_->text()), in)));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    replan();
}

QString PlanReviewDialog::output() const
{
    return output_->text();
}

void PlanReviewDialog::replan()
{
    settings_.flac = flac_->isChecked();
    settings_.deflate_level = deflate_->value();
    settings_.flac_level = flac_level_->value();
    try
    {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        plan_ = make_plan(inputs_, settings_);
        QApplication::restoreOverrideCursor();
    } catch (const zpp::Error& e)
    {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Cannot plan the archive"), QString::fromUtf8(e.what()));
        plan_ = nullptr;
    }
    refresh();
}

void PlanReviewDialog::resolve(zp_resolution_t r, const std::string& name_storage)
{
    r.new_name = r.action == ZP_RESOLVE_RENAME ? name_storage.c_str() : nullptr;
    if (zp_plan_resolve(plan_->get(), &r, 1) != ZP_OK)
        QMessageBox::warning(this, tr("Cannot apply"), QString::fromUtf8(zp_last_error()));
    refresh();
}

void PlanReviewDialog::refresh()
{
    while (auto* item = issues_layout_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    warnings_->clear();
    model_->clear();
    if (!plan_)
    {
        build_->setEnabled(false);
        return;
    }
    const auto entries = zpp::plan_entries(plan_->get());
    const auto conflicts = zpp::plan_conflicts(plan_->get());
    std::size_t files = 0;
    std::size_t flac = 0;
    std::size_t deflate = 0;
    std::size_t stored = 0;
    model_->setHorizontalHeaderLabels({ tr("Name in archive"), tr("Size"), tr("Method"), tr("Reason") });
    for (const auto& e : entries)
    {
        if (e.kind == ZP_KIND_FILE && e.codec != ZP_CODEC_GENERATED)
            ++files;
        (e.codec == ZP_CODEC_FLAC || e.codec == ZP_CODEC_FLAC_MONO ? flac : e.fallback_reason >= 0 ? stored
                : e.kind == ZP_KIND_FILE                                                           ? deflate
                                                                                                   : stored) += e.kind == ZP_KIND_FILE ? 1 : 0;
        auto* size = new QStandardItem(e.kind == ZP_KIND_DIRECTORY ? QString() : qs(satchel_gui::human_size(e.size)));
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        model_->appendRow({ new QStandardItem(qs(e.output_name)), size, new QStandardItem(method_text(e)), new QStandardItem(reason_text(e)) });
    }
    table_->resizeColumnsToContents();
    totals_->setText(tr("%1 file(s), %2 · %3 FLAC, %4 deflate or store, %5 stored")
            .arg(files)
            .arg(qs(satchel_gui::human_size(zp_plan_total_bytes(plan_->get()))))
            .arg(flac)
            .arg(deflate)
            .arg(stored));

    for (const auto& c : conflicts)
    {
        auto* box = new QGroupBox(c.kind == ZP_CONFLICT_COLLISION ? tr("Name collision: %1").arg(qs(c.detail)) : tr("Invalid name: %1").arg(qs(c.detail)), issues_);
        auto* v = new QVBoxLayout(box);
        for (const auto i : c.entries)
        {
            const auto& e = entries[i];
            auto* row = new QHBoxLayout;
            auto* name = new QLineEdit(qs(e.output_name), box);
            name->setAccessibleName(tr("New name for %1").arg(qs(e.output_name)));
            auto* rename = new QPushButton(tr("Rename"), box);
            auto* skip = new QPushButton(tr("Skip"), box);
            row->addWidget(new QLabel(qs(e.source_path), box), 1);
            row->addWidget(name, 1);
            row->addWidget(rename);
            row->addWidget(skip);
            connect(rename, &QPushButton::clicked, this, [this, i, name] { resolve({ i, ZP_RESOLVE_RENAME, nullptr }, name->text().toStdString()); });
            connect(skip, &QPushButton::clicked, this, [this, i] { resolve({ i, ZP_RESOLVE_SKIP, nullptr }, {}); });
            if (e.codec == ZP_CODEC_FLAC || e.codec == ZP_CODEC_FLAC_MONO)
            {
                auto* store = new QPushButton(tr("Store unconverted"), box);
                row->addWidget(store);
                connect(store, &QPushButton::clicked, this, [this, i] { resolve({ i, ZP_RESOLVE_DISABLE_FLAC, nullptr }, {}); });
            }
            v->addLayout(row);
        }
        issues_layout_->addWidget(box);
    }
    if (conflicts.empty())
        issues_layout_->addWidget(new QLabel(tr("No conflicts."), issues_));
    for (const auto& w : zpp::plan_warnings(plan_->get()))
        warnings_->addItem((w.kind == ZP_WARNING_SYMLINK_SKIPPED ? tr("Skipped: %1 — %2") : tr("Stored as is: %1 — %2")).arg(qs(w.source_path), qs(w.detail)));
    warnings_->setVisible(warnings_->count() > 0);
    build_->setEnabled(zp_plan_executable(plan_->get()) != 0);
}

ResolutionSheet::ResolutionSheet(zp_plan_t* plan, QWidget* parent) :
    QDialog(parent),
    plan_(plan)
{
    setWindowTitle(tr("Some names collide"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("These files would get the same name in the archive. Suggested new names:"), this));
    auto* form = new QFormLayout;
    const auto entries = zpp::plan_entries(plan);
    for (const auto& c : zpp::plan_conflicts(plan))
    {
        for (std::size_t k = 1; k < c.entries.size(); ++k)
        {
            const auto& e = entries[c.entries[k]];
            const auto existing = [&](const std::filesystem::path& p) {
                return std::ranges::any_of(entries, [&](const zpp::PlanEntry& x) { return std::filesystem::path(x.output_name) == p; });
            };
            const auto suggestion = satchel_gui::unique_path(std::filesystem::path(e.output_name), existing);
            auto* edit = new QLineEdit(QString::fromStdString(suggestion.generic_string()), this);
            form->addRow(QString::fromStdString(e.source_path), edit);
            edits_.emplace_back(c.entries[k], edit);
        }
    }
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Rename and continue"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool ResolutionSheet::apply()
{
    std::vector<std::string> names;
    names.reserve(edits_.size());
    std::vector<zp_resolution_t> res;
    res.reserve(edits_.size());
    for (const auto& [index, edit] : edits_)
        names.push_back(edit->text().toStdString());
    for (std::size_t i = 0; i < edits_.size(); ++i)
        res.push_back({ edits_[i].first, ZP_RESOLVE_RENAME, names[i].c_str() });
    return zp_plan_resolve(plan_, res.data(), res.size()) == ZP_OK && zp_plan_executable(plan_) != 0;
}

ExtractDialog::ExtractDialog(QString archive, const QString& destination, Settings settings, std::vector<std::size_t> selection, QWidget* parent) :
    QDialog(parent),
    archive_(std::move(archive)),
    selection_(std::move(selection)),
    settings_(std::move(settings))
{
    setWindowTitle(selection_.empty() ? tr("Extract all") : tr("Extract selected"));
    auto* form = new QFormLayout(this);
    auto* dest_row = new QHBoxLayout;
    destination_ = new QLineEdit(destination, this);
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto d = QFileDialog::getExistingDirectory(this, tr("Extract to"), destination_->text());
        if (!d.isEmpty())
            destination_->setText(d);
    });
    dest_row->addWidget(destination_, 1);
    dest_row->addWidget(browse);
    form->addRow(tr("Destination"), dest_row);
    restore_ = new QCheckBox(tr("Restore original audio (otherwise keep FLAC)"), this);
    restore_->setChecked(settings_.restore_audio);
    readme_ = new QCheckBox(tr("Include the generated readme"), this);
    readme_->setChecked(settings_.include_readme);
    overwrite_ = new QComboBox(this);
    overwrite_->addItems({ tr("Ask"), tr("Skip"), tr("Replace") });
    overwrite_->setCurrentIndex(static_cast<int>(settings_.overwrite));
    form->addRow(restore_);
    form->addRow(readme_);
    form->addRow(tr("Existing files"), overwrite_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Extract"));
    auto* copy = buttons->addButton(tr("Copy as CLI command"), QDialogButtonBox::HelpRole);
    connect(copy, &QPushButton::clicked, this, [this] {
        Settings s = settings_;
        s.restore_audio = restore_->isChecked();
        s.include_readme = readme_->isChecked();
        s.overwrite = static_cast<satchel_gui::Overwrite>(overwrite_->currentIndex());
        QApplication::clipboard()->setText(QString::fromStdString(satchel_gui::cli_extract_command(s, to_path(archive_), to_path(destination_->text()))));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

std::optional<ExtractChoice> ExtractDialog::run()
{
    if (exec() != QDialog::Accepted)
        return std::nullopt;
    ExtractChoice choice;
    choice.destination = destination_->text();
    choice.settings = settings_;
    choice.settings.restore_audio = restore_->isChecked();
    choice.settings.include_readme = readme_->isChecked();
    choice.settings.overwrite = static_cast<satchel_gui::Overwrite>(overwrite_->currentIndex());

    // Pre-flight: the extraction plan's warnings, errors and existing files, before writing.
    try
    {
        zpp::Stream in(zpp::not_null(zp_stream_open_file(archive_.toStdString().c_str())));
        zpp::Reader reader(zpp::not_null(zp_reader_open(app_context(), in.get())));
        zpp::Sink sink(zpp::not_null(zp_sink_filesystem(choice.destination.toStdString().c_str())));
        zp_extract_options_t options{};
        zp_extract_options_init(&options);
        options.restore_wav = choice.settings.restore_audio ? 1 : 0;
        options.include_readme = choice.settings.include_readme ? 1 : 0;
        zpp::ExtractionPlan plan(zpp::not_null(zp_extract_plan(reader.get(), selection_.empty() ? nullptr : selection_.data(), selection_.size(), sink.get(), &options)));
        QStringList problems;
        QStringList existing;
        zp_extract_issue_t issue{};
        for (std::size_t i = 0; i < zp_xplan_issue_count(plan.get()); ++i)
        {
            zpp::check(zp_xplan_get_issue(plan.get(), i, &issue));
            if (issue.kind == ZP_ISSUE_EXISTS_AT_DESTINATION)
                existing << QString::fromUtf8(issue.name);
            else
                problems << QString("%1 %2: %3").arg(issue.is_error ? "✖" : "⚠", QString::fromUtf8(issue.name), QString::fromUtf8(issue.detail));
        }
        if (!problems.isEmpty())
        {
            QMessageBox box(QMessageBox::Warning, tr("Before extracting"), tr("%1 entr(ies) need attention; refused entries will not be written.").arg(problems.size()), QMessageBox::Ok | QMessageBox::Cancel, parentWidget());
            box.setDetailedText(problems.join('\n'));
            if (box.exec() != QMessageBox::Ok)
                return std::nullopt;
        }
        if (!existing.isEmpty() && choice.settings.overwrite == satchel_gui::Overwrite::Ask)
        {
            QMessageBox box(QMessageBox::Question, tr("Files already exist"), tr("%1 file(s) already exist at the destination.").arg(existing.size()), QMessageBox::NoButton, parentWidget());
            box.setDetailedText(existing.join('\n'));
            auto* replace = box.addButton(tr("Replace"), QMessageBox::AcceptRole);
            box.addButton(tr("Skip existing"), QMessageBox::RejectRole);
            auto* cancel = box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == cancel)
                return std::nullopt;
            choice.replace_existing = box.clickedButton() == replace;
        }
        else
            choice.replace_existing = choice.settings.overwrite == satchel_gui::Overwrite::Replace;
    } catch (const zpp::Error& e)
    {
        QMessageBox::critical(parentWidget(), tr("Cannot extract"), QString::fromUtf8(e.what()));
        return std::nullopt;
    }
    return choice;
}

SettingsDialog::SettingsDialog(Settings s, QWidget* parent) :
    QDialog(parent),
    base_(std::move(s))
{
    setWindowTitle(tr("Settings"));
    auto* form = new QFormLayout(this);
    const auto path_text = [](const std::optional<std::filesystem::path>& p) { return p ? QString::fromStdU16String(p->u16string()) : QString(); };
    last_mode_ = new QCheckBox(tr("Start in the mode used last"), this);
    last_mode_->setChecked(base_.start_in_last_mode);
    simple_output_ = new QLineEdit(path_text(base_.simple_output_folder), this);
    simple_output_->setPlaceholderText(tr("Next to the source"));
    flac_ = new QCheckBox(tr("Convert PCM audio to FLAC"), this);
    flac_->setChecked(base_.flac);
    flac_level_ = new QSpinBox(this);
    flac_level_->setRange(0, 8);
    flac_level_->setValue(base_.flac_level);
    deflate_ = new QSpinBox(this);
    deflate_->setRange(1, 9);
    deflate_->setValue(base_.deflate_level);
    threads_ = new QSpinBox(this);
    threads_->setRange(0, 256);
    threads_->setSpecialValueText(tr("All cores"));
    threads_->setValue(base_.threads);
    temp_ = new QLineEdit(path_text(base_.temp_folder), this);
    temp_->setPlaceholderText(tr("System temporary folder"));
    restore_ = new QCheckBox(tr("Restore original audio"), this);
    restore_->setChecked(base_.restore_audio);
    readme_ = new QCheckBox(tr("Include the generated readme"), this);
    readme_->setChecked(base_.include_readme);
    overwrite_ = new QComboBox(this);
    overwrite_->addItems({ tr("Ask"), tr("Skip"), tr("Replace") });
    overwrite_->setCurrentIndex(static_cast<int>(base_.overwrite));
    verify_ = new QCheckBox(tr("Verify after every build (re-reads the archive)"), this);
    verify_->setChecked(base_.verify_after_build);
    associate_ = new QCheckBox(tr("Offer to open .zip files"), this);
    associate_->setChecked(base_.associate_zip);
    updates_ = new QCheckBox(tr("Check for updates automatically"), this);
    updates_->setChecked(base_.check_updates);
    form->addRow(tr("General"), last_mode_);
    form->addRow(tr("Simple mode output"), simple_output_);
    form->addRow(tr("Compression"), flac_);
    form->addRow(tr("FLAC level"), flac_level_);
    form->addRow(tr("Deflate level"), deflate_);
    form->addRow(tr("Threads (restart to apply)"), threads_);
    form->addRow(tr("Temporary files"), temp_);
    form->addRow(tr("Extraction"), restore_);
    form->addRow(QString(), readme_);
    form->addRow(tr("Existing files"), overwrite_);
    form->addRow(tr("Verification"), verify_);
    form->addRow(tr("Integration"), associate_);
    form->addRow(tr("Updates"), updates_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* copy = buttons->addButton(tr("Copy as CLI command"), QDialogButtonBox::HelpRole);
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(QString::fromStdString(satchel_gui::cli_create_command(settings(), "archive.zip", { "files" })));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

Settings SettingsDialog::settings() const
{
    Settings s = base_;
    const auto opt_path = [](const QLineEdit* e) -> std::optional<std::filesystem::path> {
        if (e->text().trimmed().isEmpty())
            return std::nullopt;
        return to_path(e->text().trimmed());
    };
    s.start_in_last_mode = last_mode_->isChecked();
    s.simple_output_folder = opt_path(simple_output_);
    s.flac = flac_->isChecked();
    s.flac_level = flac_level_->value();
    s.deflate_level = deflate_->value();
    s.threads = threads_->value();
    s.temp_folder = opt_path(temp_);
    s.restore_audio = restore_->isChecked();
    s.include_readme = readme_->isChecked();
    s.overwrite = static_cast<satchel_gui::Overwrite>(overwrite_->currentIndex());
    s.verify_after_build = verify_->isChecked();
    s.associate_zip = associate_->isChecked();
    s.check_updates = updates_->isChecked();
    return s;
}
