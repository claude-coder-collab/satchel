// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"
#include "zp_cpp.hpp"

#include <QDialog>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

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

// Plans `inputs` and lets the user review names, methods, conflicts and warnings before a build.
class PlanReviewDialog : public QDialog
{
    Q_OBJECT

public:
    PlanReviewDialog(QStringList inputs, const QString& output, satchel_gui::Settings settings, QWidget* parent = nullptr);

    // After accept(): the plan to build and where.
    std::shared_ptr<zpp::Plan> take_plan() { return plan_; }
    [[nodiscard]] QString output() const;
    [[nodiscard]] const satchel_gui::Settings& settings() const { return settings_; }

    // Builds a plan with the given options (also used by Simple mode).
    static std::shared_ptr<zpp::Plan> make_plan(const QStringList& inputs, const satchel_gui::Settings& settings);

private:
    void replan();
    void refresh();
    void resolve(zp_resolution_t r, const std::string& name_storage);

    QStringList inputs_;
    satchel_gui::Settings settings_;
    std::shared_ptr<zpp::Plan> plan_;
    QLabel* totals_ = nullptr;
    QWidget* issues_ = nullptr;
    QVBoxLayout* issues_layout_ = nullptr;
    QListWidget* warnings_ = nullptr;
    QTableView* table_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QLineEdit* output_ = nullptr;
    QCheckBox* flac_ = nullptr;
    QSpinBox* deflate_ = nullptr;
    QSpinBox* flac_level_ = nullptr;
    QPushButton* build_ = nullptr;
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
    QLineEdit* simple_output_ = nullptr;
    QCheckBox* flac_ = nullptr;
    QSpinBox* flac_level_ = nullptr;
    QSpinBox* deflate_ = nullptr;
    QSpinBox* threads_ = nullptr;
    QLineEdit* temp_ = nullptr;
    QCheckBox* restore_ = nullptr;
    QCheckBox* readme_ = nullptr;
    QComboBox* overwrite_ = nullptr;
    QCheckBox* verify_ = nullptr;
    QCheckBox* associate_ = nullptr;
    QCheckBox* updates_ = nullptr;
};
