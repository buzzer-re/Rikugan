#pragma once
// The welcome screen's binary card model, built from the get_binary_info
// text (Host::binaryInfo()) and the list_strings total.
#include <optional>

#include <QMap>
#include <QString>
#include <QStringList>

namespace rk::ui {

struct BinarySummary {
    QString name;
    QStringList chips;

    bool isEmpty() const { return name.isEmpty() && chips.isEmpty(); }
    QString shortName() const;  // file name without its directory
};

QMap<QString, QString> parseInfoLines(const QString& raw);  // lowercase keys, first occurrence wins
std::optional<qint64> parseTotalCount(const QString& raw);    // "... of 1234:" -> 1234
QString formatProcessor(const QString& processor, const QString& bits);
QString formatFileType(const QString& fileType);
QString formatAddress(const QString& value);
BinarySummary buildBinarySummary(const QString& rawInfo, std::optional<qint64> stringCount = std::nullopt);

}  // namespace rk::ui
