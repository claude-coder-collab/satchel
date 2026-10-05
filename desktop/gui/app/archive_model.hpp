// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"
#include "zp_cpp.hpp"

#include <QAbstractItemModel>
#include <QHash>
#include <QSortFilterProxyModel>

#include <functional>
#include <memory>
#include <vector>

// The open archive as rows: a flat list or a folder tree; multi-mono groups are one row that
// expands to its members. Data comes from the central directory only.
class ArchiveModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column {
        Name,
        Size,
        Packed,
        Ratio,
        Method,
        RestoresTo,
        Modified,
        Verified,
        ColumnCount,
    };
    static constexpr int EntriesRole = Qt::UserRole + 1; // QVariantList of entry indices
    static constexpr int SortRole = Qt::UserRole + 2;
    static constexpr int MethodRole = Qt::UserRole + 3; // plain method key for filtering
    static constexpr int RestorableRole = Qt::UserRole + 4;
    static constexpr int FolderRole = Qt::UserRole + 5;

    explicit ArchiveModel(QObject* parent = nullptr);
    ~ArchiveModel() override;

    void set_entries(const std::vector<zpp::EntryInfo>& entries);
    void set_tree(bool tree);
    [[nodiscard]] bool tree() const { return tree_; }
    void set_verification(const QHash<int, QString>& results);
    void clear();

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    struct Node;
    void rebuild();

    std::vector<zpp::EntryInfo> entries_;
    std::vector<satchel_gui::Row> rows_;
    std::unique_ptr<Node> root_;
    QHash<int, QString> verification_;
    bool tree_ = true;
};

class ArchiveFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void set_text(const QString& text);
    void set_method(const QString& method); // "" = all
    void set_restorable_only(bool on);

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    void change(const std::function<void()>& update);

    QString text_;
    QString method_;
    bool restorable_only_ = false;
};
