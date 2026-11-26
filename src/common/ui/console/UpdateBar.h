#pragma once
// The panel's line about Rikugan's updates (update/Updater.h): a newer release offered (Update, Release notes, Later),
// what an update is doing, that it needs a restart of the host, or why it failed. Every panel shows the same state.
// UI thread.
#include <QFrame>

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace rk::ui {

class UpdateBar : public QFrame {
    Q_OBJECT
public:
    explicit UpdateBar(QWidget* parent = nullptr);

private:
    void sync();
    void restyle();

    QLabel* text_ = nullptr;
    QPushButton* primary_ = nullptr;    // Update
    QPushButton* notes_ = nullptr;      // Release notes
    QPushButton* secondary_ = nullptr;  // Later / Dismiss
};

}  // namespace rk::ui
