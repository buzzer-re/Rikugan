#pragma once
// The bulk renamer page of the panel (overflow menu, or "Send to bulk renamer" in the host): a back bar and the
// renamer. Lives on the UI thread; follows the panel's current database.
#include <memory>

#include <QList>
#include <QWidget>

namespace rk {

struct DbContext;
class BulkRenamerWidget;

class ToolsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolsPanel(QWidget* parent = nullptr);

    void setDatabase(std::shared_ptr<DbContext> db);
    // Context-menu "Send to bulk renamer": check these function addresses.
    void addToRenamer(const QList<quint64>& functionAddresses);

Q_SIGNALS:
    void backRequested();

private:
    BulkRenamerWidget* renamer_ = nullptr;
};

}  // namespace rk
