// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "archive_model.hpp"

#include <QDateTime>
#include <QFileIconProvider>
#include <QMimeData>
#include <QPalette>

#include <map>
#include <utility>

using satchel_gui::Row;

struct ArchiveModel::Node
{
    QString name;
    const Row* row = nullptr; // null for synthesized folders
    bool folder = false;
    Node* parent = nullptr;
    int index_in_parent = 0;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<std::size_t> entries;
    std::uint64_t size = 0;
    std::uint64_t packed = 0;

    Node* add(std::unique_ptr<Node> child)
    {
        child->parent = this;
        child->index_in_parent = static_cast<int>(children.size());
        children.push_back(std::move(child));
        return children.back().get();
    }
};

namespace
{

QString method_key(const QString& method)
{
    if (method.startsWith("FLAC multi"))
        return "flac-mono";
    if (method.startsWith("FLAC"))
        return "flac";
    return method.toLower();
}

}

ArchiveModel::ArchiveModel(QObject* parent) :
    QAbstractItemModel(parent),
    root_(std::make_unique<Node>())
{
}

ArchiveModel::~ArchiveModel() = default;

void ArchiveModel::clear()
{
    beginResetModel();
    entries_.clear();
    rows_.clear();
    root_ = std::make_unique<Node>();
    verification_.clear();
    endResetModel();
}

void ArchiveModel::set_entries(const std::vector<zpp::EntryInfo>& entries)
{
    beginResetModel();
    entries_ = entries;
    verification_.clear();
    std::vector<satchel_gui::ListedEntry> listed;
    listed.reserve(entries.size());
    for (const auto& e : entries)
    {
        satchel_gui::ListedEntry l;
        l.index = e.index;
        l.name = e.name;
        l.directory = e.kind == ZP_KIND_DIRECTORY;
        l.size = e.uncompressed_size;
        l.packed = e.compressed_size;
        l.method = e.flac_channel           ? "FLAC multi-mono"
            : e.flac_restorable             ? "FLAC"
            : !e.supported                  ? "Unsupported"
            : e.kind == ZP_KIND_SYMLINK     ? "Symlink"
            : e.method == ZP_METHOD_DEFLATE ? "Deflate"
                                            : "Store";
        l.restores_to = e.flac_original_name;
        l.mtime = e.mtime;
        if (e.flac_channel)
        {
            const auto slash = e.name.rfind('/');
            l.group = (slash == std::string::npos ? std::string{} : e.name.substr(0, slash + 1)) + "|" + e.flac_original_name;
            l.channel_index = e.flac_channel->first;
            l.channel_count = e.flac_channel->second;
        }
        listed.push_back(std::move(l));
    }
    rows_ = satchel_gui::group_rows(listed);
    rebuild();
    endResetModel();
}

void ArchiveModel::set_tree(bool tree)
{
    if (tree == tree_)
        return;
    beginResetModel();
    tree_ = tree;
    rebuild();
    endResetModel();
}

void ArchiveModel::set_verification(const QHash<int, QString>& results)
{
    verification_ = results;
    if (rowCount() > 0)
        emit dataChanged(index(0, Verified), index(rowCount() - 1, Verified));
}

void ArchiveModel::rebuild()
{
    root_ = std::make_unique<Node>();
    const auto make_row_node = [](const Row& r, const QString& name) {
        auto n = std::make_unique<Node>();
        n->name = name;
        n->row = &r;
        n->entries = r.entries;
        n->size = r.size;
        n->packed = r.packed;
        for (const auto& c : r.children)
        {
            auto child = std::make_unique<Node>();
            child->name = QString::fromStdString(c.name).section('/', -1);
            child->row = &c;
            child->entries = c.entries;
            child->size = c.size;
            child->packed = c.packed;
            n->add(std::move(child));
        }
        return n;
    };
    if (!tree_)
    {
        for (const auto& r : rows_)
            root_->add(make_row_node(r, QString::fromStdString(r.name)));
        return;
    }
    std::map<QString, Node*> folders;
    const auto folder_for = [&](const QString& path) {
        Node* parent = root_.get();
        QString so_far;
        for (const auto& part : path.split('/', Qt::SkipEmptyParts))
        {
            so_far += part + '/';
            auto it = folders.find(so_far);
            if (it == folders.end())
            {
                auto f = std::make_unique<Node>();
                f->name = part;
                f->folder = true;
                it = folders.emplace(so_far, parent->add(std::move(f))).first;
            }
            parent = it->second;
        }
        return parent;
    };
    for (const auto& r : rows_)
    {
        const auto name = QString::fromStdString(r.name);
        if (r.directory)
        {
            auto* f = folder_for(name);
            f->row = &r;
            f->entries = r.entries;
            continue;
        }
        const auto slash = name.lastIndexOf('/', name.indexOf(" — ") < 0 ? -1 : name.indexOf(" — "));
        auto* parent = slash < 0 ? root_.get() : folder_for(name.left(slash));
        parent->add(make_row_node(r, slash < 0 ? name : name.mid(slash + 1)));
    }
    // Folder totals.
    std::function<void(Node*)> total = [&](Node* n) {
        for (auto& c : n->children)
        {
            total(c.get());
            if (n->folder)
            {
                n->size += c->size;
                n->packed += c->packed;
            }
        }
    };
    total(root_.get());
}

QModelIndex ArchiveModel::index(int row, int column, const QModelIndex& parent) const
{
    const Node* p = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : root_.get();
    if (row < 0 || std::cmp_greater_equal(row, p->children.size()) || column < 0 || column >= ColumnCount)
        return {};
    return createIndex(row, column, p->children[static_cast<std::size_t>(row)].get());
}

QModelIndex ArchiveModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return {};
    const auto* n = static_cast<Node*>(child.internalPointer());
    if (!n->parent || n->parent == root_.get())
        return {};
    return createIndex(n->parent->index_in_parent, 0, n->parent);
}

int ArchiveModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0)
        return 0;
    const Node* p = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : root_.get();
    return static_cast<int>(p->children.size());
}

int ArchiveModel::columnCount(const QModelIndex&) const
{
    return ColumnCount;
}

QVariant ArchiveModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    const auto* n = static_cast<Node*>(index.internalPointer());
    const Row* r = n->row;
    const bool folder = n->folder;
    const auto mtime = r ? r->mtime : 0;
    const auto method = r && !r->directory ? QString::fromStdString(r->method) : QString(folder ? "Folder" : "");
    const bool restorable = r && !r->restores_to.empty();
    switch (role)
    {
        case Qt::DisplayRole:
            switch (index.column())
            {
                case Name:
                    return n->name;
                case Size:
                    return folder && !tree_ ? QVariant() : QString::fromStdString(satchel_gui::human_size(n->size));
                case Packed:
                    return folder && !tree_ ? QVariant() : QString::fromStdString(satchel_gui::human_size(n->packed));
                case Ratio:
                    return n->size ? QString("%1%").arg(satchel_gui::percent_saved(n->size, n->packed)) : QString();
                case Method:
                    return method;
                case RestoresTo:
                    return r ? QString::fromStdString(r->restores_to) : QString();
                case Modified:
                    return mtime ? QDateTime::fromSecsSinceEpoch(mtime).toString("yyyy-MM-dd HH:mm") : QString();
                case Verified:
                {
                    QStringList results;
                    for (const auto e : n->entries)
                    {
                        const auto it = verification_.find(static_cast<int>(e));
                        if (it != verification_.end() && !results.contains(*it))
                            results << *it;
                    }
                    return results.join(", ");
                }
                default:
                    return {};
            }
        case SortRole:
            switch (index.column())
            {
                case Size:
                    return QVariant::fromValue<qulonglong>(n->size);
                case Packed:
                    return QVariant::fromValue<qulonglong>(n->packed);
                case Ratio:
                    return satchel_gui::percent_saved(n->size, n->packed);
                case Modified:
                    return QVariant::fromValue<qlonglong>(mtime);
                default:
                    return data(index, Qt::DisplayRole).toString().toLower();
            }
        case Qt::TextAlignmentRole:
            if (index.column() == Size || index.column() == Packed || index.column() == Ratio)
                return { Qt::AlignRight | Qt::AlignVCenter };
            return {};
        case Qt::DecorationRole:
            if (index.column() == Name)
            {
                static QFileIconProvider icons;
                return folder || (r && r->directory) ? icons.icon(QFileIconProvider::Folder) : icons.icon(QFileIconProvider::File);
            }
            return {};
        case Qt::ToolTipRole:
            if (method == "Unsupported")
                return tr("This entry uses a compression method or encryption that cannot be read");
            if (method == "Symlink")
                return tr("Symbolic links are not extracted");
            if (restorable)
                return tr("Restorable: extracts as %1").arg(QString::fromStdString(r->restores_to));
            return {};
        case Qt::ForegroundRole:
            if (method == "Unsupported" || method == "Symlink")
                return QVariant::fromValue(QPalette().color(QPalette::Disabled, QPalette::Text));
            return {};
        case EntriesRole:
        {
            QVariantList list;
            std::function<void(const Node*)> collect = [&](const Node* x) {
                for (const auto e : x->entries)
                    list << QVariant::fromValue<qulonglong>(e);
                if (x->folder)
                {
                    for (const auto& c : x->children)
                        collect(c.get());
                }
            };
            collect(n);
            return list;
        }
        case MethodRole:
            return method_key(method);
        case RestorableRole:
            return restorable;
        case FolderRole:
            return folder || (r && r->directory);
        default:
            return {};
    }
}

QVariant ArchiveModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    static const QStringList names{ tr("Name"), tr("Size"), tr("Packed"), tr("Ratio"), tr("Method"), tr("Restores to"), tr("Modified"), tr("Verified") };
    return names.value(section);
}

Qt::ItemFlags ArchiveModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList ArchiveModel::mimeTypes() const
{
    return { "application/x-satchel-entries" };
}

QMimeData* ArchiveModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList ids;
    for (const auto& i : indexes)
    {
        if (i.column() != 0)
            continue;
        for (const auto& v : data(i, EntriesRole).toList())
            ids << v.toString();
    }
    auto* m = new QMimeData;
    m->setData("application/x-satchel-entries", ids.join(',').toUtf8());
    return m;
}

void ArchiveFilter::change(const std::function<void()>& update)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    update();
    endFilterChange();
#else
    update();
    invalidateFilter();
#endif
}

void ArchiveFilter::set_text(const QString& text)
{
    change([&] { text_ = text; });
}

void ArchiveFilter::set_method(const QString& method)
{
    change([&] { method_ = method; });
}

void ArchiveFilter::set_restorable_only(bool on)
{
    change([&] { restorable_only_ = on; });
}

bool ArchiveFilter::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    const auto idx = sourceModel()->index(row, 0, parent);
    if (sourceModel()->data(idx, ArchiveModel::FolderRole).toBool())
    {
        for (int i = 0; i < sourceModel()->rowCount(idx); ++i)
        {
            if (filterAcceptsRow(i, idx))
                return true;
        }
        return text_.isEmpty() && method_.isEmpty() && !restorable_only_;
    }
    if (!text_.isEmpty() && !sourceModel()->data(idx, Qt::DisplayRole).toString().contains(text_, Qt::CaseInsensitive)
        && !sourceModel()->data(sourceModel()->index(row, ArchiveModel::RestoresTo, parent), Qt::DisplayRole).toString().contains(text_, Qt::CaseInsensitive))
        return false;
    if (!method_.isEmpty() && sourceModel()->data(idx, ArchiveModel::MethodRole).toString() != method_
        && !(parent.isValid() && sourceModel()->data(parent, ArchiveModel::MethodRole).toString() == method_))
        return false;
    if (restorable_only_ && !sourceModel()->data(idx, ArchiveModel::RestorableRole).toBool())
        return parent.isValid() && sourceModel()->data(parent, ArchiveModel::RestorableRole).toBool();
    return true;
}
