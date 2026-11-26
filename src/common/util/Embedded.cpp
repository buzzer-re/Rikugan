#include "common/util/Embedded.h"

namespace rk {

QString embeddedText(std::string_view path) {
    for (const EmbeddedFile& f : embeddedFiles())
        if (f.path == path) return QString::fromUtf8(f.data.data(), qsizetype(f.data.size()));
    return {};
}

}  // namespace rk
