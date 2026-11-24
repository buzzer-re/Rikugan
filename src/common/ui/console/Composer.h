#pragma once
// The message box: a growing editor (Enter sends, Shift+Enter breaks the line, typing while the chat works
// queues), the queued prompts above it, and a bottom row with the model pill, the running mode, key hints and
// Send / Stop. "/" at the start opens the slash palette over the transcript. UI thread only.
#include <QFrame>
#include <QList>
#include <QPlainTextEdit>
#include <QStringList>

#include "common/agent/CommandRouter.h"

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QToolButton)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace rk::ui {

class PalettePopup;

class ComposerEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ComposerEdit(QWidget* parent = nullptr);
    void syncHeight();

Q_SIGNALS:
    void submit();
    // Keys the slash palette may take (Up / Down / Enter / Tab / Esc): handled = true stops the editor.
    void paletteKey(int key, bool* handled);
    void historyKey(int delta, bool* handled);
    // Ctrl+K / Ctrl+M / Ctrl+J / Ctrl+N typed here: the panel's own keys win over the host's shortcuts
    void panelKey(int key);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
};

class Composer : public QFrame {
    Q_OBJECT
public:
    explicit Composer(QWidget* parent = nullptr);

    // Where popups float (the panel); the slash palette is built there once.
    void setPopupArea(QWidget* area);
    void setCommands(const QList<CommandInfo>& commands);
    void setRunning(bool running);
    // Off while only a card's buttons can answer or no database is open.
    void setInputEnabled(bool enabled);
    void setPlaceholder(const QString& text);
    void setModelLabel(const QString& label, const QString& tooltip);
    void setMode(const QString& mode);  // the running mode chip; empty hides it
    void setQueue(const QStringList& queued);

    QString text() const;
    void setText(const QString& text);
    void focusInput();
    QWidget* modelButton() const;
    bool slashOpen() const;

Q_SIGNALS:
    void submitted(const QString& text);
    void stopRequested();
    void modelPickerRequested();
    void removeQueued(int index);
    void sendQueuedNow(int index);
    void panelKey(int key);

private:
    void send();
    void updateSlash();
    void acceptCommand(const QString& key);
    void syncButton();
    void restyle();

    QVBoxLayout* queueBox_ = nullptr;
    QFrame* box_ = nullptr;
    ComposerEdit* edit_ = nullptr;
    QToolButton* model_ = nullptr;
    QToolButton* mode_ = nullptr;
    QLabel* hint_ = nullptr;
    QToolButton* send_ = nullptr;
    PalettePopup* slash_ = nullptr;
    QList<CommandInfo> commands_;
    QStringList history_;
    int historyPos_ = -1;
    bool running_ = false;
    bool enabled_ = true;
};

}  // namespace rk::ui
