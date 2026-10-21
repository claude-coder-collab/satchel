// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "session_bar.hpp"

#include "dialogs.hpp"
#include "labels.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

using satchel_gui::ExistingEntries;
using satchel_gui::RowState;

namespace
{

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

// A popup frame that closes when the user clicks elsewhere; deletes itself when hidden.
class Popover : public QFrame
{
public:
    std::function<void()> on_hide;

    explicit Popover(QWidget* parent) :
        QFrame(parent, Qt::Popup)
    {
        setFrameShape(QFrame::StyledPanel);
        setAttribute(Qt::WA_DeleteOnClose);
    }

    void show_above(QWidget* anchor)
    {
        adjustSize();
        auto pos = anchor->mapToGlobal(QPoint(0, 0)) - QPoint(0, height() + 4);
        if (const auto* screen = anchor->screen())
        {
            const auto area = screen->availableGeometry();
            pos.setX(std::clamp(pos.x(), area.left(), std::max(area.left(), area.right() - width())));
            pos.setY(std::max(pos.y(), area.top()));
        }
        move(pos);
        show();
    }

protected:
    void hideEvent(QHideEvent* e) override
    {
        QFrame::hideEvent(e);
        if (on_hide)
            std::exchange(on_hide, nullptr)();
        close();
    }
};

}

ElidedLabel::ElidedLabel(QWidget* parent) :
    QLabel(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::set_full_text(const QString& text)
{
    full_ = text;
    update_text();
}

QSize ElidedLabel::minimumSizeHint() const
{
    return { 0, QLabel::minimumSizeHint().height() };
}

void ElidedLabel::resizeEvent(QResizeEvent* e)
{
    QLabel::resizeEvent(e);
    update_text();
}

void ElidedLabel::update_text()
{
    const auto shown = fontMetrics().elidedText(full_, Qt::ElideRight, std::max(0, contentsRect().width()));
    setText(shown);
    setToolTip(shown == full_ ? QString() : full_);
}

SessionBar::SessionBar(QWidget* parent) :
    QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(8, 4, 8, 4);
    rows->setSpacing(2);
    auto* top = new QHBoxLayout;
    auto* layout = new QHBoxLayout;
    rows->addLayout(top);
    rows->addLayout(layout);
    summary_ = new ElidedLabel(this);
    summary_->setAccessibleName(tr("Staged changes"));
    issues_ = new QPushButton(this);
    issues_->setAccessibleName(tr("Issues"));
    options_ = new QPushButton(this);
    options_->setText(tr("Options"));
    discard_ = new QPushButton(tr("Discard"), this);
    go_ = new QPushButton(this);
    go_->setDefault(true);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setMaximumWidth(220);
    progress_->setAccessibleName(tr("Writing progress"));
    cancel_write_ = new QPushButton(tr("Cancel"), this);
    cancel_write_->setAccessibleName(tr("Cancel writing the archive"));
    location_label_ = new QLabel(tr("Location"), this);
    location_ = new LocationChoice(this);
    result_ = new QLabel(this);
    reveal_ = new QPushButton(tr("Reveal"), this);
    scan_delay_ = new QTimer(this);
    scan_delay_->setSingleShot(true);
    scan_delay_->setInterval(300);
    connect(scan_delay_, &QTimer::timeout, this, [this] {
        scanning_ = busy_ && !committing_;
        update_visibility();
    });
    top->addWidget(result_);
    top->addWidget(reveal_);
    top->addWidget(summary_, 1);
    layout->addWidget(location_label_);
    layout->addWidget(location_);
    layout->addStretch(1);
    layout->addWidget(issues_);
    layout->addWidget(options_);
    layout->addWidget(discard_);
    layout->addWidget(go_);
    layout->addWidget(progress_);
    layout->addWidget(cancel_write_);
    result_->hide();
    reveal_->hide();

    connect(go_, &QPushButton::clicked, this, &SessionBar::go);
    connect(location_, &LocationChoice::folder_chosen, this, [this](const QString& folder) {
        if (snapshot_)
            emit target_changed(QDir(folder).filePath(QFileInfo(snapshot_->target).fileName()));
    });
    connect(discard_, &QPushButton::clicked, this, [this] {
        if (snapshot_ && snapshot_->is_new)
            emit cancel_new();
        else
            emit discard();
    });
    connect(cancel_write_, &QPushButton::clicked, this, [this] {
        cancel_write_->setEnabled(false);
        emit cancel_write();
    });
    connect(issues_, &QPushButton::clicked, this, &SessionBar::open_issues);
    connect(options_, &QPushButton::clicked, this, &SessionBar::open_options);
    connect(reveal_, &QPushButton::clicked, this, [this] { emit reveal(result_output_); });
    hide();
}

void SessionBar::set_options(const satchel_gui::Settings& settings, ExistingEntries existing)
{
    settings_ = settings;
    existing_ = existing;
}

void SessionBar::show_snapshot(std::shared_ptr<const SessionSnapshot> snapshot, bool busy, bool committing)
{
    snapshot_ = std::move(snapshot);
    if (committing && !committing_)
    {
        progress_->setValue(0);
        cancel_write_->setEnabled(true);
    }
    committing_ = committing;
    busy_ = busy;
    if (!busy || committing)
    {
        scan_delay_->stop();
        scanning_ = false;
    }
    else if (!scanning_ && !scan_delay_->isActive())
        scan_delay_->start();
    if (snapshot_ && snapshot_->is_new)
        location_->set_folder(QFileInfo(snapshot_->target).absolutePath());
    if (!snapshot_)
    {
        summary_->set_full_text(busy ? tr("Scanning files…") : QString());
        go_->setEnabled(false);
        update_visibility();
        return;
    }
    const auto& v = snapshot_->view;
    const auto& sum = v.summary;
    QString text;
    if (committing)
        text = tr("Writing…");
    else if (busy)
        text = tr("Scanning files…");
    else if (snapshot_->is_new)
    {
        std::size_t flac = 0;
        std::size_t deflate = 0;
        std::size_t store = 0;
        for (const auto& r : v.rows)
        {
            if (!r.plan || r.generated || r.plan->kind != ZP_KIND_FILE)
                continue;
            (r.plan->codec == ZP_CODEC_FLAC || r.plan->codec == ZP_CODEC_FLAC_MONO ? flac : r.stored || r.plan->fallback_reason >= 0 ? store
                                                                                                                                     : deflate) += 1;
        }
        text = sum.added == 0 ? tr("Add files or folders, or drop them here.")
                              : tr("%n file(s), %1 · %2 FLAC, %3 deflate, %4 store", nullptr, static_cast<int>(sum.added)).arg(qs(satchel_gui::human_size(sum.added_bytes))).arg(flac).arg(deflate).arg(store);
    }
    else
    {
        QStringList parts;
        if (sum.added)
            parts << tr("+%n file(s) (%1)", nullptr, static_cast<int>(sum.added)).arg(qs(satchel_gui::human_size(sum.added_bytes)));
        if (sum.removed)
            parts << tr("−%n removed", nullptr, static_cast<int>(sum.removed));
        if (sum.moved)
            parts << tr("%n moved", nullptr, static_cast<int>(sum.moved));
        if (sum.renamed)
            parts << tr("%n renamed", nullptr, static_cast<int>(sum.renamed));
        if (sum.replaced)
            parts << tr("%n replaced", nullptr, static_cast<int>(sum.replaced));
        text = parts.isEmpty() ? tr("No changes") : parts.join(QStringLiteral(" · "));
    }
    if (!v.excluded.empty() && !busy)
        text += tr(" · %n left out", nullptr, static_cast<int>(v.excluded.size()));
    summary_->set_full_text(text);

    const auto notes = v.warnings.size() + v.excluded.size() + v.problems.size();
    issues_->setText(v.conflicts.empty() ? tr("Notes (%1)").arg(notes) : tr("Issues (%1)").arg(v.conflicts.size()));
    issues_->setIcon(v.conflicts.empty() ? QIcon() : style()->standardIcon(QStyle::SP_MessageBoxWarning));
    go_->setText(snapshot_->is_new ? tr("Create Archive") : tr("Apply Changes"));
    const bool has_work = snapshot_->has_changes;
    go_->setEnabled(!busy && v.executable && has_work);
    discard_->setText(snapshot_->is_new ? tr("Cancel") : tr("Discard"));
    discard_->setToolTip(snapshot_->is_new ? tr("Leave without creating the archive") : tr("Undo all staged changes"));
    discard_->setEnabled(!busy && (snapshot_->is_new || snapshot_->has_changes));
    options_->setEnabled(!busy);
    if (snapshot_->has_changes)
        clear_result();
    update_visibility();
}

void SessionBar::set_progress(std::uint64_t done, std::uint64_t total)
{
    progress_->setValue(total ? static_cast<int>(1000.0 * static_cast<double>(done) / static_cast<double>(total)) : 0);
}

void SessionBar::show_result(const QString& text, const QString& output, bool warning)
{
    result_->setText((warning ? QStringLiteral("⚠ ") : QStringLiteral("✓ ")) + text);
    result_output_ = output;
    result_->show();
    reveal_->setVisible(!output.isEmpty());
    update_visibility();
}

void SessionBar::clear_result()
{
    result_->hide();
    reveal_->hide();
    result_output_.clear();
}

void SessionBar::update_visibility()
{
    const auto* v = snapshot_ ? &snapshot_->view : nullptr;
    const bool staged = v && (snapshot_->is_new || snapshot_->has_changes || !v->conflicts.empty() || !v->problems.empty());
    summary_->setVisible(staged || committing_ || scanning_);
    for (auto* w : std::initializer_list<QWidget*>{ options_, discard_, go_ })
        w->setVisible(staged && !committing_);
    location_label_->setVisible(staged && snapshot_->is_new && !committing_);
    location_->setVisible(location_label_->isVisibleTo(this));
    if (scanning_ && !committing_)
        progress_->setRange(0, 0);
    else
        progress_->setRange(0, 1000);
    progress_->setVisible(committing_ || scanning_);
    cancel_write_->setVisible(committing_);
    issues_->setVisible(staged && !committing_ && (!v->conflicts.empty() || !v->warnings.empty() || !v->excluded.empty() || !v->problems.empty()));
    setVisible(staged || committing_ || scanning_ || !result_output_.isEmpty() || !result_->isHidden());
}

void SessionBar::open_issues()
{
    if (!snapshot_)
        return;
    const auto& v = snapshot_->view;
    auto* pop = new Popover(this);
    auto* outer = new QVBoxLayout(pop);
    auto* scroll = new QScrollArea(pop);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget(scroll);
    auto* layout = new QVBoxLayout(body);
    for (const auto& c : v.conflicts)
    {
        auto* box = new QGroupBox(c.kind == ZP_CONFLICT_COLLISION ? tr("Name collision: %1").arg(qs(c.detail)) : tr("Invalid name: %1").arg(qs(c.detail)), body);
        auto* grid = new QGridLayout(box);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        int line = 0;
        for (const auto& key : c.entries)
        {
            const auto it = std::ranges::find_if(v.rows, [&](const satchel_gui::SessionRow& r) { return r.key == key; });
            const bool flac = it != v.rows.end() && it->plan && (it->plan->codec == ZP_CODEC_FLAC || it->plan->codec == ZP_CODEC_FLAC_MONO);
            const bool existing = key.source.starts_with("archive:");
            const auto source = QDir::toNativeSeparators(qs(key.source));
            auto* from = new QLabel(box);
            from->setText(existing ? tr("In the archive") : tr("Added: %1").arg(from->fontMetrics().elidedText(source, Qt::ElideMiddle, 320)));
            from->setToolTip(existing ? qs(key.name) : source);
            auto* name = new QLineEdit(qs(key.name), box);
            name->setAccessibleName(tr("New name for %1").arg(qs(key.name)));
            auto* rename = new QPushButton(tr("Rename"), box);
            auto* skip = new QPushButton(existing ? tr("Remove") : tr("Skip"), box);
            grid->addWidget(from, line, 0);
            grid->addWidget(name, line, 1);
            grid->addWidget(rename, line, 2);
            grid->addWidget(skip, line, 3);
            connect(rename, &QPushButton::clicked, pop, [this, pop, key, name] {
                emit resolve({ key, ZP_RESOLVE_RENAME, name->text().toStdString() });
                pop->close();
            });
            connect(skip, &QPushButton::clicked, pop, [this, pop, key] {
                emit resolve({ key, ZP_RESOLVE_SKIP, {} });
                pop->close();
            });
            if (flac)
            {
                auto* store = new QPushButton(tr("Store unconverted"), box);
                grid->addWidget(store, line, 4);
                connect(store, &QPushButton::clicked, pop, [this, pop, key] {
                    emit resolve({ key, ZP_RESOLVE_DISABLE_FLAC, {} });
                    pop->close();
                });
            }
            ++line;
        }
        layout->addWidget(box);
    }
    if (!v.warnings.empty() || !v.excluded.empty() || !v.problems.empty())
    {
        auto* list = new QListWidget(body);
        list->setAccessibleName(tr("Notes"));
        for (const auto& p : v.problems)
            list->addItem(tr("Dropped a change: %1").arg(qs(p)));
        for (const auto& w : v.warnings)
            list->addItem((w.kind == ZP_WARNING_SYMLINK_SKIPPED ? tr("Skipped: %1 — %2") : tr("Stored as is: %1 — %2")).arg(qs(w.source_path), qs(w.detail)));
        for (const auto& x : v.excluded)
            list->addItem(tr("Left out: %1%2").arg(qs(x.archive_path), x.kind == ZP_KIND_DIRECTORY ? QStringLiteral("/") : QString()));
        list->setMinimumHeight(std::min(240, 22 * list->count() + 8));
        layout->addWidget(list);
    }
    layout->addStretch(1);
    scroll->setWidget(body);
    outer->addWidget(scroll);
    pop->resize(760, std::min(480, body->sizeHint().height() + 24));
    pop->setMinimumSize(pop->size());
    pop->show_above(issues_);
}

void SessionBar::open_options()
{
    auto* pop = new Popover(this);
    auto* form = new QFormLayout(pop);
    auto* flac = new QCheckBox(tr("Convert PCM audio to FLAC"), pop);
    flac->setChecked(settings_.flac);
    auto* flac_level = new QSpinBox(pop);
    flac_level->setRange(0, 8);
    flac_level->setValue(settings_.flac_level);
    auto* deflate = new QSpinBox(pop);
    deflate->setRange(1, 9);
    deflate->setValue(settings_.deflate_level);
    pad_spin_box(flac_level);
    pad_spin_box(deflate);
    auto* existing = new QComboBox(pop);
    existing->addItem(tr("Ask"), static_cast<int>(ExistingEntries::Ask));
    existing->addItem(tr("Replace"), static_cast<int>(ExistingEntries::Replace));
    existing->addItem(tr("Replace if newer"), static_cast<int>(ExistingEntries::ReplaceIfNewer));
    existing->addItem(tr("Keep the archive's"), static_cast<int>(ExistingEntries::Keep));
    existing->setCurrentIndex(existing->findData(static_cast<int>(existing_)));
    existing->setToolTip(tr("When an added file has the same name as an entry already in the archive"));
    auto* excludes = new ExcludeEditor(settings_, pop);
    form->addRow(flac);
    form->addRow(tr("FLAC level"), flac_level);
    form->addRow(tr("Deflate level"), deflate);
    form->addRow(tr("Existing entries"), existing);
    form->addRow(tr("Leave out"), excludes);
    form->addRow(new QLabel(tr("These apply to this archive only."), pop));
    auto* error = new QLabel(pop);
    error->setWordWrap(true);
    error->hide();
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, pop);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, pop, &QWidget::close);
    connect(buttons, &QDialogButtonBox::accepted, pop, [this, pop, error, flac, flac_level, deflate, existing, excludes] {
        auto s = settings_;
        s.flac = flac->isChecked();
        s.flac_level = flac_level->value();
        s.deflate_level = deflate->value();
        excludes->apply_to(s);
        if (const auto message = exclude_pattern_error(exclude_patterns(s)); !message.isEmpty())
        {
            error->setText(tr("Invalid Leave out pattern: %1").arg(message));
            error->show();
            pop->adjustSize();
            return;
        }
        const auto e = static_cast<ExistingEntries>(existing->currentData().toInt());
        const bool same = s.flac == settings_.flac && s.flac_level == settings_.flac_level && s.deflate_level == settings_.deflate_level && s.exclude_system == settings_.exclude_system
            && s.exclude_daw_caches == settings_.exclude_daw_caches && s.exclude_patterns == settings_.exclude_patterns && e == existing_;
        pop->close();
        if (same)
            return;
        settings_ = s;
        existing_ = e;
        emit options_changed(s, e);
    });
    pop->show_above(options_);
}
