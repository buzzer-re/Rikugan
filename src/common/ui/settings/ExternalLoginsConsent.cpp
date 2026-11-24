#include "common/ui/settings/ExternalLoginsConsent.h"

#include <QMessageBox>
#include <QPushButton>
#include <QStringList>

namespace rk::ui {

QString externalToolNames(const QList<ExternalLogin>& found) {
    QStringList names;
    for (const ExternalLogin& l : found)
        if (!names.contains(l.tool)) names << l.tool;
    return names.join(QObject::tr(" and "));
}

bool confirmExternalLogins(QWidget* parent, const QList<ExternalLogin>& found) {
    QStringList policies;
    for (const ExternalLogin& l : found) {
        if (l.provider == QLatin1String("anthropic"))
            policies << QStringLiteral("<a href=\"https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use\">"
                                       "Anthropic's policy</a>");
        if (l.provider == QLatin1String("openai-codex"))
            policies << QStringLiteral("<a href=\"https://openai.com/policies/terms-of-use/\">OpenAI's terms</a>");
    }
    QMessageBox box(parent);
    box.setWindowTitle(QObject::tr("Use your %1 login").arg(externalToolNames(found)));
    box.setTextFormat(Qt::RichText);
    box.setText(QObject::tr("<b>Use the subscription you are signed in to in %1?</b><br><br>"
                            "Rikugan would share that login: no separate sign-in, and when one side refreshes it the "
                            "other gets the new tokens. Using a subscription outside its own tool may be against the "
                            "provider's terms and carries risk to your account. Read %2 first.<br><br>"
                            "You can turn this off in Settings \u2192 Provider.")
                    .arg(externalToolNames(found).toHtmlEscaped(), policies.join(QObject::tr(" and "))));
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    QPushButton* yes = box.addButton(QObject::tr("Use it"), QMessageBox::AcceptRole);
    box.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == static_cast<QAbstractButton*>(yes);
}

}  // namespace rk::ui
