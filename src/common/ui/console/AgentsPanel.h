#pragma once
// The agents of the current chat (Ctrl+J; slides up under the transcript): a tree of agents (a child under the
// agent that spawned it) with status, current tool, tools and tokens; the selected agent's live log and output,
// Stop and Steer; "+ New agent" (preset, task, perks, max turns). Narrow panels show the tree or the detail.
// Follows SubagentRegistry with one coalesced refresh per burst; a 1 s clock runs only while an agent runs and
// the panel is visible.
#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "common/agent/Subagents.h"

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPlainTextEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSpinBox)
QT_FORWARD_DECLARE_CLASS(QStackedWidget)
QT_FORWARD_DECLARE_CLASS(QTextBrowser)
QT_FORWARD_DECLARE_CLASS(QToolButton)
QT_FORWARD_DECLARE_CLASS(QTreeWidget)
QT_FORWARD_DECLARE_CLASS(QTreeWidgetItem)

namespace rk {
class ChatSession;
}

namespace rk::ui {

class SpawnAgentDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpawnAgentDialog(QWidget* parent = nullptr);
    QString agentType() const;  // custom | network_recon | report_writer
    QString task() const;
    QStringList perks() const;
    int maxTurns() const;

private:
    void onPresetChanged();

    QComboBox* type_ = nullptr;
    QPlainTextEdit* task_ = nullptr;
    QList<QPair<QString, QCheckBox*>> perks_;
    QSpinBox* maxTurns_ = nullptr;
    QPushButton* launch_ = nullptr;
};

class AgentsPanel : public QWidget {
    Q_OBJECT
public:
    explicit AgentsPanel(QWidget* parent = nullptr);
    ~AgentsPanel() override;

    void setChat(ChatSession* chat);
    void showAgent(const QString& id);
    QString currentAgent() const { return current_; }

Q_SIGNALS:
    void closeRequested();
    void navigateRequested(quint64 address);
    // A finished agent's summary for the chat.
    void summaryForChat(const QString& text);
    // The agent's whole conversation, shown in the panel like a chat (double click, or "Open chat").
    void openChatRequested(const QString& agentId);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void refresh();
    void refreshDetail();
    void setLogHtml(const QString& html);
    void applyNarrow();
    void newAgent();
    void restyle();
    QList<SubagentInfo> agents() const;

    QPointer<ChatSession> chat_;
    int observer_ = 0;
    bool dirty_ = false;
    QString current_;
    QHash<QString, QTreeWidgetItem*> items_;
    QHash<QString, SubagentInfo> infos_;
    QTimer clock_;
    bool narrow_ = false;

    QLabel* counts_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* detail_ = nullptr;
    QToolButton* back_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* elapsed_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* openChat_ = nullptr;
    QLabel* task_ = nullptr;
    QLabel* logHead_ = nullptr;
    QTextBrowser* log_ = nullptr;
    QPushButton* toChat_ = nullptr;
    QPushButton* copy_ = nullptr;
    QString logPending_;  // a newer log, held back while the user selects in the old one
    QLineEdit* steer_ = nullptr;
    QPushButton* steerBtn_ = nullptr;
    QPushButton* new_ = nullptr;
};

}  // namespace rk::ui
