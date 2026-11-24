#include "common/ui/Theme.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QFontDatabase>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

namespace rk::ui {

namespace {

double channel(double c) {
    c /= 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& c) {
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// a toward b by t (0..1)
QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(const QColor& c, double a) {
    QColor o = c;
    o.setAlphaF(float(a));
    return o;
}

// `c` moved toward `fg` until it reads on `bg`
QColor readable(QColor c, const QColor& bg, const QColor& fg, double minimum) {
    for (int i = 0; i < 20 && contrast(c, bg) < minimum; ++i) c = mix(c, fg, 0.12);
    return c;
}

QColor opaque(const QColor& over, const QColor& under) {
    // what a translucent colour looks like over `under`
    return mix(under, QColor(over.red(), over.green(), over.blue()), over.alphaF());
}

Theme g_theme;
bool g_derived = false;
QString g_host;
QString g_signature;
std::function<QHash<QString, QColor>()> g_hostColors;

// The host's colours over the derived ones; tints that follow them (diff colours) follow again.
void applyHostColors(Theme& t, const QHash<QString, QColor>& colors) {
    static const QHash<QString, QColor Theme::*> roles{
        {"host", &Theme::host_},         {"workspace", &Theme::workspace}, {"web", &Theme::web},
        {"agent", &Theme::agent},        {"ok", &Theme::ok},               {"warn", &Theme::warn},
        {"err", &Theme::err},            {"addr", &Theme::addr},           {"sym", &Theme::sym},
        {"synComment", &Theme::synComment}, {"synString", &Theme::synString}, {"synNumber", &Theme::synNumber},
        {"synKeyword", &Theme::synKeyword}, {"synBuiltin", &Theme::synBuiltin}, {"synMeta", &Theme::synMeta},
    };
    for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
        const auto role = roles.constFind(it.key());
        if (role == roles.constEnd() || !it.value().isValid()) continue;
        const bool code = it.key().startsWith(QLatin1String("syn"));
        t.*(role.value()) = readable(QColor(it.value().rgb()), code ? t.codeBg : t.bg, t.fg, code ? 3.5 : 4.5);
    }
    t.diffAddBg = opaque(withAlpha(t.ok, t.dark ? 0.16 : 0.12), t.bg);
    t.diffDelBg = opaque(withAlpha(t.err, t.dark ? 0.16 : 0.10), t.bg);
    t.diffAddFg = t.ok;
    t.diffDelFg = t.err;
}

QString signatureOf(const QPalette& p, const QFont& f) {
    return p.color(QPalette::Base).name() + p.color(QPalette::Window).name() + p.color(QPalette::Text).name() +
           p.color(QPalette::Highlight).name() + f.toString();
}

QHash<QString, QIcon>& iconCache() {
    static QHash<QString, QIcon> c;
    return c;
}

QPixmap canvas(int logical, qreal dpr) {
    QPixmap pm(int(logical * dpr), int(logical * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    return pm;
}

void paintKind(QPainter& p, const QString& kind, const QColor& c, int s) {
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(c, 1.4);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double m = s / 2.0;
    if (kind == QLatin1String("host")) {  // diamond with a dot: the database
        QPainterPath d;
        d.moveTo(m, 2);
        d.lineTo(s - 2, m);
        d.lineTo(m, s - 2);
        d.lineTo(2, m);
        d.closeSubpath();
        p.drawPath(d);
        p.setBrush(c);
        p.drawEllipse(QPointF(m, m), 1.6, 1.6);
    } else if (kind == QLatin1String("workspace")) {  // a page with a folded corner
        QPainterPath d;
        d.moveTo(3.5, 2);
        d.lineTo(s - 6, 2);
        d.lineTo(s - 3.5, 4.5);
        d.lineTo(s - 3.5, s - 2);
        d.lineTo(3.5, s - 2);
        d.closeSubpath();
        p.drawPath(d);
        p.drawLine(QPointF(6, m), QPointF(s - 6, m));
        p.drawLine(QPointF(6, m + 3), QPointF(s - 7, m + 3));
    } else if (kind == QLatin1String("web")) {  // a globe
        p.drawEllipse(QPointF(m, m), m - 2, m - 2);
        p.drawEllipse(QPointF(m, m), (m - 2) * 0.45, m - 2);
        p.drawLine(QPointF(2.5, m), QPointF(s - 2.5, m));
    } else if (kind == QLatin1String("chip")) {  // a processor: the host's analysis tools
        const QRectF body(4, 4, s - 8, s - 8);
        p.drawRoundedRect(body, 1.5, 1.5);
        p.drawRect(QRectF(m - 1.5, m - 1.5, 3, 3));
        for (const double o : {m - 2.5, m + 2.5}) {
            p.drawLine(QPointF(o, 1.5), QPointF(o, 4));
            p.drawLine(QPointF(o, s - 4), QPointF(o, s - 1.5));
            p.drawLine(QPointF(1.5, o), QPointF(4, o));
            p.drawLine(QPointF(s - 4, o), QPointF(s - 1.5, o));
        }
    } else if (kind == QLatin1String("edit")) {  // a pencil
        QPainterPath d;
        d.moveTo(3, s - 3);
        d.lineTo(3.6, s - 6);
        d.lineTo(s - 5.5, 2.5);
        d.lineTo(s - 2.5, 5.5);
        d.lineTo(6, s - 3.6);
        d.closeSubpath();
        p.drawPath(d);
        p.drawLine(QPointF(s - 7.2, 4.2), QPointF(s - 4.2, 7.2));
    } else if (kind == QLatin1String("terminal")) {  // a prompt
        p.drawRoundedRect(QRectF(1.5, 2.5, s - 3, s - 5), 2, 2);
        p.drawLine(QPointF(4.5, m - 2), QPointF(7, m));
        p.drawLine(QPointF(7, m), QPointF(4.5, m + 2));
        p.drawLine(QPointF(8.5, m + 2.5), QPointF(s - 4.5, m + 2.5));
    } else if (kind == QLatin1String("expand")) {  // two arrows out to the corners: a larger view
        p.drawLine(QPointF(m + 1, m - 1), QPointF(s - 2.5, 2.5));
        p.drawLine(QPointF(s - 6.5, 2.5), QPointF(s - 2.5, 2.5));
        p.drawLine(QPointF(s - 2.5, 2.5), QPointF(s - 2.5, 6.5));
        p.drawLine(QPointF(m - 1, m + 1), QPointF(2.5, s - 2.5));
        p.drawLine(QPointF(2.5, s - 6.5), QPointF(2.5, s - 2.5));
        p.drawLine(QPointF(2.5, s - 2.5), QPointF(6.5, s - 2.5));
    } else if (kind == QLatin1String("collapse")) {  // two arrows in from the corners: back to the smaller view
        p.drawLine(QPointF(s - 2.5, 2.5), QPointF(m + 1, m - 1));
        p.drawLine(QPointF(m + 1, m - 5), QPointF(m + 1, m - 1));
        p.drawLine(QPointF(m + 1, m - 1), QPointF(m + 5, m - 1));
        p.drawLine(QPointF(2.5, s - 2.5), QPointF(m - 1, m + 1));
        p.drawLine(QPointF(m - 5, m + 1), QPointF(m - 1, m + 1));
        p.drawLine(QPointF(m - 1, m + 1), QPointF(m - 1, m + 5));
    } else if (kind == QLatin1String("search")) {  // a magnifier
        p.drawEllipse(QPointF(m - 1.2, m - 1.2), m - 4, m - 4);
        p.drawLine(QPointF(m + 2, m + 2), QPointF(s - 2.5, s - 2.5));
    } else if (kind == QLatin1String("chat")) {  // a speech bubble with two lines of text: a chat
        QPainterPath tail;
        tail.moveTo(3.8, s - 4.5);
        tail.lineTo(3, s - 1.5);
        tail.lineTo(7.5, s - 4.5);
        tail.closeSubpath();
        QPainterPath bubble;
        bubble.addRoundedRect(QRectF(1.5, 2, s - 3, s - 6.5), 3, 3);
        p.drawPath(bubble.united(tail));
        p.drawLine(QPointF(4.5, 5), QPointF(s - 4.5, 5));
        p.drawLine(QPointF(4.5, 7.5), QPointF(s - 6.5, 7.5));
    } else if (kind == QLatin1String("plug")) {  // a plug on its cable: connect a model
        p.drawLine(QPointF(m - 2, 1.5), QPointF(m - 2, 4.5));
        p.drawLine(QPointF(m + 2, 1.5), QPointF(m + 2, 4.5));
        QPainterPath body;
        body.moveTo(m - 4, 4.5);
        body.lineTo(m + 4, 4.5);
        body.lineTo(m + 4, 7);
        body.quadTo(m + 4, 9.5, m, 9.5);
        body.quadTo(m - 4, 9.5, m - 4, 7);
        body.closeSubpath();
        p.drawPath(body);
        p.drawLine(QPointF(m, 9.5), QPointF(m, s - 1.5));
    } else {  // agent: three linked nodes
        const QPointF a(m, 3.5), b(3.5, s - 3.5), d(s - 3.5, s - 3.5);
        p.drawLine(a, b);
        p.drawLine(a, d);
        p.setBrush(c);
        for (const QPointF& pt : {a, b, d}) p.drawEllipse(pt, 2, 2);
    }
}

void paintStatus(QPainter& p, const QString& status, int s) {
    p.setRenderHint(QPainter::Antialiasing);
    const Theme& t = theme();
    const double m = s / 2.0;
    QPen pen(t.fg3, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    if (status == QLatin1String("done")) {
        pen.setColor(t.ok);
        p.setPen(pen);
        QPainterPath d;
        d.moveTo(3, m + 0.5);
        d.lineTo(m - 1, s - 4);
        d.lineTo(s - 3, 4);
        p.drawPath(d);
    } else if (status == QLatin1String("error")) {
        pen.setColor(t.err);
        p.setPen(pen);
        p.drawLine(QPointF(4, 4), QPointF(s - 4, s - 4));
        p.drawLine(QPointF(s - 4, 4), QPointF(4, s - 4));
    } else if (status == QLatin1String("denied") || status == QLatin1String("cancelled")) {
        pen.setColor(status == QLatin1String("denied") ? t.err : t.fg3);
        p.setPen(pen);
        p.drawEllipse(QPointF(m, m), m - 2.5, m - 2.5);
        p.drawLine(QPointF(4.5, s - 4.5), QPointF(s - 4.5, 4.5));
    } else if (status == QLatin1String("running")) {
        pen.setColor(t.accent);
        p.setPen(pen);
        p.drawArc(QRectF(2.5, 2.5, s - 5, s - 5), 90 * 16, -270 * 16);
    } else {  // pending
        p.setPen(Qt::NoPen);
        p.setBrush(t.fg3);
        p.drawEllipse(QPointF(m, m), 2.2, 2.2);
    }
}

}  // namespace

QColor Theme::token(const QString& n) const {
    static const QHash<QString, QColor Theme::*> map{
        {"bg", &Theme::bg},           {"bg2", &Theme::bg2},         {"bg3", &Theme::bg3},           {"bg4", &Theme::bg4},
        {"fg", &Theme::fg},           {"fg2", &Theme::fg2},         {"fg3", &Theme::fg3},           {"line", &Theme::line},
        {"line2", &Theme::line2},     {"accent", &Theme::accent},   {"accentFg", &Theme::accentFg}, {"accentSoft", &Theme::accentSoft},
        {"accentSoft2", &Theme::accentSoft2}, {"focus", &Theme::focus}, {"selection", &Theme::selection},
        {"host", &Theme::host_},      {"workspace", &Theme::workspace}, {"web", &Theme::web},       {"agent", &Theme::agent},
        {"ok", &Theme::ok},           {"warn", &Theme::warn},       {"err", &Theme::err},           {"addr", &Theme::addr},
        {"sym", &Theme::sym},         {"userBg", &Theme::userBg},   {"userLine", &Theme::userLine}, {"codeBg", &Theme::codeBg},
        {"diffAddBg", &Theme::diffAddBg}, {"diffDelBg", &Theme::diffDelBg}, {"diffAddFg", &Theme::diffAddFg},
        {"diffDelFg", &Theme::diffDelFg}, {"shadow", &Theme::shadow},
    };
    auto it = map.constFind(n);
    return it == map.constEnd() ? QColor() : this->*(it.value());
}

Theme deriveTheme(const QPalette& pal, const QFont& font, const QString& host) {
    Theme t;
    t.host = host;
    QColor base = pal.color(QPalette::Active, QPalette::Base);
    QColor text = pal.color(QPalette::Active, QPalette::Text);
    if (contrast(base, text) < 4.5) text = luminance(base) < 0.4 ? QColor(0xe4e4e4) : QColor(0x1c2026);
    t.dark = luminance(base) < 0.2;
    t.bg = base;
    t.fg = text;
    // successive surfaces toward the text colour (step sizes from the mockup's tables)
    const double s1 = t.dark ? 0.031 : 0.044, s2 = t.dark ? 0.072 : 0.084, s3 = t.dark ? 0.124 : 0.14;
    t.bg2 = mix(base, text, s1);
    t.bg3 = mix(base, text, s2);
    t.bg4 = mix(base, text, s3);
    t.fg2 = mix(text, base, 0.28);
    t.fg3 = mix(text, base, 0.5);
    t.line = mix(base, text, t.dark ? 0.088 : 0.12);
    t.line2 = mix(base, text, t.dark ? 0.13 : 0.21);

    QColor accent = pal.color(QPalette::Active, QPalette::Highlight);
    // some hosts use a pale selection colour: keep the host's hue only when it reads as an accent
    if (contrast(accent, base) < 3.0) accent = t.dark ? QColor(0x5aa7ef) : QColor(0x1667c4);
    t.accent = accent;
    t.accentFg = contrast(QColor(Qt::white), accent) >= contrast(QColor(0x0f1a26), accent) ? QColor(Qt::white) : QColor(0x0f1a26);
    t.accentSoft = opaque(withAlpha(accent, t.dark ? 0.13 : 0.10), base);
    t.accentSoft2 = opaque(withAlpha(accent, t.dark ? 0.22 : 0.18), base);
    t.focus = accent;
    t.selection = opaque(withAlpha(pal.color(QPalette::Active, QPalette::Highlight), 0.35), base);

    // Rikugan's kind and status colours, readable on this background
    auto pick = [&](QRgb light, QRgb dark) { return readable(QColor(t.dark ? dark : light), base, text, 4.5); };
    t.host_ = pick(0x5b45c9, 0xab9cff);
    t.workspace = pick(0xa35f00, 0xe3b35f);
    t.web = pick(0x0b7f73, 0x4fc9b8);
    t.agent = pick(0xb32d82, 0xf17ec4);
    t.ok = pick(0x1f7a35, 0x6fcf83);
    t.warn = pick(0xa76400, 0xf0b54e);
    t.err = pick(0xc4252f, 0xff6b6b);
    t.addr = pick(0x007f3a, 0xa2d9af);
    t.sym = pick(0x0b4ee0, 0x80c6e9);
    t.userBg = opaque(withAlpha(accent, t.dark ? 0.10 : 0.07), mix(base, text, t.dark ? 0.03 : 0.0));
    t.userLine = opaque(withAlpha(accent, t.dark ? 0.22 : 0.16), base);
    t.codeBg = t.dark ? mix(base, QColor(Qt::black), 0.2) : mix(base, text, 0.025);
    t.diffAddBg = opaque(withAlpha(t.ok, t.dark ? 0.16 : 0.12), base);
    t.diffDelBg = opaque(withAlpha(t.err, t.dark ? 0.16 : 0.10), base);
    t.diffAddFg = t.ok;
    t.diffDelFg = t.err;
    t.shadow = QColor(0, 0, 0, t.dark ? 140 : 46);
    auto code = [&](QRgb light, QRgb dark) { return readable(QColor(t.dark ? dark : light), t.codeBg, text, 3.5); };
    t.synComment = code(0x5f7f52, 0x7fa96e);
    t.synString = code(0xa3411b, 0xd69d7f);
    t.synNumber = code(0x116644, 0xb5cea8);
    t.synKeyword = code(0x8a2b9e, 0xd09ad0);
    t.synBuiltin = code(0x795e26, 0xdcdcaa);
    t.synMeta = code(0x0b5fa5, 0x9cdcfe);

    t.ui = font;
    t.uiSmall = font;
    if (font.pointSizeF() > 0) t.uiSmall.setPointSizeF(std::max(8.0, font.pointSizeF() - 1));
    else if (font.pixelSize() > 0) t.uiSmall.setPixelSize(std::max(10, font.pixelSize() - 1));
    t.uiBold = font;
    t.uiBold.setWeight(QFont::DemiBold);
    t.mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (font.pointSizeF() > 0) t.mono.setPointSizeF(font.pointSizeF());
    t.monoSmall = t.mono;
    if (t.mono.pointSizeF() > 0) t.monoSmall.setPointSizeF(std::max(8.0, t.mono.pointSizeF() - 1));
    const int line = QFontMetrics(font).height();
    t.rowHeight = std::max(20, line + 6);
    t.gap = std::max(4, line / 3);
    t.animMs = QApplication::isEffectEnabled(Qt::UI_General) || !qApp ? 140 : 0;
    return t;
}

QColor blend(const QColor& a, const QColor& b, double t) { return mix(a, b, t); }

const Theme& theme() {
    if (!g_derived) refreshTheme(nullptr);
    return g_theme;
}

void setThemeHost(const QString& host) {
    g_host = host;
    g_theme.host = host;
}

void setHostColors(std::function<QHash<QString, QColor>()> source) {
    g_hostColors = std::move(source);
    g_signature.clear();  // the next refreshTheme() applies them
}

void refreshTheme(const QWidget* source) {
    const QPalette pal = source ? source->palette() : QApplication::palette();
    const QFont font = source ? source->font() : QApplication::font();
    const QHash<QString, QColor> hostColors = g_hostColors ? g_hostColors() : QHash<QString, QColor>();
    QString sig = signatureOf(pal, font) + g_host;
    QStringList keys = hostColors.keys();
    keys.sort();
    for (const QString& k : std::as_const(keys)) sig += k + hostColors.value(k).name();
    if (g_derived && sig == g_signature) return;
    const bool first = !g_derived;
    g_signature = sig;
    g_theme = deriveTheme(pal, font, g_host);
    applyHostColors(g_theme, hostColors);
    g_derived = true;
    iconCache().clear();
    if (!first) Q_EMIT ThemeNotifier::instance().changed();
}

ThemeNotifier& ThemeNotifier::instance() {
    static ThemeNotifier* n = new ThemeNotifier;  // never destroyed: outlives every panel
    return *n;
}

QString themeStylesheet() {
    const Theme& t = theme();
    auto c = [](const QColor& col) { return col.name(QColor::HexArgb); };
    const QString r = QString::number(t.radius);
    QString s;
    s += QStringLiteral("#rk_root { background: %1; color: %2; }").arg(c(t.bg), c(t.fg));
    s += QStringLiteral(
             "#rk_root QToolButton, #rk_root QPushButton { background: transparent; color: %1; border: 1px solid transparent;"
             " border-radius: %2px; padding: 2px 6px; }"
             "#rk_root QToolButton:hover, #rk_root QPushButton:hover { background: %3; }"
             "#rk_root QToolButton:pressed, #rk_root QPushButton:pressed { background: %4; }"
             "#rk_root QToolButton:focus, #rk_root QPushButton:focus { border-color: %5; }"
             "#rk_root QToolButton:disabled, #rk_root QPushButton:disabled { color: %6; }"
             "#rk_root QPushButton[rkPrimary=\"true\"] { background: %5; color: %7; border-color: %5; }"
             "#rk_root QPushButton[rkPrimary=\"true\"]:hover { background: %8; }"
             "#rk_root QPushButton[rkPrimary=\"true\"]:disabled { background: %4; color: %6; border-color: %4; }"
             "#rk_root QPushButton[rkDanger=\"true\"] { color: %9; border-color: %10; }")
             .arg(c(t.fg2), r, c(t.bg3), c(t.bg4), c(t.accent), c(t.fg3), c(t.accentFg), c(t.accent.lighter(110)), c(t.err))
             .arg(c(t.line2));
    s += QStringLiteral(
             "#rk_root QLineEdit, #rk_root QPlainTextEdit, #rk_root QTextEdit { background: %1; color: %2; border: 1px solid %3;"
             " border-radius: %4px; selection-background-color: %5; selection-color: %2; padding: 3px 6px; }"
             "#rk_root QLineEdit:focus, #rk_root QPlainTextEdit:focus { border-color: %6; }")
             .arg(c(t.bg2), c(t.fg), c(t.line2), r, c(t.selection), c(t.accent));
    s += QStringLiteral(
             "#rk_root QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
             "#rk_root QScrollBar::handle:vertical { background: %1; border-radius: 4px; min-height: 24px; }"
             "#rk_root QScrollBar::handle:vertical:hover { background: %2; }"
             "#rk_root QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }"
             "#rk_root QScrollBar::handle:horizontal { background: %1; border-radius: 4px; min-width: 24px; }"
             "#rk_root QScrollBar::add-line, #rk_root QScrollBar::sub-line, #rk_root QScrollBar::add-page,"
             " #rk_root QScrollBar::sub-page { background: none; border: none; width: 0; height: 0; }")
             .arg(c(t.line2), c(t.fg3));
    s += QStringLiteral(
             "#rk_root QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: %4px; padding: 4px; }"
             "#rk_root QMenu::item { padding: 4px 18px 4px 10px; border-radius: 4px; }"
             "#rk_root QMenu::item:selected { background: %5; }"
             "#rk_root QMenu::separator { height: 1px; background: %3; margin: 4px 6px; }"
             "#rk_root QToolTip { background: %6; color: %2; border: 1px solid %3; }"
             "#rk_root QCheckBox { color: %2; spacing: 6px; }")
             .arg(c(t.bg), c(t.fg), c(t.line2), r, c(t.accentSoft2), c(t.bg2));
    s += QStringLiteral(
             "#rk_root QLabel[rkMuted=\"true\"] { color: %2; }"
             "#rk_root QPushButton[rkOutline=\"true\"] { border-color: %3; }"
             "#rk_root QRadioButton { color: %1; }"
             "#rk_root QComboBox { background: %4; color: %1; border: 1px solid %3; border-radius: %5px; padding: 2px 6px; }"
             "#rk_root QComboBox QAbstractItemView { background: %6; color: %1; selection-background-color: %7; }"
             "#rk_root QTableWidget, #rk_root QTreeWidget { background: %6; color: %1; border: 1px solid %8;"
             " gridline-color: %8; alternate-background-color: %4; selection-background-color: %7; selection-color: %1; }"
             "#rk_root QHeaderView::section { background: %4; color: %9; border: none; border-right: 1px solid %8;"
             " border-bottom: 1px solid %8; padding: 3px 6px; }")
             .arg(c(t.fg), c(t.fg3), c(t.line2), c(t.bg2), r, c(t.bg), c(t.accentSoft2), c(t.line), c(t.fg2));
    s += QStringLiteral(
             "#rk_root QProgressBar { background: %1; border: 1px solid %2; border-radius: %3px; text-align: center;"
             " color: %4; }"
             "#rk_root QProgressBar::chunk { background: %5; border-radius: %3px; }")
             .arg(c(t.bg3), c(t.line2), r, c(t.fg2), c(t.accent));
    return s;
}

QIcon kindIcon(const QString& kind, const QColor& color) {
    const QString key = QStringLiteral("k:") + kind + color.name(QColor::HexArgb);
    auto it = iconCache().constFind(key);
    if (it != iconCache().constEnd()) return it.value();
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0}) {
        QPixmap pm = canvas(14, dpr);
        QPainter p(&pm);
        paintKind(p, kind, color, 14);
        p.end();
        icon.addPixmap(pm);
    }
    iconCache().insert(key, icon);
    return icon;
}

QIcon statusIcon(const QString& status) {
    const QString key = QStringLiteral("s:") + status;
    auto it = iconCache().constFind(key);
    if (it != iconCache().constEnd()) return it.value();
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0}) {
        QPixmap pm = canvas(14, dpr);
        QPainter p(&pm);
        paintStatus(p, status, 14);
        p.end();
        icon.addPixmap(pm);
    }
    iconCache().insert(key, icon);
    return icon;
}

int fuzzyScore(const QString& query, const QString& text, QList<int>* indices) {
    if (indices) indices->clear();
    if (query.isEmpty()) return 0;
    const QString t = text.toLower(), q = query.toLower();
    int qi = 0, last = -2;
    int score = 0;  // in hundredths, like the mockup's float score
    static const QString separators = QStringLiteral(" -_/.()");
    for (int i = 0; i < t.size() && qi < q.size(); ++i) {
        if (t.at(i) != q.at(qi)) continue;
        if (indices) indices->append(i);
        score += 100;
        if (i == last + 1) score += 200;
        if (i == 0 || separators.contains(t.at(i - 1))) score += 300;
        last = i;
        ++qi;
    }
    if (qi < q.size()) return -1;
    if (t.startsWith(q)) score += 600;
    return std::max(0, score - int(t.size()));
}

}  // namespace rk::ui
