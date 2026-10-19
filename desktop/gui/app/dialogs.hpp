// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"
#include "zp_cpp.hpp"

#include <QComboBox>
#include <QDialog>
#include <QStringList>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QAbstractSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableView;
class QVBoxLayout;
class QStandardItemModel;
class QWidget;

// Gives a spin box's text some room from the frame (macOS draws it against the left edge).
void pad_spin_box(QAbstractSpinBox* spin);

// A folder setting that has a default: a dropdown of the default ("Next to the source"), the
// chosen folder, and "Choose Folder…", which asks for one.
class FolderChoice : public QComboBox
{
    Q_OBJECT

public:
    FolderChoice(const QString& default_text, QString dialog_title, std::optional<std::filesystem::path> folder, QWidget* parent = nullptr);
    // Empty when the default is selected.
    [[nodiscard]] std::optional<std::filesystem::path> folder() const;
    void set_folder(std::optional<std::filesystem::path> folder);

private:
    void rebuild();

    QString default_text_;
    QString dialog_title_;
    std::optional<std::filesystem::path> folder_;
    int previous_ = 0;
};

// Exclude settings: the system and DAW cache presets and an editable list of custom patterns.
class ExcludeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ExcludeEditor(const satchel_gui::Settings& settings, QWidget* parent = nullptr);
    void apply_to(satchel_gui::Settings& settings) const;

signals:
    void changed();

private:
    QCheckBox* system_ = nullptr;
    QCheckBox* daw_ = nullptr;
    QListWidget* list_ = nullptr;
};

// Simple mode: name collisions only, defaulting to "rename with suffix".
class ResolutionSheet : public QDialog
{
    Q_OBJECT

public:
    explicit ResolutionSheet(zp_plan_t* plan, QWidget* parent = nullptr);
    // Applies the chosen names; false if the library rejected them.
    bool apply();

private:
    zp_plan_t* plan_;
    std::vector<std::pair<std::size_t, QLineEdit*>> edits_;
};

struct ExtractChoice
{
    QString destination;
    satchel_gui::Settings settings;
    bool replace_existing = false;
};

class ExtractDialog : public QDialog
{
    Q_OBJECT

public:
    ExtractDialog(QString archive, const QString& destination, satchel_gui::Settings settings, std::vector<std::size_t> selection, QWidget* parent = nullptr);
    // Options, then the pre-flight check (unsafe paths, links, groups, existing files).
    std::optional<ExtractChoice> run();

private:
    QString archive_;
    std::vector<std::size_t> selection_;
    QLineEdit* destination_ = nullptr;
    QCheckBox* restore_ = nullptr;
    QCheckBox* readme_ = nullptr;
    QComboBox* overwrite_ = nullptr;
    satchel_gui::Settings settings_;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(satchel_gui::Settings settings, QWidget* parent = nullptr);
    [[nodiscard]] satchel_gui::Settings settings() const;

private:
    satchel_gui::Settings base_;
    QCheckBox* last_mode_ = nullptr;
    FolderChoice* simple_output_ = nullptr;
    QCheckBox* flac_ = nullptr;
    QSpinBox* flac_level_ = nullptr;
    QSpinBox* deflate_ = nullptr;
    QSpinBox* threads_ = nullptr;
    FolderChoice* temp_ = nullptr;
    QCheckBox* restore_ = nullptr;
    QCheckBox* readme_ = nullptr;
    QComboBox* overwrite_ = nullptr;
    QCheckBox* verify_ = nullptr;
    ExcludeEditor* excludes_ = nullptr;
    QCheckBox* associate_ = nullptr;
    QCheckBox* updates_ = nullptr;
};
