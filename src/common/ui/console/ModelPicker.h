#pragma once
// Ctrl+M / the model pill: models grouped by provider (connected providers with their models, the others with
// "Connect"), the thinking level of the chat's model, and "Connect a provider…". The list shown is the last one
// loaded; opening refreshes it in the background and an open picker only updates when something changed.
#include <QJsonArray>
#include <QPointer>

#include "common/omc/LoginFlow.h"
#include "common/ui/console/Palette.h"

QT_FORWARD_DECLARE_CLASS(QButtonGroup)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)

namespace rk::ui {

struct ModelInfo {
    QString provider, id, name;
    qint64 contextWindow = 0;
    bool reasoning = false;
    bool operator==(const ModelInfo&) const = default;
    QString key() const { return provider + QLatin1Char('/') + id; }
};

class ModelPicker : public PalettePopup {
    Q_OBJECT
public:
    explicit ModelPicker(QWidget* area);

    // `current` = provider/id; `levels` = the chat's thinking selectors (get_state thinkingLevels).
    void open(QWidget* anchor, const QString& current, const QString& thinking, const QStringList& levels);
    void refresh();
    // A friendly name for provider/id once the list is known ("Claude Opus 4.5"); the id otherwise.
    QString displayName(const QString& key) const;
    const QList<ModelInfo>& models() const { return models_; }
    bool loaded() const { return loaded_; }          // a list (or an error) came back at least once
    const QString& error() const { return error_; }  // why the last load failed

Q_SIGNALS:
    void modelChosen(const QString& key);
    void thinkingChosen(const QString& level);
    void connectRequested(const QString& providerId);  // empty: the provider list
    void modelsChanged();
    void refreshed();  // a refresh() came back, changed or not

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void rebuild();
    void setLevels(const QStringList& levels, const QString& current);
    void stepThinking(int delta);

    QList<ModelInfo> models_;
    QList<LoginProviderInfo> providers_;
    QString error_;
    bool loaded_ = false;
    QString current_, thinking_;
    QStringList levels_;
    QWidget* thinkingRow_ = nullptr;
    QHBoxLayout* thinkingButtons_ = nullptr;
    QButtonGroup* thinkingGroup_ = nullptr;
    QLabel* thinkingModel_ = nullptr;
    QHash<QString, QString> sectionProvider_;  // section title -> provider id
};

}  // namespace rk::ui
