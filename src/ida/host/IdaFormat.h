#pragma once
// Python formatting the IDA tools reproduce (f-string format specs, repr, slices), pagination and global-value
// formatting.
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace rk::ida {

// f"{s:Ns}": left-justified in N columns, never truncated.
QString ljust(const QString& s, int width);
// f"{v:Nd}"
QString rjust(qint64 v, int width);
// f"0x{v:0Nx}"
QString hexPad(uint64_t v, int width);
// f"{v:#x}" for a signed value ("-0x8" for -8).
QString altHex(qint64 v);
// f"{v:#010x}"
QString altHex010(uint64_t v);
// repr(str)
QString pyRepr(const QString& s);
// repr(list[int])
QString pyIntList(const std::vector<qint64>& v);
// str.isprintable() for one code point
bool pyIsPrintable(char32_t c);
// bytes.decode("utf-8", "replace")
QString decodeUtf8(const QByteArray& b);

// Python slice bounds of seq[start:stop] for a sequence of `len` items.
std::pair<qsizetype, qsizetype> pySlice(qsizetype len, qint64 start, qint64 stop);

// Pagination
constexpr int kDefaultPageLimit = 80;
constexpr int kMaxPageLimit = 200;
std::pair<qint64, qint64> normalizePage(qint64 offset, qint64 limit);
QString formatPage(const QStringList& rows, qint64 offset, qint64 limit, const QString& title,
                   const QString& empty = QStringLiteral("  (none)"), const QString& nextHint = {});

// Global-value formatting
QString normalizeTypeHint(const QString& typeHint);
int bytesNeededForType(const QString& typeHint, int pointerSize, qint64 requestedSize);
QString formatGlobalValue(uint64_t address, const QByteArray& data, int pointerSize, const QString& typeHint,
                          const QString& name, const std::function<QString(uint64_t)>& resolvePointer);

}  // namespace rk::ida
