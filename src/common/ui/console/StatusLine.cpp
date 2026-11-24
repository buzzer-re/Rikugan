#include "common/ui/console/StatusLine.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>

#include "common/ui/Theme.h"

namespace rk::ui {

namespace {

QString tokens(qint64 n) {
    if (n < 1000) return QString::number(n);
    if (n < 1000000) return QString::number(double(n) / 1000.0, 'f', 1) + QLatin1Char('k');
    return QString::number(double(n) / 1e6, 'f', 1) + QLatin1Char('M');
}

}  // namespace

StatusLine::StatusLine(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_status"));
    setMouseTracking(true);
    clock_.setInterval(1000);
    connect(&clock_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(QFontMetrics(theme().monoSmall).height() + 8);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        setMinimumHeight(QFontMetrics(theme().monoSmall).height() + 8);
        update();
    });
}

QSize StatusLine::sizeHint() const { return {200, QFontMetrics(theme().monoSmall).height() + 8}; }

void StatusLine::setActivity(const ChatActivity& a) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (wasRunning_ && !a.running && a_.turnStartMs) lastTurnMs_ = now - a_.turnStartMs;
    wasRunning_ = a.running;
    a_ = a;
    if (a.running && !clock_.isActive()) clock_.start();
    else if (!a.running) clock_.stop();
    update();
}

void StatusLine::setModelFallback(const QString& model) {
    fallbackModel_ = model;
    update();
}

void StatusLine::setLocation(quint64 address, const QString& function) {
    address_ = QStringLiteral("0x%1").arg(address, 0, 16);
    function_ = function;
    update();
}

void StatusLine::paintEvent(QPaintEvent*) {
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), t.bg2);
    p.setPen(t.line);
    p.drawLine(0, 0, width(), 0);
    const QFontMetrics fm(t.monoSmall);
    const int h = height();
    int x = 8;
    p.setFont(t.monoSmall);
    auto text = [&](const QString& s, const QColor& c, bool bold = false) {
        QFont f = t.monoSmall;
        if (bold) f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(c);
        const int w = QFontMetrics(f).horizontalAdvance(s);
        p.drawText(QRect(x, 0, w + 1, h), Qt::AlignVCenter, s);
        x += w;
    };
    auto sep = [&] {
        x += 6;
        p.setPen(t.line2);
        p.drawLine(x, 5, x, h - 5);
        x += 7;
    };
    // the cursor, right-aligned (drawn first so the left part knows where to stop)
    int right = width() - 8;
    locationRect_ = {};
    if (!address_.isEmpty()) {
        const QString loc = QStringLiteral("\u2316 ") + address_ + (function_.isEmpty() ? QString() : QStringLiteral(" ") + function_);
        const int w = std::min(fm.horizontalAdvance(loc), width() / 2);
        locationRect_ = QRect(right - w, 0, w, h);
        p.setPen(t.addr);
        p.drawText(locationRect_, Qt::AlignVCenter | Qt::AlignRight, fm.elidedText(loc, Qt::ElideRight, w));
        right = locationRect_.left() - 12;
    }
    p.save();
    p.setClipRect(QRect(0, 0, std::max(0, right), h));
    QString model = a_.model.isEmpty() ? fallbackModel_ : a_.model;
    model = model.section(QLatin1Char('/'), -1);
    const int modelStart = x;
    text(model.isEmpty() ? tr("no model") : model, t.fg, true);
    if (!a_.thinking.isEmpty() && a_.thinking != QLatin1String("off")) text(QStringLiteral(" \u00b7") + a_.thinking, t.fg3);
    modelRect_ = QRect(modelStart, 0, x - modelStart, h);
    if (a_.contextPercent >= 0 || a_.contextTokens > 0) {
        sep();
        const int pct = std::clamp(a_.contextPercent, 0, 100);
        const QRectF track(x, h / 2.0 - 2, 34, 4);
        p.setPen(Qt::NoPen);
        p.setBrush(t.bg4);
        p.drawRoundedRect(track, 2, 2);
        p.setBrush(pct >= 85 ? t.err : pct >= 65 ? t.warn : t.accent);
        p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * pct / 100.0, track.height()), 2, 2);
        x += 40;
        text(QStringLiteral("%1%").arg(pct), t.fg2);
    }
    if (a_.tokensIn || a_.tokensOut) {
        sep();
        text(tokens(a_.tokensIn) + QStringLiteral(" \u2191 ") + tokens(a_.tokensOut) + QStringLiteral(" \u2193"), t.fg2);
    }
    if (a_.cost > 0) {
        sep();
        text(QStringLiteral("$%1").arg(a_.cost, 0, 'f', 2), t.fg2);
    }
    const qint64 ms = a_.running ? QDateTime::currentMSecsSinceEpoch() - a_.turnStartMs : lastTurnMs_;
    if (ms > 0) {
        sep();
        text(QString::number(double(ms) / 1000.0, 'f', 1) + QLatin1Char('s'), a_.running ? t.accent : t.fg3);
    }
    p.restore();
}

void StatusLine::mousePressEvent(QMouseEvent* e) {
    if (locationRect_.contains(e->position().toPoint()) && !address_.isEmpty()) Q_EMIT locationClicked(address_);
    else if (modelRect_.contains(e->position().toPoint())) Q_EMIT modelClicked();
}

void StatusLine::mouseMoveEvent(QMouseEvent* e) {
    const QPoint p = e->position().toPoint();
    setCursor(locationRect_.contains(p) || modelRect_.contains(p) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setToolTip(locationRect_.contains(p) ? tr("Put the address in the message") : modelRect_.contains(p) ? tr("Change the model") : QString());
}

}  // namespace rk::ui
