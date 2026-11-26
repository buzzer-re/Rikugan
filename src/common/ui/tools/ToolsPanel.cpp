#include "common/ui/tools/ToolsPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/agent/DbContext.h"
#include "common/ui/Theme.h"
#include "common/ui/tools/BulkRenamerWidget.h"

namespace rk {

ToolsPanel::ToolsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_tools"));
    // no minimum size: embedded in IDA dock forms and Binary Ninja sidebars of any size
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QWidget(this);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 3, 6, 3);
    auto* back = new QToolButton(bar);
    back->setText(QStringLiteral("\u2039 ") + tr("Chat"));
    back->setCursor(Qt::PointingHandCursor);
    row->addWidget(back);
    auto* title = new QLabel(tr("Bulk renamer"), bar);
    QFont f = ui::theme().ui;
    f.setWeight(QFont::DemiBold);
    title->setFont(f);
    row->addWidget(title);
    row->addStretch(1);
    layout->addWidget(bar);
    renamer_ = new BulkRenamerWidget(this);
    layout->addWidget(renamer_, 1);
    connect(back, &QToolButton::clicked, this, &ToolsPanel::backRequested);
}

void ToolsPanel::setDatabase(std::shared_ptr<DbContext> db) { renamer_->setDatabase(std::move(db)); }

void ToolsPanel::addToRenamer(const QList<quint64>& functionAddresses) { renamer_->selectAddresses(functionAddresses); }

}  // namespace rk
