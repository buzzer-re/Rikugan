#pragma once
// The bottom line: model and thinking, context use, tokens in/out, cost, the turn's time, and the host cursor
// (address and function; a click puts the address in the composer). A 1 s clock runs only while the chat works.
#include <QTimer>
#include <QWidget>

#include "common/ui/transcript/ChatActivity.h"

namespace rk::ui {

class StatusLine : public QWidget {
    Q_OBJECT
public:
    explicit StatusLine(QWidget* parent = nullptr);

    void setActivity(const ChatActivity& a);
    void setModelFallback(const QString& model);  // no chat yet: the configured model
    void setLocation(quint64 address, const QString& function);
    QSize sizeHint() const override;

Q_SIGNALS:
    void locationClicked(const QString& address);
    void modelClicked();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    ChatActivity a_;
    QString fallbackModel_;
    QString address_, function_;
    qint64 lastTurnMs_ = 0;
    bool wasRunning_ = false;
    QRect locationRect_, modelRect_;
    QTimer clock_;
};

}  // namespace rk::ui
