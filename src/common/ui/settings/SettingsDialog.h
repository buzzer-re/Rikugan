#pragma once
// Settings: Provider, Behavior, Skills, MCP, Profiles. Edits RikuganConfig and Rikugan's agent dir (auth.json,
// models.yml, mcp.json, skills).
#include <memory>

#include <QDialog>

namespace rk {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override;

Q_SIGNALS:
    // After OK: the config was saved. Changes that need a new omc session apply to the next one.
    void settingsChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace rk
