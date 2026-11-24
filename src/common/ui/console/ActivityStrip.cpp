#include "common/ui/console/ActivityStrip.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/ui/Theme.h"

namespace rk::ui {

namespace {

constexpr int kTickMs = 100;       // the elapsed-time text
constexpr int kFrameMs = 16;       // the spinner
constexpr int kTurnMs = 900;       // one turn of the spinner
constexpr int kTwoColumnWidth = 520;

QString clock(qint64 seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds >= 3600) return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString secs(qint64 ms) { return QString::number(double(ms) / 1000.0, 'f', 1) + QLatin1Char('s'); }

QString css(const QColor& c) { return c.name(QColor::HexArgb); }

}  // namespace

// ---- chip ----

StripChip::StripChip(QWidget* parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void StripChip::setContent(const QString& glyph, const QString& text, const QColor& ink) {
    glyph_ = glyph;
    text_ = text;
    ink_ = ink;
    updateGeometry();
    update();
}

void StripChip::setBars(const QList<QColor>& bars) {
    bars_ = bars;
    updateGeometry();
    update();
}

void StripChip::setProgress(int done, int total) {
    done_ = done;
    total_ = total;
    updateGeometry();
    update();
}

void StripChip::setAccent(bool accent) {
    accent_ = accent;
    update();
}

QSize StripChip::sizeHint() const {
    const Theme& t = theme();
    const QFontMetrics fm(t.monoSmall);
    int w = 10 + fm.horizontalAdvance(glyph_) + (glyph_.isEmpty() ? 0 : 5) + fm.horizontalAdvance(text_) + 10;
    if (!bars_.isEmpty()) w += 6 + int(std::min<qsizetype>(bars_.size(), 8)) * 4;
    if (total_ >= 0) w += 6 + 26;
    return {w, std::max(20, fm.height() + 6)};
}

void StripChip::paintEvent(QPaintEvent*) {
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool hover = underMouse();
    p.setPen(QPen(accent_ ? t.userLine : t.line2, 1));
    p.setBrush(accent_ ? t.accentSoft : hover ? t.bg3 : t.bg);
    p.drawRoundedRect(r, 5, 5);
    const QFontMetrics fm(t.monoSmall);
    int x = 10;
    p.setFont(t.monoSmall);
    if (!glyph_.isEmpty()) {
        p.setPen(ink_);
        p.drawText(QRect(x, 0, fm.horizontalAdvance(glyph_) + 1, height()), Qt::AlignVCenter, glyph_);
        x += fm.horizontalAdvance(glyph_) + 5;
    }
    QFont f = t.monoSmall;
    f.setWeight(accent_ ? QFont::Bold : QFont::Normal);
    p.setFont(f);
    p.setPen(accent_ ? t.accent : t.fg2);
    p.drawText(QRect(x, 0, fm.horizontalAdvance(text_) + 2, height()), Qt::AlignVCenter, text_);
    x += fm.horizontalAdvance(text_) + 6;
    p.setPen(Qt::NoPen);
    const int cy = height() / 2;
    for (int i = 0; i < bars_.size() && i < 8; ++i) {
        p.setBrush(bars_[i]);
        p.drawRoundedRect(QRectF(x + i * 4, cy - 4, 2.5, 8), 1, 1);
    }
    if (total_ >= 0) {
        const QRectF track(x, cy - 2, 26, 4);
        p.setBrush(t.bg4);
        p.drawRoundedRect(track, 2, 2);
        if (total_ > 0 && done_ > 0) {
            p.setBrush(done_ >= total_ ? t.ok : t.accent);
            p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * done_ / total_, track.height()), 2, 2);
        }
    }
}

// ---- state mark ----

StateGlyph::StateGlyph(QWidget* parent) : QWidget(parent) {
    setFixedSize(14, 14);
    frame_.setInterval(kFrameMs);
    frame_.setTimerType(Qt::PreciseTimer);
    connect(&frame_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void StateGlyph::setSpinning(const QColor& ink) {
    glyph_.clear();
    ink_ = ink;
    if (!frame_.isActive()) {
        clock_.start();
        frame_.start();
    }
    update();
}

void StateGlyph::setGlyph(const QString& glyph, const QColor& ink) {
    frame_.stop();
    if (glyph == glyph_ && ink == ink_) return;
    glyph_ = glyph;
    ink_ = ink;
    update();
}

void StateGlyph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (frame_.isActive()) {
        const int angle = int((clock_.elapsed() % kTurnMs) * 360 / kTurnMs);
        p.setPen(QPen(ink_, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(rect()).adjusted(2, 2, -2, -2), -angle * 16, 270 * 16);
        return;
    }
    p.setPen(ink_);
    p.setFont(theme().uiSmall);
    p.drawText(rect(), Qt::AlignCenter, glyph_);
}

// ---- strip ----

ActivityStrip::ActivityStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_strip"));
    setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(10, 4, 8, 4);
    v->setSpacing(4);

    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    stateIcon_ = new StateGlyph(this);
    row->addWidget(stateIcon_);
    state_ = new QLabel(this);
    state_->setTextFormat(Qt::RichText);
    state_->setMinimumWidth(40);
    state_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(state_, 1);
    agents_ = new StripChip(this);
    agents_->setToolTip(tr("Agents (Ctrl+J)"));
    row->addWidget(agents_);
    todo_ = new StripChip(this);
    todo_->setToolTip(tr("Todo list"));
    row->addWidget(todo_);
    goalChip_ = new StripChip(this);
    goalChip_->setToolTip(tr("Time spent on the goal"));
    row->addWidget(goalChip_);
    mode_ = new StripChip(this);
    mode_->setAccent(true);
    mode_->setCursor(Qt::ArrowCursor);
    row->addWidget(mode_);
    stop_ = new QPushButton(QStringLiteral("\u25A0 ") + tr("Stop"), this);
    stop_->setProperty("rkOutline", true);
    stop_->setCursor(Qt::PointingHandCursor);
    stop_->setToolTip(tr("Stop the chat (Esc)"));
    stop_->hide();
    row->addWidget(stop_);
    chevron_ = new QToolButton(this);
    chevron_->setAutoRaise(true);
    chevron_->setCursor(Qt::PointingHandCursor);
    chevron_->setToolTip(tr("Show or hide the todo list"));
    row->addWidget(chevron_);
    v->addLayout(row);

    goal_ = new QFrame(this);
    goal_->setObjectName(QStringLiteral("rk_goal"));
    goal_->setAttribute(Qt::WA_StyledBackground);
    auto* g = new QHBoxLayout(goal_);
    g->setContentsMargins(8, 3, 4, 3);
    g->setSpacing(6);
    auto* goalTag = new QLabel(QStringLiteral("\u25CE GOAL"), goal_);
    goalTag->setObjectName(QStringLiteral("rk_goal_tag"));
    g->addWidget(goalTag);
    goalText_ = new QLabel(goal_);
    goalText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    g->addWidget(goalText_, 1);
    goalTime_ = new QLabel(goal_);
    goalTime_->setObjectName(QStringLiteral("rk_goal_time"));
    g->addWidget(goalTime_);
    goalPause_ = new QPushButton(tr("Pause"), goal_);
    goalPause_->setProperty("rkOutline", true);
    g->addWidget(goalPause_);
    auto* goalClear = new QPushButton(tr("Clear"), goal_);
    goalClear->setProperty("rkOutline", true);
    g->addWidget(goalClear);
    goal_->hide();
    v->addWidget(goal_);

    todoBox_ = new QFrame(this);
    todoGrid_ = new QGridLayout(todoBox_);
    todoGrid_->setContentsMargins(2, 0, 2, 2);
    todoGrid_->setHorizontalSpacing(16);
    todoGrid_->setVerticalSpacing(1);
    todoBox_->hide();
    v->addWidget(todoBox_);

    connect(agents_, &StripChip::clicked, this, &ActivityStrip::agentsClicked);
    connect(todo_, &StripChip::clicked, this, [this] { setTodoExpanded(!todoOpen_); });
    connect(chevron_, &QToolButton::clicked, this, [this] { setTodoExpanded(!todoOpen_); });
    connect(stop_, &QPushButton::clicked, this, &ActivityStrip::stopRequested);
    connect(goalPause_, &QPushButton::clicked, this, [this] {
        if (a_.goal.value(QLatin1String("status")).toString() == QLatin1String("paused")) Q_EMIT goalResume();
        else Q_EMIT goalPause();
    });
    connect(goalClear, &QPushButton::clicked, this, &ActivityStrip::goalClear);
    clock_.setInterval(kTickMs);
    connect(&clock_, &QTimer::timeout, this, [this] { refreshState(); });
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        restyle();
        refreshState();
        shownTodo_ = {};
        rebuildTodo();
    });
    restyle();
    refreshState();
}

void ActivityStrip::clear() {
    a_ = ChatActivity{};
    wasRunning_ = false;
    lastTurnMs_ = 0;
    shownTodo_ = {};
    clock_.stop();
    refreshState();
    rebuildTodo();
}

void ActivityStrip::setActivity(const ChatActivity& a) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (wasRunning_ && !a.running && a_.turnStartMs) lastTurnMs_ = now - a_.turnStartMs;
    if (a.running && !wasRunning_) lastTurnMs_ = 0;
    wasRunning_ = a.running;
    if (a.goal != a_.goal) goalSeenMs_ = now;
    a_ = a;
    if (a.running && !clock_.isActive()) clock_.start();
    else if (!a.running && clock_.isActive()) clock_.stop();
    refreshState();
    if (a_.todoPhases != shownTodo_) rebuildTodo();
}

void ActivityStrip::setTodoExpanded(bool open) {
    todoOpen_ = open;
    todoBox_->setVisible(open && todoGrid_->count() > 0);
    refreshState();
}

void ActivityStrip::refreshState() {
    const Theme& t = theme();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString icon, text;
    QColor iconInk = t.fg3;
    const QString mono = t.monoSmall.family().toHtmlEscaped();
    auto muted = [&](const QString& s) { return QStringLiteral("<span style=\"color:%1\">%2</span>").arg(t.fg3.name(), s.toHtmlEscaped()); };
    if (a_.running) {
        iconInk = t.accent;
        const int agents = a_.runningAgents();
        if (!a_.currentTool.isEmpty()) {
            const QString name = a_.currentTool.section(QLatin1Char(' '), 0, 0);
            const QString args = a_.currentTool.section(QLatin1Char(' '), 1);
            text = QStringLiteral("<span style=\"font-family:'%1'; font-weight:600\">%2</span> %3").arg(mono, name.toHtmlEscaped(), muted(args));
        } else if (agents > 0) {
            text = QStringLiteral("<b>task</b> ") +
                   muted(agents == 1 ? tr("1 agent running") : tr("%1 agents running").arg(agents));
        } else if (!a_.phase.isEmpty()) {
            text = muted(a_.phase + QStringLiteral("\u2026"));
        } else {
            text = muted(tr("Thinking\u2026"));
        }
        text += QStringLiteral(" ") + muted(QStringLiteral("\u00b7 ") + secs(now - a_.turnStartMs));
    } else if (a_.lastOutcome == QLatin1String("error")) {
        icon = QStringLiteral("\u2715");
        iconInk = t.err;
        text = QStringLiteral("<span style=\"color:%1\">%2</span>").arg(t.err.name(), tr("Stopped on an error"));
    } else if (a_.lastOutcome == QLatin1String("cancelled")) {
        icon = QStringLiteral("\u25A0");
        text = muted(tr("Stopped"));
    } else if (lastTurnMs_ > 0 || a_.lastOutcome == QLatin1String("idle")) {
        icon = QStringLiteral("\u25CF");
        iconInk = t.ok;
        QStringList parts{tr("Done")};
        if (lastTurnMs_ > 0) parts << secs(lastTurnMs_);
        if (a_.undoable > 0)
            parts << (a_.undoable == 1 ? tr("1 undoable change") : tr("%1 undoable changes").arg(a_.undoable));
        text = muted(parts.join(QStringLiteral(" \u00b7 ")));
    } else {
        icon = QStringLiteral("\u25CB");
        text = muted(tr("Ready"));
    }
    if (a_.running) stateIcon_->setSpinning(iconInk);
    else stateIcon_->setGlyph(icon, iconInk);
    state_->setText(text);

    // agents: one bar per agent, coloured by state
    if (a_.agents.isEmpty()) {
        agents_->hide();
    } else {
        QList<QColor> bars;
        for (const AgentRow& r : a_.agents)
            bars << (r.status == QLatin1String("running") ? t.agent : r.status == QLatin1String("failed") ? t.err : r.status == QLatin1String("done") ? t.ok : t.fg3);
        agents_->setContent(QStringLiteral("\u2442"), QString::number(a_.runningAgents() ? a_.runningAgents() : int(a_.agents.size())), t.agent);
        agents_->setBars(bars);
        agents_->show();
    }
    const auto [done, total] = a_.todoProgress();
    todo_->setVisible(total > 0);
    if (total > 0) {
        todo_->setContent(QStringLiteral("\u2611"), QStringLiteral("%1/%2").arg(done).arg(total), t.fg2);
        todo_->setProgress(done, total);
    }
    chevron_->setVisible(total > 0);
    chevron_->setText(todoOpen_ ? QStringLiteral("\u25B4") : QStringLiteral("\u25BE"));

    const bool goal = !a_.goal.isEmpty();
    goal_->setVisible(goal);
    goalChip_->setVisible(goal);
    if (goal) {
        const QString status = a_.goal.value(QLatin1String("status")).toString();
        qint64 used = qint64(a_.goal.value(QLatin1String("timeUsedSeconds")).toDouble());
        if (a_.running && status == QLatin1String("active") && goalSeenMs_) used += (now - goalSeenMs_) / 1000;
        goalChip_->setContent(QStringLiteral("\u25CE"), clock(used), t.warn);
        goalTime_->setText(clock(used));
        const QString objective = a_.goal.value(QLatin1String("objective")).toString().simplified();
        goalText_->setText(goalText_->fontMetrics().elidedText(objective, Qt::ElideRight, std::max(40, goalText_->width())));
        goalText_->setToolTip(objective);
        goalPause_->setText(status == QLatin1String("paused") ? tr("Resume") : tr("Pause"));
    }
    mode_->setVisible(!a_.mode.isEmpty());
    if (!a_.mode.isEmpty()) {
        mode_->setContent(QStringLiteral("\u25A4"), a_.mode.toUpper(), t.accent);
        mode_->setToolTip(a_.phase.isEmpty() ? tr("%1 mode").arg(a_.mode) : tr("%1 mode, %2 phase").arg(a_.mode, a_.phase));
    }
    stop_->setVisible(a_.running);
}

void ActivityStrip::rebuildTodo() {
    shownTodo_ = a_.todoPhases;
    while (QLayoutItem* item = todoGrid_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const Theme& t = theme();
    struct Task {
        QString text, status;
    };
    QList<Task> tasks;
    for (const QJsonValue& p : a_.todoPhases)
        for (const QJsonValue& v : p.toObject().value(QLatin1String("tasks")).toArray())
            tasks << Task{v.toObject().value(QLatin1String("content")).toString(), v.toObject().value(QLatin1String("status")).toString()};
    todoColumns_ = width() >= kTwoColumnWidth ? 2 : 1;
    const int perColumn = int((tasks.size() + todoColumns_ - 1) / todoColumns_);
    for (int i = 0; i < tasks.size(); ++i) {
        const Task& task = tasks[i];
        QString glyph = QStringLiteral("\u25CB");
        QColor ink = t.fg2, glyphInk = t.fg3;
        QString style;
        if (task.status == QLatin1String("in_progress")) {
            glyph = QStringLiteral("\u25D0");
            glyphInk = t.accent;
            ink = t.fg;
            style = QStringLiteral("font-weight:600;");
        } else if (task.status == QLatin1String("completed")) {
            glyph = QStringLiteral("\u2714");
            glyphInk = t.ok;
            ink = t.fg3;
            style = QStringLiteral("text-decoration:line-through;");
        } else if (task.status == QLatin1String("abandoned")) {
            glyph = QStringLiteral("\u2013");
            ink = t.fg3;
        }
        auto* l = new QLabel(todoBox_);
        l->setTextFormat(Qt::RichText);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        l->setText(QStringLiteral("<span style=\"color:%1\">%2</span>&nbsp;&nbsp;<span style=\"color:%3;%4\">%5</span>")
                       .arg(glyphInk.name(), glyph, ink.name(), style, task.text.toHtmlEscaped()));
        l->setToolTip(task.text);
        todoGrid_->addWidget(l, perColumn ? i % perColumn : i, perColumn ? i / perColumn : 0);
    }
    todoBox_->setVisible(todoOpen_ && !tasks.isEmpty());
    refreshState();
}

void ActivityStrip::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    const int columns = width() >= kTwoColumnWidth ? 2 : 1;
    if (columns != todoColumns_ && !a_.todoPhases.isEmpty()) rebuildTodo();
    if (goal_->isVisible()) refreshState();
}

void ActivityStrip::restyle() {
    const Theme& t = theme();
    setStyleSheet(QStringLiteral("#rk_strip { background: %1; border-top: 1px solid %2; }"
                                 "#rk_strip QLabel { color: %3; background: transparent; }"
                                 "#rk_goal { background: %4; border: 1px solid %5; border-radius: 6px; }"
                                 "#rk_strip #rk_goal_tag { color: %6; font-weight: bold; }"
                                 "#rk_strip #rk_goal_time { color: %7; }"
                                 "#rk_strip QPushButton[rkOutline=\"true\"] { border: 1px solid %2; background: %1; padding: 1px 8px; }")
                      .arg(css(t.bg2), css(t.line), css(t.fg), css(blend(t.warn, t.bg, 0.9)), css(blend(t.warn, t.bg, 0.55)), css(t.warn),
                           css(t.fg3)));
    state_->setFont(t.uiSmall);
}

}  // namespace rk::ui
