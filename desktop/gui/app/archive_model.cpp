// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "archive_model.hpp"

#include "labels.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFileIconProvider>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>

using satchel_gui::Row;
using satchel_gui::RowState;

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
    bool ratio_known = true;
    bool packed_known = true;
    std::optional<RowState> state; // folders: set when every entry below shares it

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

satchel_gui::ListedEntry ArchiveModel::listed(const zpp::EntryInfo& e)
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
    l.original_size = e.original_size;
    l.mtime = e.mtime;
    if (e.flac_channel)
    {
        const auto slash = e.name.rfind('/');
        l.group = (slash == std::string::npos ? std::string{} : e.name.substr(0, slash + 1)) + "|" + e.flac_original_name;
        l.channel_index = e.flac_channel->first;
        l.channel_count = e.flac_channel->second;
    }
    return l;
}

void ArchiveModel::set_entries(const std::vector<zpp::EntryInfo>& entries)
{
    std::vector<BrowserEntry> list;
    list.reserve(entries.size());
    for (const auto& e : entries)
    {
        auto l = listed(e);
        l.index = list.size();
        BrowserEntry be;
        be.listed = std::move(l);
        list.push_back(std::move(be));
    }
    set_entries(std::move(list));
}

void ArchiveModel::set_entries(std::vector<BrowserEntry> entries)
{
    beginResetModel();
    entries_ = std::move(entries);
    verification_.clear();
    std::vector<satchel_gui::ListedEntry> listed;
    listed.reserve(entries_.size());
    for (const auto& e : entries_)
        listed.push_back(e.listed);
    rows_ = satchel_gui::group_rows(listed);
    rebuild();
    endResetModel();
}

const BrowserEntry* ArchiveModel::entry_of(const Node* n) const
{
    if (!n->row || n->row->entries.empty())
        return nullptr;
    const auto i = n->row->entries.front();
    return i < entries_.size() ? &entries_[i] : nullptr;
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
    const auto pending = [this](const Row& r) {
        return std::ranges::any_of(r.entries, [this](std::size_t i) { return i < entries_.size() && entries_[i].pending; });
    };
    const auto make_row_node = [&](const Row& r, const QString& name) {
        auto n = std::make_unique<Node>();
        n->name = name;
        n->row = &r;
        n->entries = r.entries;
        n->size = r.size;
        n->packed = r.packed;
        n->ratio_known = r.ratio_known;
        n->packed_known = !pending(r);
        for (const auto& c : r.children)
        {
            auto child = std::make_unique<Node>();
            child->name = QString::fromStdString(c.name).section('/', -1);
            child->row = &c;
            child->entries = c.entries;
            child->size = c.size;
            child->packed = c.packed;
            child->ratio_known = c.ratio_known;
            child->packed_known = !pending(c);
            n->add(std::move(child));
        }
        return n;
    };
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
        const bool group = r.channel_count > 0 && r.channel_index == 0;
        const auto path = group ? QString::fromStdString(r.group_name) : name;
        const auto slash = path.lastIndexOf('/');
        auto* parent = slash < 0 ? root_.get() : folder_for(path.left(slash));
        const auto leaf = slash < 0 ? path : path.mid(slash + 1);
        parent->add(make_row_node(r, group ? labels::group_label(leaf, r.channel_count) : leaf));
    }
    // Folder totals; deleted entries do not count. A folder takes the state its entries share.
    std::function<std::optional<RowState>(Node*)> total = [&](Node* n) -> std::optional<RowState> {
        const auto* own = entry_of(n);
        std::optional<RowState> shared = own ? std::optional(own->state) : std::nullopt;
        bool first = !own;
        for (auto& c : n->children)
        {
            const auto child = total(c.get());
            if (first)
                shared = child;
            else if (shared != child)
                shared = std::nullopt;
            first = false;
            if (n->folder && child != RowState::Deleted)
            {
                n->size += c->size;
                n->packed += c->packed;
                n->ratio_known = n->ratio_known && c->ratio_known;
                n->packed_known = n->packed_known && c->packed_known;
            }
        }
        if (n->folder)
            n->state = shared;
        return own && !n->folder ? std::optional(own->state) : shared;
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
    const auto* be = folder ? nullptr : entry_of(n);
    const int state = folder ? (n->state ? static_cast<int>(*n->state) : -1) : be ? static_cast<int>(be->state)
                                                                                  : -1;
    const bool deleted = state == static_cast<int>(RowState::Deleted);
    const bool renamed = state == static_cast<int>(RowState::Renamed);
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
                    return QString::fromStdString(satchel_gui::human_size(n->size));
                case Packed:
                    return !n->packed_known ? QVariant() : QString::fromStdString(satchel_gui::human_size(n->packed));
                case Ratio:
                    return n->size && n->ratio_known && n->packed_known ? QString("%1%").arg(satchel_gui::percent_saved(n->size, n->packed)) : QString();
                case Method:
                    return r && !r->directory ? labels::row_method(*r) : folder ? labels::method("Folder")
                                                                                : QString();
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
                    return !n->ratio_known ? std::numeric_limits<int>::min() : satchel_gui::percent_saved(n->size, n->packed);
                case Modified:
                    return QVariant::fromValue<qlonglong>(mtime);
                default:
                    return data(index, Qt::DisplayRole).toString().toLower();
            }
        case Qt::TextAlignmentRole:
            if (index.column() == Size || index.column() == Packed || index.column() == Ratio)
                return (Qt::AlignRight | Qt::AlignVCenter).toInt();
            return {};
        case Qt::DecorationRole:
            if (index.column() == Name)
            {
                if (be && be->conflicted)
                    return QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
                static QFileIconProvider icons;
                return folder || (r && r->directory) ? icons.icon(QFileIconProvider::Folder) : icons.icon(QFileIconProvider::File);
            }
            return {};
        case Qt::FontRole:
            if (deleted || (index.column() == Name && renamed))
            {
                QFont f;
                f.setStrikeOut(deleted);
                f.setItalic(renamed);
                return f;
            }
            return {};
        case Qt::ToolTipRole:
            if (be && be->conflicted)
                return tr("Another entry has the same name; see Issues");
            if (deleted)
                return tr("Will be removed when the changes are applied");
            if (be && be->state == RowState::Renamed)
                return tr("Renamed from %1").arg(be->previous_name);
            if (method == "Unsupported")
                return tr("This entry uses a compression method or encryption that cannot be read");
            if (method == "Symlink")
                return tr("Symbolic links are not extracted");
            if (restorable)
                return tr("Restorable: extracts as %1").arg(QString::fromStdString(r->restores_to));
            return {};
        case Qt::ForegroundRole:
            if (method == "Unsupported" || method == "Symlink" || deleted)
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
        case StateRole:
            return state;
        case GroupRole:
            return r && r->channel_count > 0 && r->channel_index == 0;
        case PathRole:
        {
            if (r && r->channel_count > 0 && r->channel_index == 0)
                return QString::fromStdString(r->group_name);
            if (r)
            {
                auto path = QString::fromStdString(r->name);
                while (path.endsWith('/'))
                    path.chop(1);
                return path;
            }
            QStringList parts;
            for (const auto* x = n; x && x != root_.get(); x = x->parent)
                parts.prepend(x->name);
            return parts.join('/');
        }
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
    auto f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    const auto* n = static_cast<Node*>(index.internalPointer());
    const auto* r = n->row;
    const bool member = r && r->channel_count > 0 && r->channel_index > 0;
    const auto st = data(index, StateRole).toInt();
    if (editable_ && index.column() == Name && !member && st != static_cast<int>(RowState::Deleted) && (n->folder || r))
        f |= Qt::ItemIsEditable;
    return f;
}

bool ArchiveModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || index.column() != Name)
        return false;
    const auto name = value.toString().trimmed();
    const auto* n = static_cast<Node*>(index.internalPointer());
    const auto current = n->row && n->row->channel_count > 0 && n->row->channel_index == 0 ? QString::fromStdString(n->row->group_name).section('/', -1) : n->name;
    if (name.isEmpty() || name == current)
        return false;
    emit rename_requested(index, name);
    return false;
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

void ArchiveFilter::set_show_deleted(bool on)
{
    change([&] { show_deleted_ = on; });
}

bool ArchiveFilter::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    const auto idx = sourceModel()->index(row, 0, parent);
    if (!show_deleted_ && sourceModel()->data(idx, ArchiveModel::StateRole).toInt() == static_cast<int>(RowState::Deleted))
        return false;
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

void BadgeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const auto state = index.data(ArchiveModel::StateRole).toInt();
    QString badge;
    if (index.column() == ArchiveModel::Name && state == static_cast<int>(RowState::Added))
        badge = tr("New");
    else if (index.column() == ArchiveModel::Name && state == static_cast<int>(RowState::Replaced))
        badge = tr("Replaced");
    if (badge.isEmpty())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    QFont small = opt.font;
    small.setPointSizeF(small.pointSizeF() * 0.85);
    const QFontMetrics fm(small);
    const int pad = 6;
    const int w = fm.horizontalAdvance(badge) + 2 * pad;
    const int h = fm.height() + 2;
    const auto* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);
    opt.rect.setRight(opt.rect.right() - w - 2 * pad);
    opt.backgroundBrush = Qt::NoBrush;
    opt.state &= ~QStyle::State_HasFocus;
    opt.features &= ~QStyleOptionViewItem::Alternate;
    opt.palette.setColor(QPalette::Highlight, Qt::transparent);
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
    const QRect pill(option.rect.right() - pad - w, option.rect.center().y() - h / 2, w, h);
    const bool selected = option.state & QStyle::State_Selected;
    auto fill = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Highlight);
    if (selected)
        fill.setAlpha(60);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(pill, h / 2.0, h / 2.0);
    painter->fillPath(path, fill);
    painter->setPen(option.palette.color(QPalette::HighlightedText));
    painter->setFont(small);
    painter->drawText(pill, Qt::AlignCenter, badge);
    painter->restore();
}

QSize BadgeDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    auto size = QStyledItemDelegate::sizeHint(option, index);
    const auto state = index.data(ArchiveModel::StateRole).toInt();
    if (index.column() == ArchiveModel::Name && (state == static_cast<int>(RowState::Added) || state == static_cast<int>(RowState::Replaced)))
        size.rwidth() += QFontMetrics(option.font).horizontalAdvance(tr("Replaced")) + 18;
    return size;
}
