// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "dialogs.hpp"

#include "jobs.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
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
#include <QSignalBlocker>
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

// Rich text, so Qt wraps the tooltip instead of showing one long line.
QString wrapped(const QString& text)
{
    return QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped());
}

QString preset_tooltip(int preset)
{
    QStringList names;
    for (const auto& p : zpp::exclude_preset(preset))
        names << qs(p).replace('\r', QStringLiteral("\\r"));
    return wrapped(QObject::tr("Leaves out: %1").arg(names.join(QStringLiteral(", "))));
}

}

void pad_spin_box(QAbstractSpinBox* spin)
{
    if (auto* edit = spin->findChild<QLineEdit*>())
        edit->setTextMargins(4, 0, 0, 0);
    spin->setMinimumWidth(spin->sizeHint().width() + 4);
}

FolderChoice::FolderChoice(const QString& default_text, QString dialog_title, std::optional<std::filesystem::path> folder, QWidget* parent) :
    QComboBox(parent),
    default_text_(default_text),
    dialog_title_(std::move(dialog_title)),
    folder_(std::move(folder))
{
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
    rebuild();
    setCurrentIndex(folder_ ? 1 : 0);
    previous_ = currentIndex();
    connect(this, &QComboBox::activated, this, [this](int index) {
        if (index != count() - 1)
        {
            previous_ = index;
            return;
        }
        const auto start = folder_ ? QString::fromStdU16String(folder_->u16string()) : QString();
        const auto chosen = QFileDialog::getExistingDirectory(this, dialog_title_, start);
        if (chosen.isEmpty())
        {
            setCurrentIndex(previous_);
            return;
        }
        set_folder(to_path(chosen));
    });
}

std::optional<std::filesystem::path> FolderChoice::folder() const
{
    if (folder_ && currentIndex() == 1)
        return folder_;
    return std::nullopt;
}

void FolderChoice::set_folder(std::optional<std::filesystem::path> folder)
{
    folder_ = std::move(folder);
    rebuild();
    setCurrentIndex(folder_ ? 1 : 0);
    previous_ = currentIndex();
}

void FolderChoice::rebuild()
{
    const QSignalBlocker block(this);
    clear();
    addItem(default_text_);
    if (folder_)
    {
        const auto path = QDir::toNativeSeparators(QString::fromStdU16String(folder_->u16string()));
        addItem(QFileIconProvider().icon(QFileIconProvider::Folder), QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
        setItemData(1, path, Qt::ToolTipRole);
    }
    insertSeparator(count());
    addItem(tr("Choose Folder…"));
}

ExcludeEditor::ExcludeEditor(const Settings& settings, QWidget* parent) :
    QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    system_ = new QCheckBox(tr("Leave out system files (.DS_Store, ._*, Thumbs.db, …)"), this);
    system_->setChecked(settings.exclude_system);
    system_->setToolTip(preset_tooltip(ZP_EXCLUDE_PRESET_SYSTEM));
    daw_ = new QCheckBox(tr("Leave out waveform and analysis caches of audio software"), this);
    daw_->setChecked(settings.exclude_daw_caches);
    daw_->setToolTip(preset_tooltip(ZP_EXCLUDE_PRESET_DAW_CACHES));
    list_ = new QListWidget(this);
    list_->setAccessibleName(tr("Exclude patterns"));
    list_->setToolTip(wrapped(tr("Names to leave out. * matches any text, ? one character, [a-z] one character of a set. End a pattern with / to match folders only.")));
    list_->setMaximumHeight(110);
    for (const auto& p : settings.exclude_patterns)
    {
        auto* item = new QListWidgetItem(qs(p), list_);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    auto* add = new QPushButton(tr("Add Pattern"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    remove->setEnabled(false);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addWidget(system_);
    layout->addWidget(daw_);
    layout->addWidget(list_);
    layout->addLayout(buttons);

    connect(system_, &QCheckBox::toggled, this, &ExcludeEditor::changed);
    connect(daw_, &QCheckBox::toggled, this, &ExcludeEditor::changed);
    connect(list_, &QListWidget::itemSelectionChanged, this, [this, remove] { remove->setEnabled(!list_->selectedItems().isEmpty()); });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (item->text().trimmed().isEmpty())
            delete item;
        emit changed();
    });
    connect(add, &QPushButton::clicked, this, [this] {
        auto* item = new QListWidgetItem(QStringLiteral("*.tmp"), list_);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        list_->setCurrentItem(item);
        list_->editItem(item);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        qDeleteAll(list_->selectedItems());
        emit changed();
    });
}

void ExcludeEditor::apply_to(Settings& settings) const
{
    settings.exclude_system = system_->isChecked();
    settings.exclude_daw_caches = daw_->isChecked();
    settings.exclude_patterns.clear();
    for (int i = 0; i < list_->count(); ++i)
    {
        const auto text = list_->item(i)->text().trimmed();
        if (!text.isEmpty())
            settings.exclude_patterns.push_back(text.toStdString());
    }
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
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
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
    overwrite_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
    resize(sizeHint().width() * 2, sizeHint().height());
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
    last_mode_ = new QCheckBox(tr("Start in the mode used last"), this);
    last_mode_->setChecked(base_.start_in_last_mode);
    simple_output_ = new FolderChoice(tr("Next to the source"), tr("Folder for Simple mode output"), base_.simple_output_folder, this);
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
    threads_->setMinimumWidth(threads_->fontMetrics().horizontalAdvance(tr("All cores")) + 52);
    pad_spin_box(flac_level_);
    pad_spin_box(deflate_);
    if (auto* edit = threads_->findChild<QLineEdit*>())
        edit->setTextMargins(4, 0, 0, 0);
    temp_ = new FolderChoice(tr("System temporary folder"), tr("Folder for temporary files"), base_.temp_folder, this);
    restore_ = new QCheckBox(tr("Restore original audio"), this);
    restore_->setChecked(base_.restore_audio);
    readme_ = new QCheckBox(tr("Include the generated readme"), this);
    readme_->setChecked(base_.include_readme);
    overwrite_ = new QComboBox(this);
    overwrite_->addItems({ tr("Ask"), tr("Skip"), tr("Replace") });
    overwrite_->setCurrentIndex(static_cast<int>(base_.overwrite));
    verify_ = new QCheckBox(tr("Verify after every build (re-reads the archive)"), this);
    verify_->setChecked(base_.verify_after_build);
    excludes_ = new ExcludeEditor(base_, this);
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
    form->addRow(tr("Leave out"), excludes_);
    form->addRow(tr("Integration"), associate_);
    form->addRow(tr("Updates"), updates_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* copy = buttons->addButton(tr("Copy as CLI command"), QDialogButtonBox::HelpRole);
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(QString::fromStdString(satchel_gui::cli_create_command(settings(), "archive.zip", { "files" })));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (const auto error = exclude_pattern_error(exclude_patterns(settings())); !error.isEmpty())
        {
            QMessageBox::warning(this, tr("Invalid exclude pattern"), error);
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

Settings SettingsDialog::settings() const
{
    Settings s = base_;
    s.start_in_last_mode = last_mode_->isChecked();
    s.simple_output_folder = simple_output_->folder();
    s.flac = flac_->isChecked();
    s.flac_level = flac_level_->value();
    s.deflate_level = deflate_->value();
    s.threads = threads_->value();
    s.temp_folder = temp_->folder();
    s.restore_audio = restore_->isChecked();
    s.include_readme = readme_->isChecked();
    s.overwrite = static_cast<satchel_gui::Overwrite>(overwrite_->currentIndex());
    s.verify_after_build = verify_->isChecked();
    excludes_->apply_to(s);
    s.associate_zip = associate_->isChecked();
    s.check_updates = updates_->isChecked();
    return s;
}
