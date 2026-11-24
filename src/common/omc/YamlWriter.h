#pragma once
// Block YAML for omc's config.yml / models.yml: maps, lists, strings (JSON-quoted), bools, numbers.
#include <QByteArray>
#include <QJsonObject>

namespace rk {

QByteArray toYaml(const QJsonObject& root);

}  // namespace rk
