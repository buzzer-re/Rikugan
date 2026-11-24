#include "common/ui/console/Palette.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "common/ui/Theme.h"

namespace rk::ui {

namespace {

struct Row {
    bool heading = false;
    int entry = -1;  // entries_ index (heading: the section's first entry)
    QList<int> match;
};

int headingHeight() { return std::max(18, theme().rowHeight - 2); }
int entryHeight() { return theme().rowHeight + 4; }

}  // namespace

class PaletteModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

    QList<PaletteEntry> entries;
    QList<Row> rows;
    QString query;

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(rows.size()); }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rows.size()) return {};
        const Row& r = rows[index.row()];
        if (role == Qt::DisplayRole) return r.heading ? entries[r.entry].section : entries[r.entry].title;
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid() || index.row() >= rows.size()) return Qt::NoItemFlags;
        const Row& r = rows[index.row()];
        if (r.heading || !entries[r.entry].enabled) return Qt::ItemIsEnabled;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    void rebuild() {
        beginResetModel();
        rows.clear();
        // sections in first-seen order; entries ranked by score inside each when there is a query
        QStringList order;
        QHash<QString, QList<std::pair<int, int>>> bySection;  // section -> (score, entry)
        QHash<int, QList<int>> matches;
        for (int i = 0; i < entries.size(); ++i) {
            const PaletteEntry& e = entries[i];
            if (!order.contains(e.section)) order << e.section;
            int score = 0;
            if (!query.isEmpty()) {
                QList<int> idx;
                score = fuzzyScore(query, e.title, &idx);
                const int extra = e.search.isEmpty() ? -1 : fuzzyScore(query, e.search);
                if (score < 0 && extra < 0) continue;
                if (score >= 0) matches.insert(i, idx);
                score = std::max(score, extra - 5);
            }
            bySection[e.section].append({score, i});
        }
        for (const QString& s : std::as_const(order)) {
            QList<std::pair<int, int>> list = bySection.value(s);
            if (list.isEmpty()) {
                // a section with an action (a provider to connect) stays visible without entries when unfiltered
                continue;
            }
            if (!query.isEmpty())
                std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (!s.isEmpty()) {
                int first = list.first().second;
                for (int i = 0; i < entries.size(); ++i)
                    if (entries[i].section == s) {
                        first = i;
                        break;
                    }
                rows.append(Row{true, first, {}});
            }
            for (const auto& [score, i] : list) {
                Q_UNUSED(score);
                if (entries[i].title.isEmpty()) continue;  // a section placeholder (heading only)
                rows.append(Row{false, i, matches.value(i)});
            }
        }
        endResetModel();
    }
};

namespace {

class PaletteDelegate : public QStyledItemDelegate {
public:
    PaletteDelegate(PaletteModel* m, QObject* parent) : QStyledItemDelegate(parent), m_(m) {}

    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& index) const override {
        const Row& r = m_->rows[index.row()];
        return {o.rect.width(), r.heading ? headingHeight() : entryHeight()};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& index) const override {
        const Theme& t = theme();
        const Row& r = m_->rows[index.row()];
        const PaletteEntry& e = m_->entries[r.entry];
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect rect = o.rect.adjusted(4, 0, -4, 0);
        if (r.heading) {
            QFont f = t.uiSmall;
            f.setWeight(QFont::DemiBold);
            f.setCapitalization(QFont::AllUppercase);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            p->setFont(f);
            p->setPen(t.fg3);
            const QRect hr = rect.adjusted(8, 2, -8, 0);
            p->drawText(hr, Qt::AlignLeft | Qt::AlignVCenter, e.section);
            int x = hr.left() + QFontMetrics(f).horizontalAdvance(e.section) + 10;
            if (!e.sectionNote.isEmpty()) {
                p->setFont(t.uiSmall);
                p->setPen(e.sectionNote.startsWith(QStringLiteral("\u25CF")) ? t.ok : t.fg3);
                p->drawText(QRect(x, hr.top(), hr.right() - x, hr.height()), Qt::AlignLeft | Qt::AlignVCenter, e.sectionNote);
            }
            if (!e.sectionAction.isEmpty()) {
                QFont af = t.uiSmall;
                af.setWeight(QFont::DemiBold);
                p->setFont(af);
                p->setPen(t.accent);
                p->drawText(hr, Qt::AlignRight | Qt::AlignVCenter, e.sectionAction);
            }
            p->restore();
            return;
        }
        const bool selected = o.state & QStyle::State_Selected;
        const bool hover = o.state & QStyle::State_MouseOver;
        if (selected || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? t.accentSoft2 : t.bg3);
            p->drawRoundedRect(QRectF(rect).adjusted(0.5, 1.5, -0.5, -1.5), 5, 5);
        }
        int x = rect.left() + 10;
        const QColor ink = e.enabled ? t.fg : t.fg3;
        if (e.checked || !e.glyph.isEmpty()) {
            p->setFont(t.ui);
            p->setPen(e.checked ? t.accent : t.fg3);
            p->drawText(QRect(x, rect.top(), 16, rect.height()), Qt::AlignCenter, e.checked ? QStringLiteral("\u2713") : e.glyph);
            x += 22;
        }
        // title with the matched letters in the accent
        const QFont tf = e.mono ? t.mono : t.ui;
        QFont bold = tf;
        bold.setWeight(QFont::Bold);
        const QFontMetrics fm(tf), fb(bold);
        const QString hint = e.hint;
        const int hintW = hint.isEmpty() ? 0 : QFontMetrics(t.monoSmall).horizontalAdvance(hint) + 12;
        const int limit = rect.right() - 10 - hintW;
        // runs of matched / unmatched text (never splitting a surrogate pair), the last one elided at the limit
        for (qsizetype i = 0; i < e.title.size() && x < limit;) {
            const bool m = r.match.contains(int(i));
            qsizetype j = i + 1;
            while (j < e.title.size() && (e.title.at(j).isLowSurrogate() || r.match.contains(int(j)) == m)) ++j;
            const QFontMetrics& f = m ? fb : fm;
            const QString run = f.elidedText(e.title.mid(i, j - i), Qt::ElideRight, limit - x);
            p->setFont(m ? bold : tf);
            p->setPen(m ? t.accent : ink);
            const int w = f.horizontalAdvance(run);
            p->drawText(QRect(x, rect.top(), w + 1, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, run);
            x += w;
            i = j;
        }
        if (!e.detail.isEmpty() && x + 20 < limit) {
            p->setFont(t.uiSmall);
            p->setPen(t.fg3);
            const QRect dr(x + 10, rect.top(), limit - x - 10, rect.height());
            p->drawText(dr, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(t.uiSmall).elidedText(e.detail, Qt::ElideRight, dr.width()));
        }
        if (!hint.isEmpty()) {
            p->setFont(t.monoSmall);
            p->setPen(t.fg3);
            p->drawText(QRect(limit, rect.top(), rect.right() - 10 - limit, rect.height()), Qt::AlignRight | Qt::AlignVCenter, hint);
        }
        p->restore();
    }

private:
    PaletteModel* m_;
};

}  // namespace

// ---- PaletteList ----

PaletteList::PaletteList(QWidget* parent) : QListView(parent) {
    model_ = new PaletteModel(this);
    setModel(model_);
    setItemDelegate(new PaletteDelegate(model_, this));
    setFrameShape(QFrame::NoFrame);
    setUniformItemSizes(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(QStringLiteral("QListView { background: transparent; border: none; }"));
}

void PaletteList::setEntries(const QList<PaletteEntry>& entries) {
    const QString keep = currentKey();
    model_->entries = entries;
    model_->rebuild();
    // the selection survives a refresh of the same list (the model picker updates while open)
    for (int r = 0; r < model_->rows.size(); ++r)
        if (!model_->rows[r].heading && model_->entries[model_->rows[r].entry].key == keep && !keep.isEmpty()) {
            setCurrentIndex(model_->index(r));
            updateGeometry();
            return;
        }
    selectFirst();
    updateGeometry();
}

const QList<PaletteEntry>& PaletteList::entries() const { return model_->entries; }

void PaletteList::setQuery(const QString& query) {
    if (query == model_->query) return;
    model_->query = query;
    model_->rebuild();
    selectFirst();
    updateGeometry();
}

QString PaletteList::query() const { return model_->query; }

int PaletteList::visibleCount() const {
    int n = 0;
    for (const Row& r : std::as_const(model_->rows)) n += r.heading ? 0 : 1;
    return n;
}

void PaletteList::selectFirst() {
    for (int r = 0; r < model_->rows.size(); ++r)
        if (model_->flags(model_->index(r)) & Qt::ItemIsSelectable) {
            setCurrentIndex(model_->index(r));
            scrollTo(model_->index(r));
            return;
        }
    setCurrentIndex({});
}

void PaletteList::moveSelection(int delta) {
    const int n = int(model_->rows.size());
    if (!n) return;
    int r = currentIndex().isValid() ? currentIndex().row() : (delta > 0 ? -1 : n);
    for (int step = 0; step < n; ++step) {
        r = (r + delta + n) % n;
        if (model_->flags(model_->index(r)) & Qt::ItemIsSelectable) {
            setCurrentIndex(model_->index(r));
            scrollTo(model_->index(r));
            return;
        }
    }
}

QString PaletteList::currentKey() const {
    const QModelIndex i = currentIndex();
    if (!i.isValid() || i.row() >= model_->rows.size() || model_->rows[i.row()].heading) return {};
    return model_->entries[model_->rows[i.row()].entry].key;
}

bool PaletteList::activateCurrent() {
    const QString key = currentKey();
    if (key.isEmpty()) return false;
    Q_EMIT activated(key);
    return true;
}

void PaletteList::setMaxRows(int rows) {
    maxRows_ = rows;
    updateGeometry();
}

QSize PaletteList::sizeHint() const {
    int h = 0, entries = 0;
    for (const Row& r : std::as_const(model_->rows)) {
        if (!r.heading && ++entries > maxRows_) break;
        h += r.heading ? headingHeight() : entryHeight();
    }
    return {QListView::sizeHint().width(), std::max(h, entryHeight()) + 4};
}

void PaletteList::mouseMoveEvent(QMouseEvent* e) {
    const QModelIndex i = indexAt(e->position().toPoint());
    if (i.isValid() && (model_->flags(i) & Qt::ItemIsSelectable)) setCurrentIndex(i);
    QListView::mouseMoveEvent(e);
}

void PaletteList::mousePressEvent(QMouseEvent* e) { e->accept(); }

void PaletteList::mouseReleaseEvent(QMouseEvent* e) {
    const QModelIndex i = indexAt(e->position().toPoint());
    if (!i.isValid() || e->button() != Qt::LeftButton) return;
    const Row& r = model_->rows[i.row()];
    if (r.heading) {
        const PaletteEntry& entry = model_->entries[r.entry];
        if (!entry.sectionAction.isEmpty()) Q_EMIT sectionActionActivated(entry.section);
        return;
    }
    if (model_->flags(i) & Qt::ItemIsSelectable) {
        setCurrentIndex(i);
        activateCurrent();
    }
}

// ---- PalettePopup ----

PalettePopup::PalettePopup(QWidget* parent, bool search, const QString& placeholder) : QFrame(parent) {
    setObjectName(QStringLiteral("rk_popup"));
    setAttribute(Qt::WA_StyledBackground);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(4, 4, 4, 4);
    layout_->setSpacing(2);
    if (search) {
        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("rk_popup_search"));
        search_->setPlaceholderText(placeholder);
        search_->setClearButtonEnabled(false);
        search_->installEventFilter(this);
        layout_->addWidget(search_);
        connect(search_, &QLineEdit::textChanged, this, [this](const QString& q) {
            list_->setQuery(q.trimmed());
            place();
        });
    }
    list_ = new PaletteList(this);
    layout_->addWidget(list_, 1);
    hide();
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &PalettePopup::restyle);
    restyle();
}

void PalettePopup::setHeaderWidget(QWidget* w) {
    header_ = w;
    layout_->insertWidget(search_ ? 1 : 0, w);
}

void PalettePopup::setFooterWidget(QWidget* w) {
    footer_ = w;
    layout_->addWidget(w);
}

void PalettePopup::restyle() {
    const Theme& t = theme();
    setStyleSheet(QStringLiteral("#rk_popup { background: %1; border: 1px solid %2; border-radius: 8px; }"
                                 "#rk_popup_search { background: %1; border: none; border-bottom: 1px solid %3; border-radius: 0;"
                                 " padding: 6px 8px; color: %4; }")
                      .arg(t.bg.name(), t.line2.name(), t.accent.name(), t.fg.name()));
}

void PalettePopup::popupAt(QWidget* anchor, bool above, int width) {
    anchor_ = anchor;
    above_ = above;
    width_ = width;
    if (search_) {
        search_->clear();
        list_->setQuery({});
    }
    show();
    raise();
    place();
    if (search_) search_->setFocus();
    qApp->installEventFilter(this);
}

void PalettePopup::place() {
    QWidget* area = parentWidget();
    if (!area || !isVisible()) return;
    const int margin = 8;
    const int w = width_ > 0 ? std::min(width_, area->width() - 2 * margin) : area->width() - 2 * margin;
    layout_->activate();
    int h = layout_->hasHeightForWidth() ? layout_->totalHeightForWidth(w) : layout_->totalSizeHint().height();
    QRect a = anchor_ ? QRect(anchor_->mapTo(area, QPoint(0, 0)), anchor_->size()) : QRect(0, area->height(), area->width(), 0);
    int y = above_ ? a.top() - h - 4 : a.bottom() + 4;
    const int maxH = above_ ? a.top() - margin - 4 : area->height() - a.bottom() - margin - 4;
    if (h > maxH && maxH > 80) {
        h = maxH;
        y = above_ ? a.top() - h - 4 : a.bottom() + 4;
    }
    int x = width_ > 0 ? std::clamp(a.left(), margin, area->width() - w - margin) : margin;
    setGeometry(x, std::max(margin, y), w, h);
}

void PalettePopup::dismiss() {
    if (!isVisible()) return;
    qApp->removeEventFilter(this);
    hide();
    Q_EMIT dismissed();
}

bool PalettePopup::handleKey(int key) {
    if (!isVisible()) return false;
    switch (key) {
    case Qt::Key_Down: list_->moveSelection(1); return true;
    case Qt::Key_Up: list_->moveSelection(-1); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab: return list_->activateCurrent();
    case Qt::Key_Escape: dismiss(); return true;
    default: return false;
    }
}

void PalettePopup::keyPressEvent(QKeyEvent* e) {
    if (handleKey(e->key())) return;
    QFrame::keyPressEvent(e);
}

bool PalettePopup::eventFilter(QObject* o, QEvent* e) {
    if (o == search_ && e->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Down || key == Qt::Key_Up || key == Qt::Key_Return || key == Qt::Key_Enter ||
            key == Qt::Key_Escape)
            return handleKey(key);
    }
    // a press outside the popup and its anchor closes it
    if (e->type() == QEvent::MouseButtonPress && isVisible()) {
        QWidget* w = qobject_cast<QWidget*>(o);
        if (w && w->isWindow() == false && !isAncestorOf(w) && w != this && !(anchor_ && (w == anchor_ || anchor_->isAncestorOf(w))))
            dismiss();
    }
    return false;
}

}  // namespace rk::ui
