#include "common/config/ConnectionTest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QUrl>

#include "common/util/Files.h"
#include "common/util/SystemTool.h"

namespace rk {

namespace {

constexpr int kMaxSeconds = 12;

QString withoutSlash(QString s) {
    while (s.endsWith(QLatin1Char('/'))) s.chop(1);
    return s;
}

// The model names in a model-list reply (OpenAI and Anthropic: data[].id, Ollama: models[].name).
QStringList modelNames(const QByteArray& body) {
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    QStringList out;
    for (const QJsonValue& v : o.value(QStringLiteral("data")).toArray()) out << v.toObject().value(QStringLiteral("id")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("models")).toArray()) out << v.toObject().value(QStringLiteral("name")).toString();
    out.removeAll(QString());
    return out;
}

QString certificateAdvice() {
    return QObject::tr("If the server uses a self-signed certificate or a private CA, choose that certificate under "
                       "Certificate.");
}

}  // namespace

QString connectionTestUrl(const CustomEndpoint& e) {
    const QString base = withoutSlash(e.baseUrl.trimmed());
    if (e.api == QLatin1String("anthropic-messages"))
        return base + (base.endsWith(QLatin1String("/v1")) ? QStringLiteral("/models") : QStringLiteral("/v1/models"));
    if (e.api == QLatin1String("ollama-chat") && !base.endsWith(QLatin1String("/v1"))) return base + QStringLiteral("/api/tags");
    return base + QStringLiteral("/models");
}

ConnectionTestResult testConnection(const CustomEndpoint& e) {
    const QUrl url(connectionTestUrl(e));
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
        return {false, QObject::tr("Enter a base URL such as https://api.example.com/v1.")};
    const QString curl = systemTool("curl");
    if (curl.isEmpty()) return {false, QObject::tr("The test needs curl, which is not installed.")};

    // the key goes in a header file, not on the command line other users can list; one header per LF-ended line
    // (curl keeps a CR as part of the value)
    QTemporaryFile headers;
    if (!headers.open()) return {false, QObject::tr("Cannot write a temporary file for the test.")};
    headers.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray key = e.apiKey.trimmed().toUtf8();
    if (e.api == QLatin1String("anthropic-messages")) {
        headers.write("anthropic-version: 2023-06-01\n");
        if (!key.isEmpty()) headers.write("x-api-key: " + key + "\n");
    } else if (!key.isEmpty()) {
        headers.write("Authorization: Bearer " + key + "\n");
    }
    headers.close();

    QStringList args{QStringLiteral("-sS"), QStringLiteral("-o"), QStringLiteral("-"), QStringLiteral("-w"),
                     QStringLiteral("\n%{http_code}"), QStringLiteral("--max-time"), QString::number(kMaxSeconds),
                     QStringLiteral("--connect-timeout"), QStringLiteral("6"), QStringLiteral("-H"),
                     QLatin1Char('@') + headers.fileName()};
    if (!e.caFile.isEmpty()) {
        const QString ca = expandHome(e.caFile);
        if (!QFile::exists(ca)) return {false, QObject::tr("The certificate file %1 does not exist.").arg(e.caFile)};
        args << QStringLiteral("--cacert") << ca;
    }
#ifdef _WIN32
    args << QStringLiteral("--ssl-no-revoke");  // Schannel cannot check revocation for a private certificate
#endif
    args << url.toString(QUrl::FullyEncoded);

    QProcess p;
    p.start(curl, args);
    if (!p.waitForStarted(10000)) return {false, QObject::tr("Cannot run curl: %1").arg(p.errorString())};
    if (!p.waitForFinished((kMaxSeconds + 5) * 1000)) {
        p.kill();
        p.waitForFinished(3000);
        return {false, QObject::tr("No answer from %1 within %2 seconds.").arg(url.host()).arg(kMaxSeconds)};
    }
    const QByteArray out = p.readAllStandardOutput();
    // curl's first line without its "curl: (60) " prefix; the lines after it are a generic manual
    static const QRegularExpression prefix(QStringLiteral("^curl: (\\(\\d+\\) )?"));
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0)
                            .remove(prefix).trimmed().left(240);
    const int rc = p.exitCode();
    const QString host = url.port() > 0 ? QStringLiteral("%1:%2").arg(url.host()).arg(url.port()) : url.host();
    switch (rc) {
    case 0: break;
    case 6: return {false, QObject::tr("Cannot resolve the host %1. Check the address.").arg(url.host())};
    case 7: return {false, QObject::tr("Cannot connect to %1: nothing answers there, or a firewall blocks it.").arg(host)};
    case 28: return {false, QObject::tr("No answer from %1 within %2 seconds.").arg(host).arg(kMaxSeconds)};
    case 35:
    case 51:
    case 53:
    case 54:
    case 58:
    case 59:
    case 60:
    case 66:
    case 83:
        return {false, QObject::tr("%1 was reached, but its certificate is not trusted (%2). %3").arg(host, err, certificateAdvice())};
    case 77: return {false, QObject::tr("Cannot use the certificate file %1 (%2).").arg(e.caFile, err)};
    default: return {false, QObject::tr("The test failed (curl %1): %2").arg(rc).arg(err)};
    }

    const qsizetype nl = out.lastIndexOf('\n');
    const int status = out.mid(nl + 1).trimmed().toInt();
    const QByteArray body = nl < 0 ? QByteArray() : out.left(nl);
    if (status >= 200 && status < 300) {
        const QStringList names = modelNames(body);
        if (names.isEmpty()) return {true, QObject::tr("Connected: %1 answered (HTTP %2).").arg(host).arg(status)};
        QStringList shown = names.mid(0, 6);
        if (names.size() > shown.size()) shown << QObject::tr("and %n more", nullptr, int(names.size() - shown.size()));
        return {true, QObject::tr("Connected: %1 lists %n model(s): %2.", nullptr, int(names.size())).arg(host, shown.join(QStringLiteral(", ")))};
    }
    if (status == 401 || status == 403)
        return {false, QObject::tr("%1 answered but refused the API key (HTTP %2).").arg(host).arg(status)};
    if (status == 404)
        return {false, QObject::tr("%1 answered, but %2 does not exist (HTTP 404). Check the base URL: OpenAI-compatible "
                                   "servers usually end in /v1.")
                           .arg(host, url.path())};
    return {false, QObject::tr("%1 answered with HTTP %2.").arg(host).arg(status)};
}

}  // namespace rk
