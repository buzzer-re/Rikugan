#pragma once
// The transcript's interactive rows: a disk/shell permission, a script or database-change approval, a question,
// an omc dialog (select / input / editor / confirm) and the plan / save decisions of the modes. Each card answers
// the chat itself and then reports how it was resolved; the view turns the row back into a painted line.
// UI thread only.
#include <QFrame>
#include <QPointer>

#include "common/ui/transcript/TranscriptModel.h"

QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace rk {
class ChatSession;
}

namespace rk::ui {

class TranscriptCard : public QFrame {
    Q_OBJECT
public:
    // nullptr for an item kind that has no card or a resolved item.
    static TranscriptCard* create(const TranscriptItem& item, ChatSession* chat, QWidget* parent);

    // Height for `width` (the view lays the row out with it).
    int heightFor(int width) const;
    // Focuses the first answer control (keyboard users).
    void focusFirst();

Q_SIGNALS:
    // The user answered; `resolution` is what the resolved row shows ("Allow once", the chosen option ...).
    void answered(const QString& resolution);
    void heightChanged();

protected:
    TranscriptCard(ChatSession* chat, QWidget* parent);
    bool event(QEvent* e) override;

    QVBoxLayout* body() const { return body_; }
    QHBoxLayout* buttonRow();
    QPushButton* addButton(const QString& text, bool primary = false, bool danger = false);
    void addText(const QString& text, bool markdown = false, bool muted = false);
    void addCode(const QString& code, const QString& language, int maxLines = 14);
    void finish(const QString& resolution);  // disables the card and emits answered once

    QPointer<ChatSession> chat_;

private:
    void restyle();
    QVBoxLayout* body_ = nullptr;
    QHBoxLayout* buttons_ = nullptr;
    bool done_ = false;
};

}  // namespace rk::ui
