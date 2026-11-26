#pragma once
// Asking before Rikugan uses Claude Code's or Codex's subscription login (ExternalLogins.h): using a subscription
// outside its own tool may be against the provider's terms. UI thread.
#include <QList>

#include "common/omc/ExternalLogins.h"

QT_FORWARD_DECLARE_CLASS(QWidget)

namespace rk::ui {

// "Claude Code and Codex", "Claude Code" ... for the logins found.
QString externalToolNames(const QList<ExternalLogin>& found);
// The question, with the providers' policy links; true when the user agrees.
bool confirmExternalLogins(QWidget* parent, const QList<ExternalLogin>& found);

}  // namespace rk::ui
