#pragma once
// The panel's top line: the chat title (opens the chat switcher), the host's MCP server switch (Binary Ninja), the
// command palette, a new chat and the overflow menu (fork, rename, export, delete, undo, bulk renamer, agents,
// settings).
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QAction)
QT_FORWARD_DECLARE_CLASS(QMenu)
QT_FORWARD_DECLARE_CLASS(QToolButton)

namespace rk::ui {

class ConsoleHeader : public QWidget {
    Q_OBJECT
public:
    explicit ConsoleHeader(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setChatActionsEnabled(bool enabled);
    void setUndoAvailable(bool available);
    // The host MCP switch: shown only on a host with an MCP server.
    void setHostMcp(bool shown, bool on, bool busy, const QString& tooltip);
    QWidget* titleButton() const;

Q_SIGNALS:
    void switcherRequested();
    void paletteRequested();
    void newChatRequested();
    void forkRequested();
    void renameRequested();
    void exportRequested();      // Markdown, chosen file
    void exportHtmlRequested();  // omc's HTML export next to the database
    void deleteRequested();
    void undoRequested();
    void agentsRequested();
    void renamerRequested();
    void settingsRequested();
    void hostMcpClicked();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void restyle();
    // a narrow header (Binary Ninja's sidebar) keeps the title: the palette and MCP buttons drop their words first
    void fit();
    QToolButton* title_ = nullptr;
    QToolButton* mcp_ = nullptr;
    bool mcpOn_ = false;
    bool mcpBusy_ = false;
    QToolButton* palette_ = nullptr;
    QToolButton* add_ = nullptr;
    QToolButton* more_ = nullptr;
    QMenu* menu_ = nullptr;
    QList<QAction*> chatActions_;
    QAction* undo_ = nullptr;
};

}  // namespace rk::ui
