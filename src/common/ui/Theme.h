#pragma once
// The panel's colours, fonts and metrics, derived from the host's QPalette so Rikugan looks native in IDA (light by
// default, dark themes too) and Binary Ninja (dark by default). The tool-kind, status, data and code colours come
// from the host's own theme when it names them (setHostColors: Binary Ninja's), else from Rikugan's defaults; both
// are contrast-corrected against the host background.
//
// UI thread only. theme() is a cached struct; refreshTheme() recomputes when the host palette or font changed and
// then emits ThemeNotifier::changed() once.
#include <functional>

#include <QColor>
#include <QFont>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QString>

QT_FORWARD_DECLARE_CLASS(QWidget)

namespace rk::ui {

struct Theme {
    QString host;  // "ida" | "binja" | ""
    bool dark = false;
    // surfaces and text
    QColor bg, bg2, bg3, bg4, fg, fg2, fg3, line, line2;
    QColor accent, accentFg, accentSoft, accentSoft2, focus, selection;
    // tool kinds and status
    QColor host_, workspace, web, agent, ok, warn, err;
    // data
    QColor addr, sym, userBg, userLine, codeBg, diffAddBg, diffDelBg, diffAddFg, diffDelFg, shadow;
    // code: comment, string, number, keyword, builtin, meta (readable on codeBg)
    QColor synComment, synString, synNumber, synKeyword, synBuiltin, synMeta;
    QFont ui, uiSmall, uiBold, mono, monoSmall;
    int radius = 6, rowHeight = 22, gap = 6;
    int animMs = 140;  // 0 when the platform turns UI animations off

    // A colour by token name ("bg2", "accent", "host" ...); invalid for unknown names.
    QColor token(const QString& name) const;
};

const Theme& theme();
void setThemeHost(const QString& host);
// Where the host's theme colours come from, read at every refreshTheme(): token name (host, workspace, web, agent,
// ok, warn, err, addr, sym, synComment, synString, synNumber, synKeyword, synBuiltin, synMeta) -> colour.
void setHostColors(std::function<QHash<QString, QColor>()> source);
// Re-derives the theme from `source`'s palette and font (the application's when null). Emits changed() only when
// they differ from the last derivation.
void refreshTheme(const QWidget* source = nullptr);
// The derivation itself (tests feed synthetic palettes).
Theme deriveTheme(const QPalette& palette, const QFont& font, const QString& host);
// `a` toward `b` by t (0..1): tints for painted chips and badges.
QColor blend(const QColor& a, const QColor& b, double t);

class ThemeNotifier : public QObject {
    Q_OBJECT
public:
    static ThemeNotifier& instance();
Q_SIGNALS:
    void changed();
};

// QSS for Rikugan's own widgets: everything under a widget named "rk_root" (the panel and each popup set that object
// name on their top level). Never styles the host.
QString themeStylesheet();

// Painted icons, cached per colour and theme. kind: host | workspace | web | chip | edit | terminal | search | chat |
// plug | agent; status: running | done | error | denied | pending | cancelled.
QIcon kindIcon(const QString& kind, const QColor& color);
QIcon statusIcon(const QString& status);

// Subsequence match with bonuses for consecutive letters, word starts and a prefix (the mockup's scorer). Returns
// < 0 when `query` is not a subsequence of `text`; `indices` gets the matched positions.
int fuzzyScore(const QString& query, const QString& text, QList<int>* indices = nullptr);

}  // namespace rk::ui
