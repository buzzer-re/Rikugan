#pragma once
// Files under skills/ and prompts/, compiled into the plugin (cmake/embed_gen.cmake).
#include <span>
#include <string_view>

#include <QString>

namespace rk {

struct EmbeddedFile {
    std::string_view path;  // relative to the repo root, e.g. "prompts/base.md"
    std::string_view data;
};

std::span<const EmbeddedFile> embeddedFiles();

// Contents of an embedded file, or an empty string when there is none.
QString embeddedText(std::string_view path);

}  // namespace rk
