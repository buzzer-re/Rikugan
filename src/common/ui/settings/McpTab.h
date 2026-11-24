#pragma once
// "MCP": the servers in Rikugan's omc
// <agent dir>/mcp.json — add, edit, remove, enable. Keys this tab doesn't know are kept as they are.
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace rk {

class McpTab : public QWidget {
    Q_OBJECT
public:
    explicit McpTab(QWidget* parent = nullptr);
    // Writes mcp.json when something changed. Empty on success, else the error.
    QString save();

private:
    void reload();
    void fillList();
    void editServer(const QString& name);  // empty: new server
    void removeServer();

    QString path_;
    QJsonObject file_;     // the whole file as read
    QJsonObject servers_;  // working copy of mcpServers
    QStringList disabledList_;  // working copy of disabledServers
    bool readable_ = true;
    bool dirty_ = false;

    QListWidget* list_ = nullptr;
    QLabel* empty_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* remove_ = nullptr;
};

}  // namespace rk
