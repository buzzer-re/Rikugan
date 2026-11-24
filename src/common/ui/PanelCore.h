#pragma once
// The Rikugan panel: header (chat switcher, command palette, new chat, overflow), activity strip, transcript,
// agents panel, composer and status line, with the bulk renamer page, the login sheet and the pickers floating
// over it. Hosts embed one PanelCore (IDA dock widget, BN sidebar widget).
#include <cstdint>
#include <functional>
#include <memory>

#include <QList>
#include <QString>
#include <QWidget>

namespace rk {

struct DbContext;

class PanelCore : public QWidget {
    Q_OBJECT
public:
    explicit PanelCore(QWidget* parent = nullptr);
    ~PanelCore() override;

    // The database shown (nullptr: no database open). Restores its chats (config.restore_sessions_on_start).
    void setDatabase(std::shared_ptr<DbContext> db);
    std::shared_ptr<DbContext> database() const;
    // Opens a file in the host: what the panel offers while no database is open ("Open a binary…" and the host's
    // recent chats). `dbId` is set when continuing a chat of that database: a tab that already shows it is
    // focused instead. False when nothing opened. The database's arrival goes through setDatabase as usual.
    using FileOpener = std::function<bool(const QString& file, const QString& dbId)>;
    void setFileOpener(const QString& hostName, FileOpener opener);
    // Cursor moved in the host (UI thread): status line and suggestions.
    void setLocation(uint64_t address, const QString& functionName);
    // Context-menu actions.
    void submitToCurrentChat(const QString& text);   // sends it (opens a chat when none)
    void prefillComposer(const QString& text);       // puts it in the composer and focuses it
    void addToBulkRenamer(const QList<quint64>& functionAddresses);
    void showTools();
    void openSettings();
    // The header's full-window button runs `open` (Binary Ninja's sidebar and panes); unset: no button.
    void setFullWindowOpener(std::function<void()> open);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;  // host theme switches

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace rk
