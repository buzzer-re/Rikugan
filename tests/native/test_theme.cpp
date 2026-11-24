// Theme derivation from host palettes: IDA light (default), a dark host (Binary Ninja's default), readable kind
// colours, one change signal per real palette change, and the palettes' fuzzy ranking.
#include <cmath>

#include <QSignalSpy>
#include <QWidget>
#include <QtTest>

#include "TestEnv.h"
#include "common/ui/Theme.h"

namespace {

double channel(double c) {
    c /= 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double lum(const QColor& c) { return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue()); }
double contrast(const QColor& a, const QColor& b) {
    return (std::max(lum(a), lum(b)) + 0.05) / (std::min(lum(a), lum(b)) + 0.05);
}
int distance(const QColor& a, const QColor& b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
}

QPalette palette(QRgb window, QRgb base, QRgb text, QRgb highlight) {
    QPalette p;
    p.setColor(QPalette::Window, QColor(window));
    p.setColor(QPalette::Base, QColor(base));
    p.setColor(QPalette::Text, QColor(text));
    p.setColor(QPalette::WindowText, QColor(text));
    p.setColor(QPalette::Highlight, QColor(highlight));
    return p;
}

const QPalette kIdaLight = palette(0xf0f0f0, 0xffffff, 0x000000, 0xd7e8fb);
const QPalette kBnDark = palette(0x2a2a2a, 0x232323, 0xe0e0e0, 0x4a6fa5);

}  // namespace

class ThemeTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void lightHostLooksLikeTheLightMockup() {
        const rk::ui::Theme t = rk::ui::deriveTheme(kIdaLight, QFont(), "ida");
        QVERIFY(!t.dark);
        QCOMPARE(t.bg, QColor(0xffffff));
        QVERIFY2(distance(t.bg2, QColor(0xf5f6f8)) < 24, qPrintable(t.bg2.name()));
        QVERIFY2(distance(t.line, QColor(0xe1e4e8)) < 40, qPrintable(t.line.name()));
        // IDA's pale selection blue is not an accent: the mockup's accent stands in
        QCOMPARE(t.accent, QColor(0x1667c4));
        QVERIFY(contrast(t.fg, t.bg) >= 7);
    }

    void darkHostLooksLikeTheDarkMockup() {
        const rk::ui::Theme t = rk::ui::deriveTheme(kBnDark, QFont(), "binja");
        QVERIFY(t.dark);
        QCOMPARE(t.bg, QColor(0x232323));
        QVERIFY2(distance(t.bg2, QColor(0x292929)) < 18, qPrintable(t.bg2.name()));
        QVERIFY2(distance(t.bg3, QColor(0x313131)) < 18, qPrintable(t.bg3.name()));
        QCOMPARE(t.accent, QColor(0x4a6fa5));  // the host's own highlight reads as an accent here
        QVERIFY(contrast(t.fg, t.bg) >= 7);
    }

    void kindColoursReadOnEveryBackground() {
        for (const QPalette& p : {kIdaLight, kBnDark, palette(0x3c3f41, 0x2b2b2b, 0xa9b7c6, 0x2f65ca),
                                  palette(0xfdf6e3, 0xfdf6e3, 0x657b83, 0x268bd2)}) {
            const rk::ui::Theme t = rk::ui::deriveTheme(p, QFont(), {});
            for (const QColor& c : {t.host_, t.workspace, t.web, t.agent, t.ok, t.warn, t.err, t.addr, t.sym})
                QVERIFY2(contrast(c, t.bg) >= 4.5, qPrintable(c.name() + " on " + t.bg.name()));
        }
    }

    void unreadableHostTextIsReplaced() {
        const rk::ui::Theme t = rk::ui::deriveTheme(palette(0x202020, 0x202020, 0x303030, 0x4a6fa5), QFont(), {});
        QVERIFY(contrast(t.fg, t.bg) >= 4.5);
    }

    void changedOncePerRealChange() {
        QWidget w;
        w.setPalette(kIdaLight);
        rk::ui::refreshTheme(&w);
        QSignalSpy spy(&rk::ui::ThemeNotifier::instance(), &rk::ui::ThemeNotifier::changed);
        rk::ui::refreshTheme(&w);  // same palette
        QCOMPARE(spy.count(), 0);
        w.setPalette(kBnDark);
        rk::ui::refreshTheme(&w);
        QCOMPARE(spy.count(), 1);
        QVERIFY(rk::ui::theme().dark);
        rk::ui::refreshTheme(&w);
        QCOMPARE(spy.count(), 1);
    }

    void fuzzyRanksLikeThePalettes() {
        using rk::ui::fuzzyScore;
        QVERIFY(fuzzyScore("gl", "goal") > fuzzyScore("gl", "dialog"));        // prefix beats a scattered match
        QVERIFY(fuzzyScore("mod", "modify") > fuzzyScore("mod", "model-hub-old"));
        QVERIFY(fuzzyScore("xyz", "goal") < 0);
        QVERIFY(fuzzyScore("tb", "tool bar") > fuzzyScore("tb", "tab"));  // a word start counts
        QList<int> idx;
        QVERIFY(fuzzyScore("btw", "by the way", &idx) >= 0);
        QCOMPARE(idx, (QList<int>{0, 3, 7}));
        QCOMPARE(fuzzyScore("", "anything"), 0);
    }
};

RK_TEST(ThemeTest)
#include "test_theme.moc"
