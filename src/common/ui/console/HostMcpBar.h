#pragma once
// The host's own MCP server (Binary Ninja's), in the panel: the line under the header that offers its tools for a
// database or says why they cannot be used, and the state behind the header's MCP button.
//
// The first time a database shows, the server is looked for. With the user's earlier yes for that database its
// tools turn on, with no answer yet the line asks, with a no nothing happens until the button is clicked. Clicking
// the button is consent. The answer is remembered per database (database.json). UI thread.
#include <functional>
#include <memory>

#include <QFrame>
#include <QSet>
#include <QString>

#include "common/agent/HostMcp.h"

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace rk {

struct DbContext;

namespace ui {

class HostMcpBar : public QFrame {
    Q_OBJECT
public:
    explicit HostMcpBar(QWidget* parent = nullptr);

    void setDatabase(const std::shared_ptr<DbContext>& db);
    // The header button: off when on, otherwise look for the server and use it.
    void toggle();

    bool available() const;  // the host has a server and a database shows
    bool on() const;
    bool busy() const { return probing_; }
    QString tooltip() const;

    // Turns the host's tools on or off for `db`: its registry, its live chats and, with `remember`, its answer.
    static void apply(DbContext& db, bool on, bool remember);

Q_SIGNALS:
    void stateChanged();

private:
    void probe(bool asked);
    void probed(const std::weak_ptr<DbContext>& db, bool asked, const HostMcp::Probe& p);
    // the line: a message with up to two actions
    void notify(const QString& text, const QString& yes = {}, std::function<void()> onYes = {}, const QString& no = {},
              std::function<void()> onNo = {});
    void restyle();

    std::weak_ptr<DbContext> db_;
    QSet<QString> looked_;  // databases already looked for at first show
    bool probing_ = false;
    QLabel* text_ = nullptr;
    QPushButton* yes_ = nullptr;
    QPushButton* no_ = nullptr;
    std::function<void()> onYes_, onNo_;
};

}  // namespace ui
}  // namespace rk
