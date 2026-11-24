#include "common/ui/transcript/TranscriptView.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextTable>
#include <QUrl>

#include "common/tools/Tool.h"
#include "common/ui/Syntax.h"
#include "common/ui/Theme.h"
#include "common/ui/transcript/Cards.h"

namespace rk::ui {

namespace {

constexpr int kRail = 14;      // x of the timeline rail
constexpr int kIndent = 28;    // left edge of row content
constexpr int kRightPad = 12;
constexpr int kNode = 4;       // timeline node radius
constexpr int kDetailLines = 60;
constexpr int kSpinMs = 16;        // the spinner's frame: as smooth as the display, only while something runs
constexpr int kSpinTurnMs = 900;   // one full turn

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString elapsed(qint64 ms) {
    if (ms < 0) ms = 0;
    if (ms < 60000) return QString::number(double(ms) / 1000.0, 'f', 1) + QLatin1Char('s');
    return QStringLiteral("%1:%2").arg(ms / 60000).arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
}

QString tokens(qint64 n) {
    if (n < 1000) return QString::number(n);
    if (n < 1000000) return QString::number(double(n) / 1000.0, 'f', 1) + QLatin1Char('k');
    return QString::number(double(n) / 1e6, 'f', 1) + QLatin1Char('M');
}

QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

bool isCardKind(ItemKind k) {
    return k == ItemKind::Permission || k == ItemKind::Approval || k == ItemKind::Question || k == ItemKind::Dialog ||
           k == ItemKind::Decision;
}

QString chipHref(const Chip& c) { return QStringLiteral("rk:%1:%2").arg(c.kind, c.value); }

// Chips as Markdown links (styled as chips after the import).
QString linkify(const QString& text, const QList<Chip>& chips) {
    if (chips.isEmpty()) return text;
    QString out;
    out.reserve(text.size() + chips.size() * 24);
    int pos = 0;
    for (const Chip& c : chips) {
        if (c.from < pos || c.to > text.size()) continue;
        out += QStringView(text).mid(pos, c.from - pos);
        QString label = text.mid(c.from, c.to - c.from);
        if (label.startsWith(QLatin1Char('`'))) label = label.mid(1, label.size() - 2);
        label.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        // a function name reads as one: ƒ before it, like the host's own symbol view
        if (c.kind == QLatin1String("sym")) label.prepend(QStringLiteral("\u0192"));
        out += QStringLiteral("[%1](%2)").arg(label, chipHref(c));
        pos = c.to;
    }
    out += QStringView(text).mid(pos);
    return out;
}

QString languageOfFence(const QString& lang) {
    const QString l = lang.toLower();
    if (l == QLatin1String("python") || l == QLatin1String("py")) return kLanguagePython;
    if (l == QLatin1String("c") || l == QLatin1String("cpp") || l == QLatin1String("c++") || l == QLatin1String("h"))
        return kLanguageC;
    if (l == QLatin1String("sh") || l == QLatin1String("bash") || l == QLatin1String("shell")) return kLanguageShell;
    if (l == QLatin1String("json")) return kLanguageJson;
    if (l == QLatin1String("yaml") || l == QLatin1String("toml") || l == QLatin1String("ini")) return kLanguageConfig;
    return kLanguagePlain;
}

// Theme colours over what the Markdown importer produced: code blocks with syntax colours, inline code, chips,
// links and tables.
void polishMarkdown(QTextDocument* d) {
    const Theme& t = theme();
    struct Span {
        int from, len;
        QTextCharFormat fmt;
    };
    QList<Span> spans;
    QList<std::pair<QTextBlock, QString>> codeBlocks;
    for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        if (bf.hasProperty(QTextFormat::BlockCodeFence) || bf.nonBreakableLines()) {
            codeBlocks.append({b, bf.stringProperty(QTextFormat::BlockCodeLanguage)});
            continue;
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            const QTextCharFormat cf = f.charFormat();
            QTextCharFormat fmt;
            if (cf.isAnchor()) {
                const QString href = cf.anchorHref();
                fmt.setFontUnderline(false);
                if (href.startsWith(QLatin1String("rk:"))) {
                    const QColor ink = href.startsWith(QLatin1String("rk:addr")) ? t.addr : t.sym;
                    fmt.setForeground(ink);
                    fmt.setBackground(blend(ink, t.bg, t.dark ? 0.84 : 0.9));
                    fmt.setFontFamilies(t.mono.families());
                    fmt.setFontPointSize(t.monoSmall.pointSizeF() > 0 ? t.monoSmall.pointSizeF() : t.ui.pointSizeF());
                } else {
                    fmt.setForeground(t.accent);
                }
            } else if (cf.fontFixedPitch()) {
                fmt.setBackground(t.bg3);
                fmt.setFontFamilies(t.mono.families());
                if (t.monoSmall.pointSizeF() > 0) fmt.setFontPointSize(t.monoSmall.pointSizeF());
            } else {
                continue;
            }
            spans.append({f.position(), f.length(), fmt});
        }
    }
    QTextCursor c(d);
    for (const Span& s : std::as_const(spans)) {
        c.setPosition(s.from);
        c.setPosition(s.from + s.len, QTextCursor::KeepAnchor);
        c.mergeCharFormat(s.fmt);
    }
    int state = -1;
    QString lastLang;
    for (const auto& [block, lang] : codeBlocks) {
        QTextBlockFormat bf = block.blockFormat();
        bf.setBackground(t.codeBg);
        bf.setLeftMargin(8);
        bf.setRightMargin(8);
        QTextCursor bc(block);
        bc.setBlockFormat(bf);
        bc.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QTextCharFormat mono;
        mono.setFontFamilies(t.mono.families());
        if (t.monoSmall.pointSizeF() > 0) mono.setFontPointSize(t.monoSmall.pointSizeF());
        bc.mergeCharFormat(mono);
        const QString language = languageOfFence(lang);
        if (language != lastLang) state = -1;
        lastLang = language;
        if (language.isEmpty()) continue;
        for (const CodeSpan& sp : scanLine(language, block.text(), state, &state)) {
            QTextCursor sc(block);
            sc.setPosition(block.position() + sp.start);
            sc.setPosition(block.position() + sp.start + sp.length, QTextCursor::KeepAnchor);
            sc.mergeCharFormat(syntaxFormat(sp.kind));
        }
    }
    for (QTextFrame* fr : d->rootFrame()->childFrames()) {
        if (auto* table = qobject_cast<QTextTable*>(fr)) {
            QTextTableFormat tf = table->format();
            tf.setBorder(1);
            tf.setBorderBrush(t.line2);
            tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
            tf.setBorderCollapse(true);
            tf.setCellPadding(4);
            tf.setCellSpacing(0);
            table->setFormat(tf);
        }
    }
}

// The document the selection overlay shows on top of its row: painting it twice doubles every glyph.
const QTextDocument* g_hiddenDoc = nullptr;

void drawDoc(QPainter* p, QTextDocument* d, const QPoint& at, const QColor& color) {
    if (d == g_hiddenDoc) return;
    p->save();
    p->translate(at);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, color);
    ctx.palette.setColor(QPalette::Link, theme().accent);
    ctx.clip = QRectF(0, 0, d->textWidth(), d->size().height());
    d->documentLayout()->draw(p, ctx);
    p->restore();
}

void roundRect(QPainter* p, const QRectF& r, const QColor& fill, const QColor& border = {}, qreal radius = -1) {
    p->save();
    p->setPen(border.isValid() ? QPen(border, 1) : Qt::NoPen);
    p->setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
    const qreal rr = radius < 0 ? theme().radius : radius;
    p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rr, rr);
    p->restore();
}

void spinner(QPainter* p, const QRectF& r, const QColor& c, int phase) {
    p->save();
    p->setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
    p->setBrush(Qt::NoBrush);
    p->drawArc(r.adjusted(1.5, 1.5, -1.5, -1.5), -phase * 16, 270 * 16);
    p->restore();
}

void statusGlyph(QPainter* p, const QRect& r, ToolStatus s, int phase) {
    const Theme& t = theme();
    switch (s) {
    case ToolStatus::Running:
    case ToolStatus::Pending: spinner(p, r, t.accent, phase); return;
    case ToolStatus::Done: statusIcon(QStringLiteral("done")).paint(p, r); return;
    case ToolStatus::Error: statusIcon(QStringLiteral("error")).paint(p, r); return;
    case ToolStatus::Denied: statusIcon(QStringLiteral("denied")).paint(p, r); return;
    case ToolStatus::Cancelled: statusIcon(QStringLiteral("cancelled")).paint(p, r); return;
    }
}

QString kindName(ToolKind k) {
    switch (k) {
    case ToolKind::Host: return QStringLiteral("host");
    case ToolKind::Workspace: return QStringLiteral("workspace");
    case ToolKind::Web: return QStringLiteral("web");
    case ToolKind::Agent: return QStringLiteral("agent");
    }
    return {};
}

// The row's icon: what the tool does (the mockup's set), its kind's colour.
QString toolGlyph(const TranscriptItem& it) {
    const QString& n = it.toolName;
    if (it.toolKind == ToolKind::Host) return QStringLiteral("chip");
    if (n == QLatin1String("edit") || n == QLatin1String("ast_edit")) return QStringLiteral("edit");
    if (n == QLatin1String("bash")) return QStringLiteral("terminal");
    if (n == QLatin1String("grep") || n == QLatin1String("find")) return QStringLiteral("search");
    return kindName(it.toolKind);
}

QColor kindColor(ToolKind k) {
    const Theme& t = theme();
    switch (k) {
    case ToolKind::Host: return t.host_;
    case ToolKind::Workspace: return t.workspace;
    case ToolKind::Web: return t.web;
    case ToolKind::Agent: return t.agent;
    }
    return t.fg2;
}

QString kindBadge(ToolKind k) {
    switch (k) {
    case ToolKind::Host: return theme().host == QLatin1String("binja") ? QStringLiteral("BN") : QStringLiteral("IDA");
    case ToolKind::Workspace: return QStringLiteral("FS");
    case ToolKind::Web: return QStringLiteral("WEB");
    case ToolKind::Agent: return QStringLiteral("AGENT");
    }
    return {};
}

QString permissionOutcome(const QString& r) {
    if (r == QLatin1String("allowed_once")) return QStringLiteral("\u2713 Allowed once");
    if (r == QLatin1String("allowed_chat")) return QStringLiteral("\u2713 Allow for this chat");
    if (r == QLatin1String("denied")) return QStringLiteral("\u2715 Denied");
    if (r == QLatin1String("cancelled")) return QStringLiteral("\u2013 Cancelled");
    return r;
}

QString cardHeading(const TranscriptItem& it) {
    switch (it.kind) {
    case ItemKind::Permission: return QStringLiteral("permission");
    case ItemKind::Approval: return QStringLiteral("approval");
    case ItemKind::Question: return QStringLiteral("question");
    case ItemKind::Dialog: return QStringLiteral("rikugan asks");
    case ItemKind::Decision: return str(it.payload, "kind") == QLatin1String("save") ? QStringLiteral("save") : QStringLiteral("plan");
    default: return {};
    }
}

QString resolvedLine(const TranscriptItem& it) {
    const QString first = [&] {
        switch (it.kind) {
        case ItemKind::Permission: return str(it.payload, "tool") + QStringLiteral("  ") + str(it.payload, "details").section(QLatin1Char('\n'), 0, 0);
        case ItemKind::Approval: return str(it.payload, "toolName");
        case ItemKind::Question: return str(it.payload, "question").section(QLatin1Char('\n'), 0, 0);
        case ItemKind::Dialog: return str(it.payload, "title").section(QLatin1Char('\n'), 0, 0);
        case ItemKind::Decision: {
            const QJsonObject p = it.payload.value(QLatin1String("payload")).toObject();
            if (str(it.payload, "kind") == QLatin1String("save"))
                return QStringLiteral("%1 patch(es)").arg(p.value(QLatin1String("patches")).toArray().size());
            return QStringLiteral("%1 step(s)").arg(p.value(QLatin1String("steps")).toArray().size());
        }
        default: return QString();
        }
    }();
    const QString outcome = it.kind == ItemKind::Permission ? permissionOutcome(it.resolution)
                            : it.resolution == QLatin1String("cancelled") ? QStringLiteral("\u2013 Cancelled")
                                                                          : QStringLiteral("\u2192 ") + it.resolution;
    return it.kind == ItemKind::Permission || it.kind == ItemKind::Approval ? outcome + QStringLiteral("   ") + first
                                                                              : first + QStringLiteral("   ") + outcome;
}

}  // namespace

// ==== delegate ====================================================================================================

class TranscriptDelegate : public QStyledItemDelegate {
public:
    enum class HitKind { None, Chip, Link, Expand, Undo, Agent, Text, Detail, Group };
    struct Hit {
        HitKind kind = HitKind::None;
        QString value;
    };

    explicit TranscriptDelegate(TranscriptView* view) : QStyledItemDelegate(view), view_(view) {}

    int spinPhase = 0;  // the spinners' angle in degrees, from the clock
    // the row whose text the selection overlay shows (its main document, or its detail)
    quint64 selectionItem = 0;
    bool selectionDetail = false;

    void clearCaches() {
        docs_.clear();
        details_.clear();
        sizes_.clear();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const TranscriptModel* m = view_->transcriptModel();
        if (!m || index.row() >= m->rowCount()) return {};
        const TranscriptItem& it = m->at(index.row());
        const int width = rowWidth();
        TranscriptCard* card = view_->cardFor(it.id);
        const int cardH = card ? card->heightFor(cardWidth(width)) : -1;
        auto c = sizes_.constFind(it.id);
        if (c != sizes_.constEnd() && c->rev == it.revision && c->width == width && c->expanded == it.expanded && c->cardH == cardH)
            return {width, c->height};
        const int h = layout(it, width, index.row()).height;
        sizes_.insert(it.id, SizeEntry{it.revision, width, it.expanded, cardH, h});
        return {width, h};
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const TranscriptModel* m = view_->transcriptModel();
        if (!m || index.row() >= m->rowCount()) return;
        const RowLayout l = layout(m->at(index.row()), rowWidth(), index.row());
        editor->setGeometry(l.card.translated(option.rect.topLeft()));
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const TranscriptModel* m = view_->transcriptModel();
        if (!m || index.row() >= m->rowCount()) return;
        const TranscriptItem& it = m->at(index.row());
        const RowLayout l = layout(it, rowWidth(), index.row());
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);
        p->translate(option.rect.topLeft());
        g_hiddenDoc = nullptr;
        if (it.id == selectionItem && !l.doc.isEmpty() && !selectionDetail) g_hiddenDoc = docFor(it, l.doc.width());
        if (it.id == selectionItem && !l.detailDoc.isEmpty() && selectionDetail) g_hiddenDoc = detailFor(it, l.detailDoc.width());
        paintRail(p, it, l, index.row(), m->rowCount());
        paintItem(p, it, l, m);
        g_hiddenDoc = nullptr;
        p->restore();
    }

    Hit hit(const QModelIndex& index, const QRect& rect, const QPoint& viewportPos) const {
        const TranscriptModel* m = view_->transcriptModel();
        if (!m || !index.isValid() || index.row() >= m->rowCount()) return {};
        const TranscriptItem& it = m->at(index.row());
        const RowLayout l = layout(it, rowWidth(), index.row());
        const QPoint pos = viewportPos - rect.topLeft();
        if (l.doc.contains(pos)) {
            if (QTextDocument* d = docFor(it, l.doc.width())) {
                const QString href = d->documentLayout()->anchorAt(QPointF(pos - l.doc.topLeft()));
                if (href.startsWith(QLatin1String("rk:"))) return {HitKind::Chip, href.section(QLatin1Char(':'), 2)};
                if (!href.isEmpty()) return {HitKind::Link, href};
            }
            return {HitKind::Text, {}};
        }
        if (it.kind == ItemKind::Thinking && l.header.contains(pos)) return {HitKind::Expand, {}};
        if (it.kind == ItemKind::Tool && l.group.contains(pos)) return {HitKind::Group, {}};
        if (it.kind == ItemKind::Tool && !l.header.isEmpty()) {
            const ToolGeometry g = toolGeometry(it, l.header);
            if (g.undo.contains(pos) && it.mutationIndex >= 0 && !it.undone) return {HitKind::Undo, QString::number(it.mutationIndex)};
            for (const auto& [r, chip] : g.chips)
                if (r.contains(pos)) return {HitKind::Chip, chip.value};
            if (l.header.contains(pos)) return {HitKind::Expand, {}};
            if (l.detail.contains(pos)) return {HitKind::Detail, {}};
        }
        if (it.kind == ItemKind::Task) {
            for (int i = 0; i < l.agents.size() && i < it.agents.size(); ++i)
                if (l.agents[i].contains(pos)) return {HitKind::Agent, it.agents[i].id};
        }
        if (it.kind == ItemKind::Finding && l.header.contains(pos)) {
            const QString a = str(it.payload, "address");
            if (!a.isEmpty()) return {HitKind::Chip, a};
        }
        return {};
    }

    // The selectable text of a row (selection overlay): the main document and where it sits in the row.
    QTextDocument* selectableDoc(const QModelIndex& index, bool detail, QRect* where) const {
        const TranscriptModel* m = view_->transcriptModel();
        if (!m || index.row() >= m->rowCount()) return nullptr;
        const TranscriptItem& it = m->at(index.row());
        const RowLayout l = layout(it, rowWidth(), index.row());
        if (detail) {
            *where = l.detailDoc;
            return l.detailDoc.isEmpty() ? nullptr : detailFor(it, l.detailDoc.width());
        }
        *where = l.doc;
        return l.doc.isEmpty() ? nullptr : docFor(it, l.doc.width());
    }

    // a row that draws a spinner (streaming text repaints as it grows, not on the spinner's clock)
    bool hasRunning(const TranscriptItem& it) const {
        if (it.kind == ItemKind::Tool && it.groupCount >= 2) {
            const TranscriptModel* m = view_->transcriptModel();
            for (int r : m->groupRows(m->rowOfId(it.id)))
                if (m->at(r).status == ToolStatus::Running || m->at(r).status == ToolStatus::Pending) return true;
            return false;
        }
        if (it.kind == ItemKind::Tool) return it.status == ToolStatus::Running || it.status == ToolStatus::Pending;
        if (it.kind == ItemKind::Task)
            return std::any_of(it.agents.cbegin(), it.agents.cend(), [](const AgentRow& a) { return a.status == QLatin1String("running"); });
        return false;
    }

    void forget() { sizes_.clear(); }

private:
    struct SizeEntry {
        quint32 rev = 0;
        int width = 0;
        bool expanded = false;
        int cardH = -1;
        int height = 0;
    };
    struct DocEntry {
        quint32 rev = 0;
        std::shared_ptr<QTextDocument> doc;
    };
    struct RowLayout {
        int height = 0;
        int nodeY = -1;      // timeline node, -1: none
        QRect header;        // the row's heading line / tool row
        QRect group;         // a tool group's own line ("name ×N"), above its first call
        QRect box;           // a framed area (bubble, card, output box)
        QRect doc;           // the main document
        QRect card;          // the live card
        QRect detail, detailDoc;
        QList<QRect> agents;
    };
    struct ToolGeometry {
        QRect icon, name, badge, status, undo;
        QList<std::pair<QRect, QString>> segments;  // label text runs (plain)
        QList<std::pair<QRect, Chip>> chips;
        QRect summary;
        QString labelElided;
    };

    int rowWidth() const { return std::max(160, view_->viewport()->width()); }
    static int cardWidth(int width) { return std::max(100, width - kIndent - kRightPad); }

    static int lineH(const QFont& f) { return QFontMetrics(f).height(); }

    // ---- documents ----

    QTextDocument* docFor(const TranscriptItem& it, int width) const {
        DocEntry& e = docs_[it.id];
        if (!e.doc || e.rev != it.revision) {
            e.doc = std::make_shared<QTextDocument>();
            e.rev = it.revision;
            buildDoc(e.doc.get(), it);
            e.doc->setTextWidth(width);
        } else if (int(e.doc->textWidth()) != width) {
            e.doc->setTextWidth(width);
        }
        return e.doc.get();
    }

    QTextDocument* detailFor(const TranscriptItem& it, int width) const {
        DocEntry& e = details_[it.id];
        if (!e.doc || e.rev != it.revision) {
            e.doc = std::make_shared<QTextDocument>();
            e.rev = it.revision;
            buildDetail(e.doc.get(), it);
            e.doc->setTextWidth(width);
        } else if (int(e.doc->textWidth()) != width) {
            e.doc->setTextWidth(width);
        }
        return e.doc.get();
    }

    static void buildDoc(QTextDocument* d, const TranscriptItem& it) {
        const Theme& t = theme();
        d->setDocumentMargin(0);
        d->setDefaultFont(t.ui);
        switch (it.kind) {
        case ItemKind::User: d->setPlainText(it.text); return;
        case ItemKind::Notice:
        case ItemKind::Phase:
            d->setDefaultFont(t.uiSmall);
            d->setPlainText(it.text);
            return;
        case ItemKind::Error:
            d->setPlainText(it.text);
            return;
        case ItemKind::CommandOutput:
            d->setDefaultFont(t.monoSmall);
            d->setPlainText(it.text.trimmed());
            return;
        case ItemKind::Thinking: {
            QFont f = t.ui;
            f.setItalic(true);
            d->setDefaultFont(f);
            d->setMarkdown(it.text, QTextDocument::MarkdownDialectGitHub);
            polishMarkdown(d);
            return;
        }
        case ItemKind::Decision: {
            // a decided plan keeps its steps with their progress
            const QJsonObject p = it.payload.value(QLatin1String("payload")).toObject();
            const QJsonObject st = it.payload.value(QLatin1String("stepStatus")).toObject();
            const QJsonArray steps = p.value(QLatin1String("steps")).toArray();
            QTextCursor c(d);
            for (int i = 0; i < steps.size(); ++i) {
                const QString s = st.value(QString::number(i)).toString();
                QTextCharFormat glyph;
                QString g = QStringLiteral("\u25CB");
                glyph.setForeground(t.fg3);
                if (s == QLatin1String("running") || s == QLatin1String("in_progress")) {
                    g = QStringLiteral("\u25D0");
                    glyph.setForeground(t.accent);
                } else if (s == QLatin1String("done") || s == QLatin1String("completed")) {
                    g = QStringLiteral("\u2713");
                    glyph.setForeground(t.ok);
                } else if (s == QLatin1String("failed")) {
                    g = QStringLiteral("\u2715");
                    glyph.setForeground(t.err);
                } else if (s == QLatin1String("skipped")) {
                    g = QStringLiteral("\u2013");
                }
                if (i) c.insertBlock();
                c.insertText(g + QStringLiteral("  "), glyph);
                const QJsonValue v = steps.at(i);
                QTextCharFormat text;
                text.setForeground(s == QLatin1String("done") || s == QLatin1String("completed") ? t.fg2 : t.fg);
                // the step's own Markdown (`code`, **bold**) inline after its glyph
                c.setCharFormat(text);
                c.insertFragment(QTextDocumentFragment::fromMarkdown(v.isString() ? v.toString() : str(v.toObject(), "text"),
                                                                     QTextDocument::MarkdownDialectGitHub));
            }
            return;
        }
        default:
            break;
        }
        // Text, Btw, Finding, Note: Markdown with chips
        d->setMarkdown(linkify(it.text, it.chips), QTextDocument::MarkdownDialectGitHub);
        polishMarkdown(d);
    }

    static void buildDetail(QTextDocument* d, const TranscriptItem& it) {
        const Theme& t = theme();
        d->setDocumentMargin(0);
        d->setDefaultFont(t.monoSmall);
        QTextCursor c(d);
        QTextCharFormat plain, muted, add, del;
        plain.setForeground(t.fg);
        muted.setForeground(t.fg3);
        add.setForeground(t.diffAddFg);
        del.setForeground(t.diffDelFg);
        bool first = true;
        auto line = [&](const QString& s, const QTextCharFormat& f, const QColor& bg = {}) {
            if (!first) c.insertBlock();
            first = false;
            QTextBlockFormat bf;
            if (bg.isValid()) bf.setBackground(bg);
            c.setBlockFormat(bf);
            c.insertText(s, f);
        };
        auto lines = [&](const QString& text, const QTextCharFormat& f, int cap) {
            const QStringList ls = text.split(QLatin1Char('\n'));
            for (int i = 0; i < ls.size() && i < cap; ++i) line(ls[i], f);
            if (ls.size() > cap) line(QStringLiteral("\u2026 %1 more lines").arg(ls.size() - cap), muted);
        };
        // what was asked
        if (it.toolName == QLatin1String("bash")) {
            line(QStringLiteral("$ ") + str(it.args, "command"), muted);
        } else if (!it.diff.isEmpty()) {
            line(str(it.args, "path"), muted);
        } else if (!it.args.isEmpty() && it.code.isEmpty()) {
            lines(QString::fromUtf8(QJsonDocument(it.args).toJson(QJsonDocument::Indented)).trimmed(), muted, 16);
        }
        if (!it.diff.isEmpty()) {
            const QStringList ls = it.diff.split(QLatin1Char('\n'));
            for (int i = 0; i < ls.size() && i < kDetailLines; ++i) {
                const QString& l = ls[i];
                if (l.startsWith(QLatin1Char('+')) && !l.startsWith(QLatin1String("+++"))) line(l, add, t.diffAddBg);
                else if (l.startsWith(QLatin1Char('-')) && !l.startsWith(QLatin1String("---"))) line(l, del, t.diffDelBg);
                else line(l, l.startsWith(QLatin1String("@@")) ? muted : plain);
            }
            if (ls.size() > kDetailLines) line(QStringLiteral("\u2026 %1 more lines").arg(ls.size() - kDetailLines), muted);
            return;
        }
        if (!it.code.isEmpty()) {
            line(str(it.args, "path"), muted);
            const int start = c.position();
            lines(it.code, plain, kDetailLines);
            // syntax colours over the inserted lines
            const QString language = languageForPath(str(it.args, "path"));
            if (!language.isEmpty()) {
                int state = -1;
                for (QTextBlock b = d->findBlock(start); b.isValid(); b = b.next()) {
                    for (const CodeSpan& sp : scanLine(language, b.text(), state, &state)) {
                        QTextCursor sc(b);
                        sc.setPosition(b.position() + sp.start);
                        sc.setPosition(b.position() + sp.start + sp.length, QTextCursor::KeepAnchor);
                        sc.mergeCharFormat(syntaxFormat(sp.kind));
                    }
                }
            }
        }
        if (!it.webSources.isEmpty()) {
            for (const QJsonValue& v : it.webSources) {
                const QJsonObject s = v.toObject();
                line(str(s, "title").isEmpty() ? str(s, "url") : str(s, "title"), plain);
                if (!str(s, "url").isEmpty()) line(QStringLiteral("  ") + str(s, "url"), muted);
            }
            return;
        }
        if (!it.result.isEmpty() && it.code.isEmpty()) {
            if (!first) line(QString(), muted);
            lines(it.result.trimmed(), it.status == ToolStatus::Error ? del : plain, kDetailLines);
        }
    }

    // ---- layout ----

    RowLayout layout(const TranscriptItem& it, int width, int /*row*/) const {
        const Theme& t = theme();
        const int L = kIndent, W = width - kIndent - kRightPad;
        const int rowH = t.rowHeight;
        RowLayout l;
        auto withDoc = [&](int x, int y, int w) {
            QTextDocument* d = docFor(it, w);
            return QRect(x, y, w, int(std::ceil(d->size().height())));
        };
        switch (it.kind) {
        case ItemKind::User: {
            l.header = QRect(L, 6, W, rowH);
            l.nodeY = l.header.center().y();
            l.doc = withDoc(L + 10, l.header.bottom() + 7, W - 20);
            l.box = QRect(L, l.header.bottom() + 1, W, l.doc.height() + 12);
            l.height = l.box.bottom() + 8;
            break;
        }
        case ItemKind::Step:
            l.header = QRect(L, 6, W, rowH);
            l.nodeY = l.header.center().y();
            l.height = l.header.bottom() + 2;
            break;
        case ItemKind::Thinking: {
            l.header = QRect(L, 0, W, rowH);
            if (it.expanded || it.streaming) {
                l.doc = withDoc(L + 12, rowH + 2, W - 12);
                l.height = l.doc.bottom() + 6;
            } else {
                l.height = rowH;
            }
            break;
        }
        case ItemKind::Text:
            l.doc = withDoc(L, 3, W);
            l.height = l.doc.bottom() + 8;
            break;
        case ItemKind::Tool: {
            int y = 0;
            if (it.groupCount >= 2) {
                // the group's line; its first call (this row's own) shows under it while the group is open
                l.group = QRect(L, 0, W, rowH);
                l.height = rowH;
                if (!it.groupOpen) break;
                y = rowH;
            }
            l.header = QRect(L, y, W, rowH);
            l.height = y + rowH;
            if (it.expanded) {
                const int x = L + 20;
                QTextDocument* d = detailFor(it, W - 20 - 16);
                l.detailDoc = QRect(x + 8, y + rowH + 8, W - 20 - 16, int(std::ceil(d->size().height())));
                l.detail = QRect(x, y + rowH + 2, W - 20, l.detailDoc.height() + 12);
                l.height = l.detail.bottom() + 6;
            }
            break;
        }
        case ItemKind::Task: {
            l.header = QRect(L, 2, W, rowH);
            const int sh = lineH(t.uiSmall);
            int y = l.header.bottom() + 3;
            for (int i = 0; i < it.agents.size(); ++i) {
                const QRect r(L + 18, y, W - 18, rowH + sh + 8);
                l.agents << r;
                y = r.bottom() + 4;
            }
            l.height = y + 4;
            break;
        }
        case ItemKind::Permission:
        case ItemKind::Approval:
        case ItemKind::Question:
        case ItemKind::Dialog:
        case ItemKind::Decision: {
            l.header = QRect(L, 6, W, rowH);
            l.nodeY = l.header.center().y();
            const int top = l.header.bottom() + 2;
            if (TranscriptCard* card = view_->cardFor(it.id)) {
                l.card = QRect(L, top, W, card->heightFor(W));
                l.height = l.card.bottom() + 8;
            } else if (it.kind == ItemKind::Decision && str(it.payload, "kind") == QLatin1String("plan")) {
                l.box = QRect(L, top, W, rowH + 4);
                l.doc = withDoc(L + 10, l.box.bottom() + 4, W - 20);
                l.height = l.doc.bottom() + 10;
            } else {
                l.box = QRect(L, top, W, rowH + 4);
                l.height = l.box.bottom() + 8;
            }
            break;
        }
        case ItemKind::Phase: {
            l.header = QRect(L, 8, W, rowH);
            l.nodeY = l.header.center().y();
            l.height = l.header.bottom() + 4;
            if (!it.text.trimmed().isEmpty()) {
                l.doc = withDoc(L, l.header.bottom() + 1, W);
                l.height = l.doc.bottom() + 6;
            }
            break;
        }
        case ItemKind::Finding: {
            l.header = QRect(L + 10, 6, W - 20, rowH);
            l.doc = withDoc(L + 10, l.header.bottom() + 1, W - 20);
            l.box = QRect(L, 3, W, l.doc.bottom() + 6 - 3);
            l.height = l.box.bottom() + 6;
            break;
        }
        case ItemKind::Note:
            l.header = QRect(L, 3, W, rowH);
            l.nodeY = l.header.center().y();
            l.height = l.header.bottom() + 4;
            break;
        case ItemKind::Notice:
            l.doc = withDoc(L, 4, W);
            l.nodeY = 4 + lineH(t.uiSmall) / 2;
            l.height = l.doc.bottom() + 6;
            break;
        case ItemKind::Error:
        case ItemKind::CommandOutput: {
            l.doc = withDoc(L + 10, 9, W - 20);
            l.box = QRect(L, 3, W, l.doc.height() + 12);
            l.nodeY = l.box.top() + 6 + lineH(it.kind == ItemKind::Error ? t.ui : t.monoSmall) / 2;
            l.height = l.box.bottom() + 6;
            break;
        }
        case ItemKind::Btw: {
            l.header = QRect(L, 6, W, rowH);
            l.nodeY = l.header.center().y();
            l.doc = withDoc(L + 10, l.header.bottom() + 7, W - 20);
            l.box = QRect(L, l.header.bottom() + 1, W, std::max(l.doc.height(), lineH(t.ui)) + 12);
            l.height = l.box.bottom() + 8;
            break;
        }
        }
        return l;
    }

    ToolGeometry toolGeometry(const TranscriptItem& it, const QRect& row) const {
        const Theme& t = theme();
        ToolGeometry g;
        const int cy = row.center().y();
        const QFontMetrics fmName(boldMono()), fmSmall(t.uiSmall), fmMono(t.monoSmall), fmBadge(badgeFont());
        int x = row.left() + 12;
        g.icon = QRect(x, cy - 7, 14, 14);
        x = g.icon.right() + 7;
        const int nameW = fmName.horizontalAdvance(it.toolName);
        g.name = QRect(x, row.top(), nameW, row.height());
        x = g.name.right() + 7;
        const QString badge = kindBadge(it.toolKind);
        const int badgeW = fmBadge.horizontalAdvance(badge) + 8;
        g.badge = QRect(x, cy - fmBadge.height() / 2 - 1, badgeW, fmBadge.height() + 2);
        x = g.badge.right() + 7;
        // right side, from the edge inwards: status, undo, summary
        int right = row.right();
        g.status = QRect(right - 14, cy - 7, 14, 14);
        right = g.status.left() - 8;
        // no Undo on a change nothing can reverse (set_type, the host's MCP tools ...)
        if (!it.mutation.isEmpty() && it.mutationIndex >= 0 && it.mutationReversible) {
            const QString u = it.undone ? QStringLiteral("undone") : QStringLiteral("Undo");
            const int uw = fmSmall.horizontalAdvance(u) + 14;
            g.undo = QRect(right - uw, cy - fmSmall.height() / 2 - 2, uw, fmSmall.height() + 4);
            right = g.undo.left() - 8;
        }
        if (!it.summary.isEmpty()) {
            const int sw = std::min(fmSmall.horizontalAdvance(it.summary) + 2, std::max(40, row.width() / 4));
            g.summary = QRect(right - sw, row.top(), sw, row.height());
            right = g.summary.left() - 10;
        }
        // label runs with chips, cut at `right`
        const QString& label = it.label;
        int pos = 0;
        auto run = [&](const QString& s) -> bool {
            if (s.isEmpty()) return true;
            const int w = fmMono.horizontalAdvance(s);
            if (x + w <= right) {
                g.segments.append({QRect(x, row.top(), w, row.height()), s});
                x += w;
                return true;
            }
            const QString e = fmMono.elidedText(s, Qt::ElideRight, std::max(0, right - x));
            if (!e.isEmpty()) g.segments.append({QRect(x, row.top(), fmMono.horizontalAdvance(e), row.height()), e});
            x = right;
            return false;
        };
        for (const Chip& c : it.chips) {
            if (c.from < pos || c.to > label.size()) continue;
            if (!run(label.mid(pos, c.from - pos))) return g;
            const QString text = (c.kind == QLatin1String("sym") ? QStringLiteral("\u0192") : QString()) + label.mid(c.from, c.to - c.from);
            const int w = fmMono.horizontalAdvance(text) + 10;
            if (x + w > right) {
                run(text);
                return g;
            }
            g.chips.append({QRect(x, cy - fmMono.height() / 2 - 2, w, fmMono.height() + 4), Chip{c.from, c.to, c.kind, c.value}});
            g.segments.append({QRect(x + 5, row.top(), w - 10, row.height()), text});
            x += w + 2;
            pos = c.to;
        }
        run(label.mid(pos));
        return g;
    }

    static QFont boldMono() {
        QFont f = theme().monoSmall;
        f.setWeight(QFont::DemiBold);
        return f;
    }
    static QFont badgeFont() {
        QFont f = theme().monoSmall;
        if (f.pointSizeF() > 0) f.setPointSizeF(std::max(6.5, f.pointSizeF() - 2));
        f.setWeight(QFont::DemiBold);
        return f;
    }

    // ---- painting ----

    void paintRail(QPainter* p, const TranscriptItem& it, const RowLayout& l, int row, int rows) const {
        const Theme& t = theme();
        const int top = row == 0 ? (l.nodeY >= 0 ? l.nodeY : 0) : 0;
        const int bottom = row == rows - 1 ? (l.nodeY >= 0 ? l.nodeY : l.height / 2) : l.height;
        p->setPen(QPen(t.line, 1));
        if (bottom > top) p->drawLine(QPointF(kRail + 0.5, top), QPointF(kRail + 0.5, bottom));
        if (l.nodeY < 0) return;
        QColor fill = t.fg3, ring;
        int r = kNode;
        switch (it.kind) {
        case ItemKind::User: fill = t.fg3; break;
        case ItemKind::Step:
            fill = t.bg;
            ring = it.streaming ? t.accent : t.fg3;
            break;
        case ItemKind::Permission:
        case ItemKind::Approval:
        case ItemKind::Dialog:
        case ItemKind::Question: fill = t.warn; break;
        case ItemKind::Decision:
        case ItemKind::Phase: fill = t.accent; break;
        case ItemKind::Btw: fill = t.web; break;
        case ItemKind::Error: fill = t.err; r = 3; break;
        case ItemKind::Note: fill = t.ok; break;
        default: fill = t.fg3; r = 2; break;
        }
        p->save();
        p->setPen(ring.isValid() ? QPen(ring, 1.5) : Qt::NoPen);
        p->setBrush(fill);
        p->drawEllipse(QPointF(kRail + 0.5, l.nodeY + 0.5), r, r);
        p->restore();
    }

    void heading(QPainter* p, const QRect& r, const QString& who, const QString& when, const QColor& whoColor) const {
        const Theme& t = theme();
        QFont f = t.uiSmall;
        f.setWeight(QFont::DemiBold);
        p->setFont(f);
        p->setPen(whoColor);
        p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, who);
        if (when.isEmpty()) return;
        const int x = r.left() + QFontMetrics(f).horizontalAdvance(who) + 6;
        p->setFont(t.uiSmall);
        p->setPen(t.fg3);
        p->drawText(QRect(x, r.top(), r.right() - x, r.height()), Qt::AlignLeft | Qt::AlignVCenter, when);
    }

    QString stepOffset(const TranscriptItem& it) const {
        const double ms = it.payload.value(QLatin1String("offsetMs")).toDouble();
        return ms > 0 ? QStringLiteral("+%1").arg(elapsed(qint64(ms))) : QString();
    }

    void paintItem(QPainter* p, const TranscriptItem& it, const RowLayout& l, const TranscriptModel* m) const {
        const Theme& t = theme();
        switch (it.kind) {
        case ItemKind::User:
            heading(p, l.header, QStringLiteral("you"), QDateTime::fromMSecsSinceEpoch(it.timeMs).toString(QStringLiteral("HH:mm")), t.fg2);
            roundRect(p, l.box, t.userBg, t.userLine);
            drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg);
            return;
        case ItemKind::Step: {
            QString sub = stepOffset(it);
            heading(p, l.header, it.title, sub, t.fg2);
            return;
        }
        case ItemKind::Thinking: {
            const bool open = it.expanded || it.streaming;
            p->setFont(t.uiSmall);
            p->setPen(t.accent);
            const QString caret = open ? QStringLiteral("\u25BE") : QStringLiteral("\u25B8");
            QString label = it.streaming ? QStringLiteral("Thinking\u2026") : QStringLiteral("Thought");
            if (!it.streaming && it.endMs > it.timeMs && it.timeMs) label += QStringLiteral(" \u00b7 ") + elapsed(it.endMs - it.timeMs);
            p->drawText(l.header.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, caret + QStringLiteral("  \u2727 ") + label);
            if (open && !l.doc.isEmpty()) {
                p->setPen(QPen(t.line2, 1));
                p->drawLine(QPointF(l.doc.left() - 7.5, l.doc.top()), QPointF(l.doc.left() - 7.5, l.doc.bottom()));
                drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg2);
            }
            return;
        }
        case ItemKind::Text:
            drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg);
            return;
        case ItemKind::Tool:
            paintTool(p, it, l, m);
            return;
        case ItemKind::Task:
            paintTask(p, it, l);
            return;
        case ItemKind::Permission:
        case ItemKind::Approval:
        case ItemKind::Question:
        case ItemKind::Dialog:
        case ItemKind::Decision: {
            QString when;
            if (m->activity().turnStartMs && it.timeMs >= m->activity().turnStartMs)
                when = QStringLiteral("+") + elapsed(it.timeMs - m->activity().turnStartMs);
            heading(p, l.header, cardHeading(it), when, it.kind == ItemKind::Decision ? t.accent : t.warn);
            if (l.box.isEmpty()) return;  // the live card paints itself
            roundRect(p, l.box, t.bg2, t.line);
            QColor ink = t.fg2;
            if (it.kind == ItemKind::Permission || it.kind == ItemKind::Approval)
                ink = it.resolution.startsWith(QLatin1String("allowed")) || it.resolution.startsWith(QLatin1String("Allowed")) ? t.ok
                      : it.resolution == QLatin1String("cancelled")                                                         ? t.fg3
                                                                                                                            : t.err;
            p->setFont(t.uiSmall);
            p->setPen(ink);
            p->drawText(l.box.adjusted(10, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(t.uiSmall).elidedText(resolvedLine(it), Qt::ElideRight, l.box.width() - 20));
            if (!l.doc.isEmpty()) drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg);
            return;
        }
        case ItemKind::Phase: {
            QFont f = badgeFont();
            const QString title = it.title.toUpper();
            const int w = QFontMetrics(f).horizontalAdvance(title) + 12;
            const QRect badge(l.header.left(), l.header.center().y() - 8, w, 16);
            roundRect(p, badge, t.accentSoft2, {}, 3);
            p->setFont(f);
            p->setPen(t.accent);
            p->drawText(badge, Qt::AlignCenter, title);
            if (!l.doc.isEmpty()) drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg2);
            return;
        }
        case ItemKind::Finding: {
            const QString rel = str(it.payload, "relevance");
            const QColor bar = rel == QLatin1String("high") ? t.err : rel == QLatin1String("low") ? t.fg3 : t.warn;
            roundRect(p, l.box, t.bg2, t.line);
            p->fillRect(QRect(l.box.left(), l.box.top() + 3, 3, l.box.height() - 6), bar);
            QFont f = t.uiSmall;
            f.setWeight(QFont::DemiBold);
            p->setFont(f);
            p->setPen(bar);
            QString head = QStringLiteral("\u25C6 ") + it.title + QStringLiteral(" \u00b7 ") + rel;
            p->drawText(l.header, Qt::AlignLeft | Qt::AlignVCenter, head);
            const QString addr = str(it.payload, "address");
            if (!addr.isEmpty()) {
                p->setFont(t.monoSmall);
                p->setPen(t.addr);
                p->drawText(l.header, Qt::AlignRight | Qt::AlignVCenter, addr);
            }
            drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg);
            return;
        }
        case ItemKind::Note:
            heading(p, l.header, QStringLiteral("research note"),
                    it.title + (str(it.payload, "path").isEmpty() ? QString() : QStringLiteral("  ") + str(it.payload, "path")), t.ok);
            return;
        case ItemKind::Notice:
            drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg3);
            return;
        case ItemKind::Error:
            roundRect(p, l.box, blend(t.err, t.bg, 0.9), blend(t.err, t.bg, 0.6));
            drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.err);
            return;
        case ItemKind::CommandOutput:
            roundRect(p, l.box, t.codeBg, t.line);
            drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg);
            return;
        case ItemKind::Btw: {
            heading(p, l.header, QStringLiteral("btw"), it.title, t.web);
            roundRect(p, l.box, blend(t.web, t.bg, 0.93), blend(t.web, t.bg, 0.7));
            if (it.text.isEmpty() && it.streaming) {
                p->setFont(t.uiSmall);
                p->setPen(t.fg3);
                p->drawText(l.doc, Qt::AlignLeft | Qt::AlignTop, QStringLiteral("\u2026"));
            } else {
                drawDoc(p, docFor(it, l.doc.width()), l.doc.topLeft(), t.fg);
            }
            return;
        }
        }
    }

    // A group's line: the tool once with its call count, the calls' arguments in a row, how many are done.
    void paintGroup(QPainter* p, const TranscriptItem& it, const QRect& row, const TranscriptModel* m) const {
        const Theme& t = theme();
        int running = 0, failed = 0, n = 0;
        QStringList labels;
        for (int r : m->groupRows(m->rowOfId(it.id))) {
            const TranscriptItem& c = m->at(r);
            ++n;
            if (c.status == ToolStatus::Running || c.status == ToolStatus::Pending) ++running;
            else if (c.status == ToolStatus::Error || c.status == ToolStatus::Denied) ++failed;
            if (!c.label.isEmpty()) labels << c.label;
        }
        const QFontMetrics fmName(boldMono()), fmSmall(t.uiSmall), fmMono(t.monoSmall), fmBadge(badgeFont());
        const int cy = row.center().y();
        int x = row.left() + 12;
        const QColor kc = kindColor(it.toolKind);
        kindIcon(toolGlyph(it), kc).paint(p, QRect(x, cy - 7, 14, 14));
        x += 14 + 7;
        p->setFont(boldMono());
        p->setPen(t.fg);
        const int nameW = fmName.horizontalAdvance(it.toolName);
        p->drawText(QRect(x, row.top(), nameW, row.height()), Qt::AlignLeft | Qt::AlignVCenter, it.toolName);
        x += nameW + 7;
        const QString badge = kindBadge(it.toolKind);
        const QRect b(x, cy - fmBadge.height() / 2 - 1, fmBadge.horizontalAdvance(badge) + 8, fmBadge.height() + 2);
        roundRect(p, b, blend(kc, t.bg, t.dark ? 0.82 : 0.88), {}, 3);
        p->setFont(badgeFont());
        p->setPen(kc);
        p->drawText(b, Qt::AlignCenter, badge);
        x = b.right() + 6;
        // "×N ▸": the count and the toggle
        const QString count = QStringLiteral("\u00d7%1 %2").arg(n).arg(it.groupOpen ? QStringLiteral("\u25BE") : QStringLiteral("\u25B8"));
        const QRect cr(x, cy - fmSmall.height() / 2 - 1, fmSmall.horizontalAdvance(count) + 10, fmSmall.height() + 2);
        roundRect(p, cr, blend(t.accent, t.bg, t.dark ? 0.85 : 0.9), {}, 3);
        p->setFont(t.uiSmall);
        p->setPen(t.accent);
        p->drawText(cr, Qt::AlignCenter, count);
        x = cr.right() + 8;
        // right side: status, then "4 done · 1 failed"
        const QRect st(row.right() - 14, cy - 7, 14, 14);
        statusGlyph(p, st, running ? ToolStatus::Running : failed ? ToolStatus::Error : ToolStatus::Done, spinPhase);
        QString tally = running ? QStringLiteral("%1/%2 done").arg(n - running).arg(n) : QStringLiteral("%1 done").arg(n - failed);
        if (failed) tally += QStringLiteral(" \u00b7 %1 failed").arg(failed);
        const int tw = fmSmall.horizontalAdvance(tally) + 2;
        const QRect tallyR(st.left() - 8 - tw, row.top(), tw, row.height());
        p->setPen(failed ? t.err : t.fg3);
        p->drawText(tallyR, Qt::AlignRight | Qt::AlignVCenter, tally);
        // the calls' arguments, as many as fit
        const int room = tallyR.left() - 10 - x;
        if (!it.groupOpen && room > 0) {
            p->setFont(t.monoSmall);
            p->setPen(t.fg2);
            p->drawText(QRect(x, row.top(), room, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        fmMono.elidedText(labels.join(QStringLiteral(" \u00b7 ")), Qt::ElideRight, room));
        }
    }

    void paintTool(QPainter* p, const TranscriptItem& it, const RowLayout& l, const TranscriptModel* m) const {
        const Theme& t = theme();
        // tree connector from the step
        p->setPen(QPen(t.line2, 1));
        const qreal gx = (l.group.isEmpty() ? l.header : l.group).left() + 4.5;
        p->drawLine(QPointF(gx, 0), QPointF(gx, l.height));
        if (!l.group.isEmpty()) {
            p->drawLine(QPointF(gx, l.group.center().y() + 0.5), QPointF(l.group.left() + 10, l.group.center().y() + 0.5));
            paintGroup(p, it, l.group, m);
            if (l.header.isEmpty()) return;
            p->setPen(QPen(t.line2, 1));
        }
        const ToolGeometry g = toolGeometry(it, l.header);
        p->drawLine(QPointF(gx, l.header.center().y() + 0.5), QPointF(g.icon.left() - 2, l.header.center().y() + 0.5));
        const QColor kc = kindColor(it.toolKind);
        kindIcon(toolGlyph(it), kc).paint(p, g.icon);
        p->setFont(boldMono());
        p->setPen(it.status == ToolStatus::Denied || it.status == ToolStatus::Cancelled ? t.fg3 : t.fg);
        p->drawText(g.name, Qt::AlignLeft | Qt::AlignVCenter, it.toolName);
        roundRect(p, g.badge, blend(kc, t.bg, t.dark ? 0.82 : 0.88), {}, 3);
        p->setFont(badgeFont());
        p->setPen(kc);
        p->drawText(g.badge, Qt::AlignCenter, kindBadge(it.toolKind));
        for (const auto& [r, chip] : g.chips) {
            const QColor ink = chip.kind == QLatin1String("addr") ? t.addr : t.sym;
            roundRect(p, r, blend(ink, t.bg, t.dark ? 0.84 : 0.9), {}, 3);
        }
        p->setFont(t.monoSmall);
        for (const auto& [r, text] : g.segments) {
            QColor ink = t.fg2;
            for (const auto& [cr, chip] : g.chips)
                if (cr.contains(r.center())) ink = chip.kind == QLatin1String("addr") ? t.addr : t.sym;
            p->setPen(ink);
            p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, text);
        }
        if (!g.summary.isEmpty()) {
            p->setFont(t.uiSmall);
            p->setPen(it.status == ToolStatus::Error || it.status == ToolStatus::Denied ? t.err : t.fg3);
            p->drawText(g.summary, Qt::AlignRight | Qt::AlignVCenter,
                        QFontMetrics(t.uiSmall).elidedText(it.summary, Qt::ElideRight, g.summary.width()));
        }
        if (!g.undo.isEmpty()) {
            const QColor ink = it.undone ? t.fg3 : t.warn;
            roundRect(p, g.undo, it.undone ? QColor() : blend(t.warn, t.bg, 0.9), blend(ink, t.bg, 0.4), 4);
            p->setFont(t.uiSmall);
            p->setPen(ink);
            p->drawText(g.undo, Qt::AlignCenter, it.undone ? QStringLiteral("undone") : QStringLiteral("Undo"));
        }
        statusGlyph(p, g.status, it.status, spinPhase);
        if (!l.detail.isEmpty()) {
            roundRect(p, l.detail, t.codeBg, t.line);
            drawDoc(p, detailFor(it, l.detailDoc.width()), l.detailDoc.topLeft(), t.fg);
        }
    }

    void paintTask(QPainter* p, const TranscriptItem& it, const RowLayout& l) const {
        const Theme& t = theme();
        p->setPen(QPen(t.line2, 1));
        const qreal gx = l.header.left() + 4.5;
        p->drawLine(QPointF(gx, 0), QPointF(gx, l.header.center().y()));
        p->drawLine(QPointF(gx, l.header.center().y() + 0.5), QPointF(gx + 8, l.header.center().y() + 0.5));
        int done = 0;
        bool running = false;
        for (const AgentRow& a : it.agents) {
            done += a.status != QLatin1String("running") ? 1 : 0;
            running |= a.status == QLatin1String("running");
        }
        const QRect icon(l.header.left() + 12, l.header.center().y() - 7, 14, 14);
        kindIcon(QStringLiteral("agent"), t.agent).paint(p, icon);
        QFont bold = boldMono();
        p->setFont(bold);
        p->setPen(t.fg);
        const QRect name(icon.right() + 7, l.header.top(), QFontMetrics(bold).horizontalAdvance(QStringLiteral("task")), l.header.height());
        p->drawText(name, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("task"));
        const QString right = QStringLiteral("%1/%2 done").arg(done).arg(it.agents.size());
        const QFontMetrics fs(t.uiSmall);
        const int rw = fs.horizontalAdvance(right) + (running ? 20 : 0);
        QString mid;
        if (!it.agents.isEmpty())
            mid = QStringLiteral("%1 agent%2 \u00b7 %3")
                      .arg(it.agents.size())
                      .arg(it.agents.size() == 1 ? QString() : QStringLiteral("s"), it.agents.first().task);
        p->setFont(t.uiSmall);
        p->setPen(t.fg3);
        const QRect midR(name.right() + 8, l.header.top(), l.header.right() - rw - name.right() - 16, l.header.height());
        p->drawText(midR, Qt::AlignLeft | Qt::AlignVCenter, fs.elidedText(mid, Qt::ElideRight, midR.width()));
        p->drawText(QRect(l.header.right() - rw, l.header.top(), rw - (running ? 20 : 0), l.header.height()),
                    Qt::AlignRight | Qt::AlignVCenter, right);
        if (running) spinner(p, QRectF(l.header.right() - 14, l.header.center().y() - 7, 14, 14), t.agent, spinPhase);

        const qint64 now = nowMs();
        for (int i = 0; i < l.agents.size() && i < it.agents.size(); ++i) {
            const AgentRow& a = it.agents[i];
            const QRect r = l.agents[i];
            roundRect(p, r, t.bg2, t.line);
            const int y1 = r.top() + 4, h1 = t.rowHeight - 4;
            const QRect st(r.left() + 8, y1 + h1 / 2 - 7, 14, 14);
            if (a.status == QLatin1String("running")) spinner(p, st, t.agent, spinPhase);
            else statusIcon(a.status == QLatin1String("done") ? QStringLiteral("done")
                            : a.status == QLatin1String("failed") ? QStringLiteral("error")
                                                                   : QStringLiteral("cancelled"))
                     .paint(p, st);
            const QString time = elapsed((a.endedMs ? a.endedMs : now) - (a.startedMs ? a.startedMs : now));
            const int tw = fs.horizontalAdvance(time);
            p->setFont(t.uiSmall);
            p->setPen(t.fg3);
            p->drawText(QRect(r.right() - 8 - tw, y1, tw, h1), Qt::AlignRight | Qt::AlignVCenter, time);
            p->setFont(bold);
            p->setPen(t.fg);
            const QFontMetrics fb(bold);
            const QString nm = fb.elidedText(a.name.isEmpty() ? QStringLiteral("agent") : a.name, Qt::ElideRight, r.width() / 2);
            const QRect nr(st.right() + 7, y1, fb.horizontalAdvance(nm), h1);
            p->drawText(nr, Qt::AlignLeft | Qt::AlignVCenter, nm);
            p->setFont(t.uiSmall);
            p->setPen(t.fg2);
            // the name is the task's start until the agent registers its own name: say it once
            if (!a.task.startsWith(a.name)) {
                const int taskW = r.right() - 16 - tw - nr.right() - 8;
                p->drawText(QRect(nr.right() + 8, y1, taskW, h1), Qt::AlignLeft | Qt::AlignVCenter, fs.elidedText(a.task, Qt::ElideRight, taskW));
            }
            // line 2: what it is doing, or how it ended
            const QRect l2(st.left() + 4, y1 + h1, r.width() - 24, fs.height() + 2);
            const QString stats = QStringLiteral("%1 tool%2 \u00b7 %3")
                                      .arg(a.tools)
                                      .arg(a.tools == 1 ? QString() : QStringLiteral("s"), tokens(a.tokens));
            const int sw = fs.horizontalAdvance(stats);
            p->setPen(t.fg3);
            p->drawText(QRect(r.right() - 8 - sw, l2.top(), sw, l2.height()), Qt::AlignRight | Qt::AlignVCenter, stats);
            QString doing;
            QColor ink = t.agent;
            if (a.status == QLatin1String("running") && !a.currentTool.isEmpty()) doing = QStringLiteral("\u25B8 ") + a.currentTool + QLatin1Char(' ') + a.currentArgs;
            else if (!a.output.isEmpty()) {
                doing = a.output;
                ink = a.status == QLatin1String("failed") ? t.err : t.fg2;
            }
            p->setPen(ink);
            p->setFont(t.monoSmall);
            const int dw = r.right() - 16 - sw - l2.left() - 6;
            p->drawText(QRect(l2.left() + 6, l2.top(), dw, l2.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(t.monoSmall).elidedText(doing, Qt::ElideRight, dw));
        }
    }

    TranscriptView* view_;
    mutable QHash<quint64, DocEntry> docs_, details_;
    mutable QHash<quint64, SizeEntry> sizes_;
};

// ==== view ========================================================================================================

TranscriptView::TranscriptView(QWidget* parent) : QListView(parent) {
    setObjectName(QStringLiteral("rk_transcript"));
    setFrameShape(QFrame::NoFrame);
    setUniformItemSizes(false);
    setResizeMode(QListView::Adjust);
    setLayoutMode(QListView::SinglePass);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setWordWrap(true);
    verticalScrollBar()->setSingleStep(24);
    delegate_ = new TranscriptDelegate(this);
    setItemDelegate(delegate_);

    pill_ = new QPushButton(QStringLiteral("\u2193 new"), this);
    pill_->setObjectName(QStringLiteral("rk_pill"));
    pill_->setCursor(Qt::PointingHandCursor);
    pill_->hide();
    connect(pill_, &QPushButton::clicked, this, &TranscriptView::scrollToEnd);

    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &TranscriptView::onRangeChanged);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &TranscriptView::onScrolled);
    spinner_.setInterval(kSpinMs);
    spinner_.setTimerType(Qt::PreciseTimer);
    connect(&spinner_, &QTimer::timeout, this, &TranscriptView::tickSpinner);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &TranscriptView::restyle);
    restyle();
}

TranscriptView::~TranscriptView() = default;

void TranscriptView::setTranscriptModel(TranscriptModel* model) {
    if (model_) disconnect(model_, nullptr, this, nullptr);
    model_ = model;
    setModel(model);
    if (model) {
        connect(model, &TranscriptModel::activityChanged, this, &TranscriptView::updateSpinner);
        connect(model, &TranscriptModel::awaitingChanged, this, [this] {
            if (model_) syncCards(0, model_->rowCount() - 1);
        });
    }
    reset();
}

void TranscriptView::setEmptyWidget(QWidget* widget) {
    empty_ = widget;
    if (widget) widget->setParent(this);
    syncEmpty();
}

TranscriptCard* TranscriptView::cardFor(quint64 itemId) const {
    auto it = cards_.constFind(itemId);
    return it == cards_.constEnd() ? nullptr : it.value().data();
}

void TranscriptView::scrollToEnd() {
    follow_ = true;
    unseen_ = 0;
    pill_->hide();
    settingScroll_ = true;
    scrollToBottom();
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    settingScroll_ = false;
}

void TranscriptView::onRangeChanged(int, int max) {
    if (follow_) {
        settingScroll_ = true;
        verticalScrollBar()->setValue(max);
        settingScroll_ = false;
    }
}

void TranscriptView::onScrolled(int value) {
    if (settingScroll_) return;
    follow_ = value >= verticalScrollBar()->maximum() - 4;
    if (follow_) {
        unseen_ = 0;
        pill_->hide();
    }
    closeSelection();
}

void TranscriptView::rowsInserted(const QModelIndex& parent, int start, int end) {
    QListView::rowsInserted(parent, start, end);
    syncCards(start, end);
    syncGroups(start, end);
    syncEmpty();
    if (!follow_) {
        unseen_ += end - start + 1;
        pill_->show();
        placePill();
    }
}

void TranscriptView::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles) {
    QListView::dataChanged(topLeft, bottomRight, roles);
    syncCards(topLeft.row(), bottomRight.row());
    syncGroups(topLeft.row(), bottomRight.row());
    // QListView keeps item sizes on dataChanged: the changed rows may have grown (one relayout for all of them)
    Q_EMIT delegate_->sizeHintChanged(topLeft);
    if (selection_ && model_)
        for (int r = topLeft.row(); r <= bottomRight.row() && r < model_->rowCount(); ++r)
            if (model_->at(r).id == selectionId_) closeSelection();
}

void TranscriptView::reset() {
    closeSelection();
    for (const QPointer<TranscriptCard>& c : std::as_const(cards_))
        if (c) c->deleteLater();
    cards_.clear();
    delegate_->clearCaches();
    QListView::reset();
    if (model_) {
        syncCards(0, model_->rowCount() - 1);
        syncGroups(0, model_->rowCount() - 1);
    }
    syncEmpty();
    unseen_ = 0;
    pill_->hide();
    follow_ = true;
    QTimer::singleShot(0, this, [this] {
        if (follow_) scrollToEnd();
    });
    updateSpinner();
}

// The calls of a closed group stay hidden under its line; a group that changed (opened, closed, grew) updates
// all of its calls.
void TranscriptView::syncGroups(int from, int to) {
    if (!model_) return;
    from = std::max(0, from);
    to = std::min(to, model_->rowCount() - 1);
    for (int r = from; r <= to; ++r) {
        const TranscriptItem& it = model_->at(r);
        if (it.groupCount >= 2) {
            const QList<int> rows = model_->groupRows(r);
            for (qsizetype i = 1; i < rows.size(); ++i)
                if (isRowHidden(rows[i]) == it.groupOpen) setRowHidden(rows[i], !it.groupOpen);
        } else if (it.groupHead) {
            const int head = model_->rowOfId(it.groupHead);
            const bool hide = head >= 0 && !model_->at(head).groupOpen;
            if (isRowHidden(r) != hide) setRowHidden(r, hide);
        }
    }
}

void TranscriptView::syncCards(int from, int to) {
    if (!model_) return;
    from = std::max(0, from);
    to = std::min(to, model_->rowCount() - 1);
    for (int r = from; r <= to; ++r) {
        const TranscriptItem& it = model_->at(r);
        if (!isCardKind(it.kind)) continue;
        const QModelIndex idx = model_->index(r, 0);
        TranscriptCard* existing = cardFor(it.id);
        if (it.resolved) {
            if (existing) {
                // the card may be inside its own click handler: unregister it from the next event loop pass
                cards_.remove(it.id);
                existing->hide();
                QMetaObject::invokeMethod(
                    this, [this, p = QPersistentModelIndex(idx)] {
                        if (p.isValid() && indexWidget(p)) setIndexWidget(p, nullptr);
                    },
                    Qt::QueuedConnection);
                Q_EMIT delegate_->sizeHintChanged(idx);
            }
            continue;
        }
        if (existing) continue;
        TranscriptCard* card = TranscriptCard::create(it, model_->chat(), viewport());
        if (!card) continue;
        const quint64 id = it.id;
        cards_.insert(id, card);
        connect(card, &TranscriptCard::answered, this, [this, id](const QString& resolution) {
            if (!model_) return;
            const int row = model_->rowOfId(id);
            if (row >= 0 && !model_->at(row).resolved) model_->resolveCard(row, resolution);
        });
        connect(card, &TranscriptCard::heightChanged, this, [this, id] {
            if (!model_) return;
            const int row = model_->rowOfId(id);
            if (row >= 0) Q_EMIT delegate_->sizeHintChanged(model_->index(row, 0));
        });
        setIndexWidget(idx, card);
        Q_EMIT delegate_->sizeHintChanged(idx);
        card->show();
    }
}

void TranscriptView::syncEmpty() {
    if (!empty_) return;
    const bool empty = !model_ || model_->rowCount() == 0;
    empty_->setGeometry(viewport()->geometry());
    empty_->setVisible(empty);
    if (empty) empty_->raise();
}

void TranscriptView::placePill() {
    const QSize s = pill_->sizeHint();
    const QRect vp = viewport()->geometry();
    pill_->setGeometry(vp.center().x() - s.width() / 2, vp.bottom() - s.height() - 10, s.width(), s.height());
    pill_->raise();
}

void TranscriptView::resizeEvent(QResizeEvent* e) {
    closeSelection();
    QListView::resizeEvent(e);
    syncEmpty();
    if (pill_->isVisible()) placePill();
    if (follow_) QTimer::singleShot(0, this, [this] {
            if (follow_) scrollToEnd();
        });
}

void TranscriptView::updateSpinner() {
    const bool running = model_ && model_->activity().running;
    if (running && !spinner_.isActive()) {
        if (!spinClock_.isValid()) spinClock_.start();
        spinner_.start();
    } else if (!running && spinner_.isActive()) {
        spinner_.stop();
        viewport()->update();
    }
}

void TranscriptView::tickSpinner() {
    if (!model_ || !isVisible()) return;
    delegate_->spinPhase = int((spinClock_.elapsed() % kSpinTurnMs) * 360 / kSpinTurnMs);
    // only visible rows that run something repaint
    const QRect vp = viewport()->rect();
    QModelIndex idx = indexAt(QPoint(kIndent, vp.top() + 1));
    if (!idx.isValid()) idx = model_->index(0, 0);
    for (int r = idx.row(); r >= 0 && r < model_->rowCount(); ++r) {
        const QRect rect = visualRect(model_->index(r, 0));
        if (rect.top() > vp.bottom()) break;
        if (delegate_->hasRunning(model_->at(r))) viewport()->update(rect);
    }
}

void TranscriptView::restyle() {
    const Theme& t = theme();
    delegate_->clearCaches();
    setStyleSheet(QStringLiteral("#rk_transcript { background: %1; border: none; }").arg(t.bg.name()));
    pill_->setStyleSheet(QStringLiteral("#rk_pill { background: %1; color: %2; border: none; border-radius: 10px; padding: 3px 12px; }")
                             .arg(t.accent.name(), t.accentFg.name()));
    scheduleDelayedItemsLayout();
    viewport()->update();
}

// ---- mouse ----

void TranscriptView::activate(const QModelIndex& index, const QPoint& pos) {
    const TranscriptDelegate::Hit h = delegate_->hit(index, visualRect(index), pos);
    switch (h.kind) {
    case TranscriptDelegate::HitKind::Chip: {
        bool ok = false;
        const quint64 ea = h.value.startsWith(QLatin1String("0x")) ? h.value.mid(2).toULongLong(&ok, 16) : h.value.toULongLong(&ok, 0);
        if (ok) Q_EMIT navigateRequested(ea);
        return;
    }
    case TranscriptDelegate::HitKind::Link:
        QDesktopServices::openUrl(QUrl(h.value));
        return;
    case TranscriptDelegate::HitKind::Expand:
        if (model_) model_->toggleExpanded(index.row());
        return;
    case TranscriptDelegate::HitKind::Group:
        if (model_) model_->toggleGroup(index.row());
        return;
    case TranscriptDelegate::HitKind::Undo:
        Q_EMIT undoRequested(h.value.toInt());
        return;
    case TranscriptDelegate::HitKind::Agent:
        if (!h.value.isEmpty()) Q_EMIT agentActivated(h.value);
        return;
    default:
        return;
    }
}

void TranscriptView::mousePressEvent(QMouseEvent* e) {
    const QModelIndex index = indexAt(e->position().toPoint());
    if (!index.isValid() || e->button() != Qt::LeftButton) {
        QListView::mousePressEvent(e);
        return;
    }
    const TranscriptDelegate::Hit h = delegate_->hit(index, visualRect(index), e->position().toPoint());
    if (h.kind == TranscriptDelegate::HitKind::Text || h.kind == TranscriptDelegate::HitKind::Detail) {
        openSelection(index, e);
        return;
    }
    closeSelection();
    e->accept();
}

void TranscriptView::mouseReleaseEvent(QMouseEvent* e) {
    const QModelIndex index = indexAt(e->position().toPoint());
    if (index.isValid() && e->button() == Qt::LeftButton) activate(index, e->position().toPoint());
    e->accept();
}

void TranscriptView::mouseDoubleClickEvent(QMouseEvent* e) { e->accept(); }

void TranscriptView::mouseMoveEvent(QMouseEvent* e) {
    const QModelIndex index = indexAt(e->position().toPoint());
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (index.isValid()) {
        switch (delegate_->hit(index, visualRect(index), e->position().toPoint()).kind) {
        case TranscriptDelegate::HitKind::Chip:
        case TranscriptDelegate::HitKind::Link:
        case TranscriptDelegate::HitKind::Expand:
        case TranscriptDelegate::HitKind::Undo:
        case TranscriptDelegate::HitKind::Agent:
        case TranscriptDelegate::HitKind::Group: shape = Qt::PointingHandCursor; break;
        case TranscriptDelegate::HitKind::Text:
        case TranscriptDelegate::HitKind::Detail: shape = Qt::IBeamCursor; break;
        default: break;
        }
    }
    viewport()->setCursor(shape);
    e->accept();
}

void TranscriptView::leaveEvent(QEvent* e) {
    viewport()->unsetCursor();
    QListView::leaveEvent(e);
}

// Text selection: a read-only browser over the row's document, opened where the press landed.
void TranscriptView::openSelection(const QModelIndex& index, QMouseEvent* press) {
    closeSelection();
    QRect where;
    const bool detail =
        delegate_->hit(index, visualRect(index), press->position().toPoint()).kind == TranscriptDelegate::HitKind::Detail;
    QTextDocument* doc = delegate_->selectableDoc(index, detail, &where);
    if (!doc || !model_) return;
    const Theme& t = theme();
    auto* b = new QTextBrowser(viewport());
    b->setObjectName(QStringLiteral("rk_selection"));
    b->setFrameShape(QFrame::NoFrame);
    b->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    b->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    b->setOpenLinks(false);
    b->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    b->setDocument(doc->clone(b));
    b->document()->setTextWidth(where.width());
    const TranscriptItem& it = model_->at(index.row());
    const QColor ink = it.kind == ItemKind::Notice ? t.fg3 : it.kind == ItemKind::Thinking ? t.fg2 : it.kind == ItemKind::Error ? t.err : t.fg;
    b->setStyleSheet(QStringLiteral("#rk_selection { background: transparent; color: %1; selection-background-color: %2; padding: 0; }")
                         .arg(ink.name(), t.selection.name()));
    b->viewport()->setAutoFillBackground(false);
    b->setGeometry(where.translated(visualRect(index).topLeft()));
    connect(b, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        const QString href = url.toString();
        if (href.startsWith(QLatin1String("rk:"))) {
            bool ok = false;
            const QString v = href.section(QLatin1Char(':'), 2);
            const quint64 ea = v.startsWith(QLatin1String("0x")) ? v.mid(2).toULongLong(&ok, 16) : v.toULongLong(&ok, 0);
            if (ok) Q_EMIT navigateRequested(ea);
        } else {
            QDesktopServices::openUrl(url);
        }
    });
    b->show();
    b->setFocus();
    selection_ = b;
    selectionId_ = it.id;
    delegate_->selectionItem = it.id;
    delegate_->selectionDetail = detail;
    viewport()->update(visualRect(index));
    // the press that opened it starts the selection
    QMouseEvent forwarded(press->type(), b->viewport()->mapFrom(viewport(), press->position().toPoint()), press->globalPosition(),
                          press->button(), press->buttons(), press->modifiers());
    QApplication::sendEvent(b->viewport(), &forwarded);
    connect(qApp, &QApplication::focusChanged, b, [this, b](QWidget*, QWidget* now) {
        if (now != b && selection_ == b) closeSelection();
    });
}

void TranscriptView::closeSelection() {
    if (!selection_) return;
    QTextBrowser* b = selection_;
    selection_ = nullptr;
    selectionId_ = 0;
    delegate_->selectionItem = 0;
    b->hide();
    b->deleteLater();
    viewport()->update();  // the row paints its text again
}

void TranscriptView::contextMenuEvent(QContextMenuEvent* e) {
    const QModelIndex index = indexAt(e->pos());
    if (!index.isValid() || !model_) return;
    const TranscriptItem& it = model_->at(index.row());
    QMenu menu(this);
    if (selection_ && selection_->textCursor().hasSelection()) {
        const QString sel = selection_->textCursor().selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        menu.addAction(tr("Copy selection"), this, [sel] { QApplication::clipboard()->setText(sel); });
    }
    const QString text = it.kind == ItemKind::Tool ? it.toolName + QLatin1Char(' ') + it.label : it.text;
    if (!text.isEmpty()) menu.addAction(tr("Copy"), this, [text] { QApplication::clipboard()->setText(text); });
    if (it.kind == ItemKind::Tool) {
        if (!it.result.isEmpty())
            menu.addAction(tr("Copy result"), this, [r = it.result] { QApplication::clipboard()->setText(r); });
        menu.addAction(it.expanded ? tr("Collapse") : tr("Show details"), this, [this, row = index.row()] {
            if (model_) model_->toggleExpanded(row);
        });
        if (it.mutationIndex >= 0 && it.mutationReversible && !it.undone)
            menu.addAction(tr("Undo this change"), this, [this, i = it.mutationIndex] { Q_EMIT undoRequested(i); });
    }
    if (menu.isEmpty()) return;
    menu.exec(e->globalPos());
}

}  // namespace rk::ui
