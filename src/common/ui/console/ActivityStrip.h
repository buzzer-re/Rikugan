#pragma once
// The line above the composer: what the chat is doing now (thinking, the running tool, its agents) or how the last
// turn ended, with chips for the agents (opens the agents panel), the todo progress, the goal, the running mode and
// Stop. Below it the goal banner (Pause / Resume / Clear) and the live todo list. Its clocks run only while the
// chat works: the elapsed time ten times a second, the spinner every frame.
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QList>
#include <QTimer>
#include <QWidget>

#include "common/ui/transcript/ChatActivity.h"

QT_FORWARD_DECLARE_CLASS(QFrame)
QT_FORWARD_DECLARE_CLASS(QGridLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QToolButton)

namespace rk::ui {

// A small rounded chip: glyph, text, then mini bars (agents) or a progress bar (todo).
class StripChip : public QAbstractButton {
    Q_OBJECT
public:
    explicit StripChip(QWidget* parent = nullptr);
    void setContent(const QString& glyph, const QString& text, const QColor& ink);
    void setBars(const QList<QColor>& bars);
    void setProgress(int done, int total);
    void setAccent(bool accent);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString glyph_, text_;
    QColor ink_;
    QList<QColor> bars_;
    int done_ = 0, total_ = -1;
    bool accent_ = false;
};

// The strip's state mark: a turning arc while the chat works (painted every frame, only then), else a glyph.
class StateGlyph : public QWidget {
    Q_OBJECT
public:
    explicit StateGlyph(QWidget* parent = nullptr);
    void setSpinning(const QColor& ink);
    void setGlyph(const QString& glyph, const QColor& ink);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString glyph_;
    QColor ink_;
    QTimer frame_;
    QElapsedTimer clock_;
};

class ActivityStrip : public QWidget {
    Q_OBJECT
public:
    explicit ActivityStrip(QWidget* parent = nullptr);

    void setActivity(const ChatActivity& a);
    void clear();
    bool todoExpanded() const { return todoOpen_; }
    void setTodoExpanded(bool open);

Q_SIGNALS:
    void agentsClicked();
    void goalPause();
    void goalResume();
    void goalClear();
    void stopRequested();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void refreshState();
    void rebuildTodo();
    void restyle();

    ChatActivity a_;
    bool wasRunning_ = false;
    qint64 lastTurnMs_ = 0;
    bool todoOpen_ = true;
    QJsonArray shownTodo_;
    QTimer clock_;
    int todoColumns_ = 1;
    qint64 goalSeenMs_ = 0;  // when the goal's timeUsedSeconds was last reported

    StateGlyph* stateIcon_ = nullptr;
    QLabel* state_ = nullptr;
    StripChip* agents_ = nullptr;
    StripChip* todo_ = nullptr;
    StripChip* goalChip_ = nullptr;
    StripChip* mode_ = nullptr;
    QPushButton* stop_ = nullptr;
    QToolButton* chevron_ = nullptr;
    QFrame* goal_ = nullptr;
    QLabel* goalText_ = nullptr;
    QLabel* goalTime_ = nullptr;
    QPushButton* goalPause_ = nullptr;
    QFrame* todoBox_ = nullptr;
    QGridLayout* todoGrid_ = nullptr;
};

}  // namespace rk::ui
