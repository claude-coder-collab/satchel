// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"
#include "session.hpp"
#include "zp_cpp.hpp"

#include <QAbstractItemModel>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

#include <functional>
#include <memory>
#include <vector>

// One row of the browser before grouping: an archive entry or a staged change.
struct BrowserEntry
{
    satchel_gui::ListedEntry listed; // listed.index = position in the list given to set_entries
    satchel_gui::RowState state = satchel_gui::RowState::Unchanged;
    bool pending = false; // packed size not known yet (staged additions)
    bool conflicted = false;
    QString previous_name;
};

// The open archive, with staged changes, as rows: a flat list or a folder tree; multi-mono groups
// are one row that expands to its members.
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
    static constexpr int StateRole = Qt::UserRole + 6; // satchel_gui::RowState as int
    static constexpr int PathRole = Qt::UserRole + 7; // full path of a folder or entry (group: restored name)
    static constexpr int GroupRole = Qt::UserRole + 8; // multi-mono group row

    explicit ArchiveModel(QObject* parent = nullptr);
    ~ArchiveModel() override;

    void set_entries(std::vector<BrowserEntry> entries);
    // Entries of an archive without staged changes.
    void set_entries(const std::vector<zpp::EntryInfo>& entries);
    [[nodiscard]] static satchel_gui::ListedEntry listed(const zpp::EntryInfo& e);
    // Allows renaming in place (Name column); edits are reported, not applied.
    void set_editable(bool editable) { editable_ = editable; }
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
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

signals:
    void rename_requested(const QModelIndex& index, const QString& new_leaf_name);

private:
    struct Node;
    void rebuild();
    [[nodiscard]] const BrowserEntry* entry_of(const Node* n) const;

    std::vector<BrowserEntry> entries_;
    std::vector<satchel_gui::Row> rows_;
    std::unique_ptr<Node> root_;
    QHash<int, QString> verification_;
    bool tree_ = true;
    bool editable_ = false;
};

// Name column: "New" / "Replaced" badges after the name.
class BadgeDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

class ArchiveFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void set_text(const QString& text);
    void set_method(const QString& method); // "" = all
    void set_restorable_only(bool on);
    void set_show_deleted(bool on);

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    void change(const std::function<void()>& update);

    QString text_;
    QString method_;
    bool restorable_only_ = false;
    bool show_deleted_ = true;
};
