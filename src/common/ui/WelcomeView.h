#pragma once
// What an empty chat shows: "New chat · <binary>", the binary's facts and starting points. Picking a starting
// point emits activated(prompt) (the panel puts it in the composer). With no database open it lists what can be
// opened instead, and a pick emits openRequested(key).
#include <optional>

#include <QFrame>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "common/ui/BinarySummary.h"

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace rk::ui {

struct Suggestion {
    QString command;  // "/explore" or empty
    QString title;
    QString prompt;   // what goes in the composer (the row's key with no database)
    QString detail;   // muted second line
};

// Skill slugs without "/" (the deobfuscation row needs that skill); the cursor address goes in the /modify row.
QList<Suggestion> buildSuggestions(const QStringList& skillSlugs, std::optional<quint64> cursorAddress);

class SuggestionRow : public QFrame {
    Q_OBJECT
public:
    explicit SuggestionRow(const Suggestion& suggestion, QWidget* parent = nullptr);

Q_SIGNALS:
    void activated(const QString& prompt);

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QString prompt_;
};

class WelcomeView : public QWidget {
    Q_OBJECT
public:
    explicit WelcomeView(QWidget* parent = nullptr);
    void setSuggestions(const QList<Suggestion>& suggestions);
    void setBinarySummary(const BinarySummary& summary);
    // No database: "No binary open", `facts`, and rows that open something.
    void setNoDatabase(const QString& facts, const QList<Suggestion>& rows);

Q_SIGNALS:
    void activated(const QString& prompt);
    void openRequested(const QString& key);

private:
    void restyle();
    QLabel* title_ = nullptr;
    QLabel* facts_ = nullptr;
    QLabel* hint_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
    QList<Suggestion> suggestions_;
    bool noDatabase_ = false;
};

}  // namespace rk::ui
