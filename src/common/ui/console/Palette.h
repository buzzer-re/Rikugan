#pragma once
// The panel's pickers: a fuzzy-filtered list with sections (slash commands, the Ctrl+K command palette, the model
// picker, the login provider list). PaletteList is the list; PalettePopup frames it with an optional search field
// and floats over the panel as a child widget (no top-level window: host docks keep their focus). Both are built
// once and only filtered, shown and hidden. UI thread only.
#include <functional>

#include <QFrame>
#include <QList>
#include <QListView>
#include <QPointer>

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace rk::ui {

struct PaletteEntry {
    QString key;         // what activation reports
    QString section;     // heading the entry sits under ("" = none)
    QString glyph;       // one character before the title
    QString title;       // matched by the query
    QString detail;      // dim text after the title
    QString hint;        // right-aligned (shortcut, context size, source)
    QString search;      // more text the query matches (ids)
    QString sectionNote; // on the first entry of a section: dim text beside the heading ("● ready")
    QString sectionAction;  // on the first entry of a section: a link at the heading's right ("Connect")
    bool checked = false;
    bool enabled = true;
    bool mono = false;   // title in the monospace font (commands)
};

class PaletteModel;

class PaletteList : public QListView {
    Q_OBJECT
public:
    explicit PaletteList(QWidget* parent = nullptr);

    void setEntries(const QList<PaletteEntry>& entries);
    const QList<PaletteEntry>& entries() const;
    // Filters and ranks in place (sections keep their order; entries rank by fuzzy score within them).
    void setQuery(const QString& query);
    QString query() const;
    int visibleCount() const;  // entries shown (headings not counted)

    void moveSelection(int delta);  // skips headings and disabled entries
    QString currentKey() const;
    bool activateCurrent();         // emits activated; false when nothing is selected
    // Rows that fit before the list scrolls.
    void setMaxRows(int rows);

Q_SIGNALS:
    void activated(const QString& key);
    void sectionActionActivated(const QString& section);

protected:
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    QSize sizeHint() const override;

private:
    void selectFirst();
    PaletteModel* model_ = nullptr;
    int maxRows_ = 10;
};

class PalettePopup : public QFrame {
    Q_OBJECT
public:
    // `search`: with its own search field (Ctrl+K, model picker); without, the caller drives the query (slash).
    PalettePopup(QWidget* parent, bool search, const QString& placeholder = {});

    PaletteList* list() const { return list_; }
    QLineEdit* searchField() const { return search_; }
    // Widgets between the search field and the list (model picker: thinking control) and below it (footer).
    void setHeaderWidget(QWidget* w);
    void setFooterWidget(QWidget* w);
    // Shows the popup over `anchor`'s parent area: above `anchor` (composer) or below it (header button), as wide as
    // `width` (0: the parent's width minus margins).
    void popupAt(QWidget* anchor, bool above, int width = 0);
    void dismiss();
    // Keys from the composer while the popup has no search field: true when handled.
    bool handleKey(int key);

Q_SIGNALS:
    void dismissed();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void restyle();
    void place();
    QVBoxLayout* layout_ = nullptr;
    QLineEdit* search_ = nullptr;
    PaletteList* list_ = nullptr;
    QPointer<QWidget> header_, footer_, anchor_;
    bool above_ = true;
    int width_ = 0;
};

}  // namespace rk::ui
