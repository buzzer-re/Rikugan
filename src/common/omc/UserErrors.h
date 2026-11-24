#pragma once
// omc's setup errors speak to its command line ("run `omc login`", "--api-key", "see `omc models`"). Inside
// Rikugan the fix is connecting a model (Get Started, or Settings → Provider); this rewrites those messages and
// leaves every other text unchanged.
#include <QString>

namespace rk {

QString userFacingError(const QString& message);
// No model can run yet: no credentials at all, or none for the provider asked for.
bool needsModelSetup(const QString& message);

}  // namespace rk
