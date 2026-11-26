#pragma once
// Untrusted data from the binary on its way to the model.
// Delimiter wrapping of tool results is gone: omc sends host-tool output as structured tool results, and the
// system prompt's data-integrity section tells the model how to treat it.
#include <QJsonArray>
#include <QMap>
#include <QString>

namespace rk::sanitize {

// Removes zero-width characters, role markers ([SYSTEM], <|im_start|>, ...), the Anthropic magic string and
// instruction-override phrases, with homoglyph-normalised matching.
QString stripInjectionMarkers(const QString& text);
// RIKUGAN.md for the system prompt: markers stripped, capped at 20,000 chars.
QString sanitizeMemory(const QString& text);
// Binary metadata for the system prompt: markers stripped, capped at 2,000 chars, closing tag neutralised, wrapped in
// <contextType>…</contextType>.
QString sanitizeBinaryContext(const QString& text, const QString& contextType = QStringLiteral("binary_data"));
// Escapes text for an XML-like attribute value.
QString escapeAttr(const QString& text);
// IOC redaction for the active profile: `filters` is category -> on, `customRules` the
// profile's [{name, pattern, replacement}].
QString stripIocs(const QString& text, const QMap<QString, bool>& filters, const QJsonArray& customRules);

}  // namespace rk::sanitize
