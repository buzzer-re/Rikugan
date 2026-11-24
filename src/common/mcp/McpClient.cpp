#include "common/mcp/McpClient.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QJsonDocument>
#include <QUrl>

namespace rk {

namespace {

constexpr qsizetype kMaxReply = 64 * 1024 * 1024;
const char* const kProtocolVersion = "2025-06-18";

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString sysError() { return QString::fromLocal8Bit(std::strerror(errno)); }

class Socket {
public:
    ~Socket() {
        if (fd_ >= 0) ::close(fd_);
    }
    int fd() const { return fd_; }
    void reset(int fd) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Waits until `fd` is ready for `events`, in short slices so a cancelled call ends promptly.
void waitFor(int fd, short events, const CancelToken* cancel, qint64 deadline) {
    for (;;) {
        if (cancel && cancel->cancelled()) throw ToolError(QStringLiteral("Cancelled"));
        const qint64 left = deadline - nowMs();
        if (left <= 0) throw ToolError(QStringLiteral("the MCP server did not answer in time"));
        pollfd p{fd, events, 0};
        const int r = ::poll(&p, 1, int(std::min<qint64>(left, 100)));
        if (r > 0) return;
        if (r < 0 && errno != EINTR) throw ToolError(sysError());
    }
}

void connectTo(Socket& sock, const McpEndpoint& e, const CancelToken* cancel, qint64 deadline) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const QByteArray host = e.host.toUtf8(), port = QByteArray::number(e.port);
    if (const int rc = ::getaddrinfo(host.constData(), port.constData(), &hints, &list); rc != 0)
        throw ToolError(QStringLiteral("cannot resolve %1: %2").arg(e.host, QString::fromLocal8Bit(gai_strerror(rc))));
    QString failure;
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
        sock.reset(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (sock.fd() < 0) {
            failure = sysError();
            continue;
        }
        ::fcntl(sock.fd(), F_SETFD, FD_CLOEXEC);
        ::fcntl(sock.fd(), F_SETFL, ::fcntl(sock.fd(), F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(sock.fd(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (::connect(sock.fd(), ai->ai_addr, ai->ai_addrlen) == 0) break;
        if (errno != EINPROGRESS) {
            failure = sysError();
            sock.reset(-1);
            continue;
        }
        try {
            waitFor(sock.fd(), POLLOUT, cancel, deadline);
        } catch (...) {
            ::freeaddrinfo(list);
            throw;
        }
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(sock.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
        if (err == 0) break;
        failure = QString::fromLocal8Bit(std::strerror(err));
        sock.reset(-1);
    }
    ::freeaddrinfo(list);
    if (sock.fd() < 0) throw ToolError(QStringLiteral("cannot connect to %1: %2").arg(e.url(), failure));
}

void sendAll(int fd, const QByteArray& data, const CancelToken* cancel, qint64 deadline) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    qsizetype off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.constData() + off, size_t(data.size() - off), flags);
        if (n > 0) {
            off += n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            waitFor(fd, POLLOUT, cancel, deadline);
        } else if (n < 0 && errno != EINTR) {
            throw ToolError(QStringLiteral("cannot send to the MCP server: %1").arg(sysError()));
        }
    }
}

// Decodes a chunked body. True once the last chunk arrived.
bool dechunk(const QByteArray& in, QByteArray* out) {
    out->clear();
    qsizetype pos = 0;
    for (;;) {
        const qsizetype eol = in.indexOf("\r\n", pos);
        if (eol < 0) return false;
        bool ok = false;
        const qsizetype size = in.mid(pos, eol - pos).split(';').first().trimmed().toLongLong(&ok, 16);
        if (!ok) throw ToolError(QStringLiteral("the MCP server sent a malformed chunked reply"));
        if (size == 0) return true;
        if (in.size() < eol + 2 + size + 2) return false;
        out->append(in.mid(eol + 2, size));
        pos = eol + 2 + size + 2;
    }
}

// The JSON-RPC messages of an event stream (each event's data lines joined).
QList<QJsonObject> sseMessages(const QByteArray& body) {
    QList<QJsonObject> out;
    QByteArray text = body;
    text.replace("\r\n", "\n");
    QList<QByteArray> events;
    for (qsizetype pos = 0; pos < text.size();) {
        qsizetype end = text.indexOf("\n\n", pos);
        if (end < 0) end = text.size();
        events << text.mid(pos, end - pos);
        pos = end + 2;
    }
    for (const QByteArray& event : events) {
        QByteArray data;
        for (const QByteArray& line : event.split('\n')) {
            if (!line.startsWith("data:")) continue;
            QByteArray v = line.mid(5);
            if (v.startsWith(' ')) v.remove(0, 1);
            if (!data.isEmpty()) data += '\n';
            data += v;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) out << doc.object();
    }
    return out;
}

// The reply to request `id` among the messages a server sent (a single object, a batch or a stream).
std::optional<QJsonObject> findReply(const QList<QJsonObject>& messages, qint64 id) {
    for (const QJsonObject& m : messages)
        if (m.value(QStringLiteral("id")).toInteger(-1) == id &&
            (m.contains(QStringLiteral("result")) || m.contains(QStringLiteral("error"))))
            return m;
    return std::nullopt;
}

QList<QJsonObject> jsonMessages(const QByteArray& body) {
    QList<QJsonObject> out;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) out << doc.object();
    else
        for (const QJsonValue& v : doc.array()) out << v.toObject();
    return out;
}

}  // namespace

std::optional<McpEndpoint> McpEndpoint::parse(const QString& url, const QString& token, QString* error) {
    const QUrl u(url.trimmed(), QUrl::StrictMode);
    if (!u.isValid() || u.scheme() != QLatin1String("http") || u.host().isEmpty()) {
        if (error) *error = QStringLiteral("not an http:// address: %1").arg(url);
        return std::nullopt;
    }
    McpEndpoint e;
    e.host = u.host();
    e.port = quint16(u.port(80));
    e.path = u.path(QUrl::FullyEncoded).isEmpty() ? QStringLiteral("/") : u.path(QUrl::FullyEncoded);
    if (u.hasQuery()) e.path += QLatin1Char('?') + u.query(QUrl::FullyEncoded);
    e.token = token;
    return e;
}

QString McpEndpoint::url() const {
    const QString h = host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
    return QStringLiteral("http://%1:%2%3").arg(h).arg(port).arg(path);
}

McpClient::Reply McpClient::post(const QByteArray& json, qint64 id, const CancelToken* cancel, qint64 deadline) const {
    Socket sock;
    connectTo(sock, endpoint_, cancel, deadline);
    QByteArray req = "POST " + endpoint_.path.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + endpoint_.host.toUtf8() + ':' + QByteArray::number(endpoint_.port) + "\r\n";
    req += "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\nConnection: close\r\n";
    req += "Content-Length: " + QByteArray::number(json.size()) + "\r\n";
    if (!endpoint_.token.isEmpty()) req += "Authorization: Bearer " + endpoint_.token.toUtf8() + "\r\n";
    if (!sessionId_.isEmpty()) req += "Mcp-Session-Id: " + sessionId_ + "\r\n";
    if (!version_.isEmpty()) req += "MCP-Protocol-Version: " + version_ + "\r\n";
    req += "\r\n" + json;
    sendAll(sock.fd(), req, cancel, deadline);

    Reply r;
    QByteArray raw;
    qsizetype headerEnd = -1;
    qsizetype contentLength = -1;
    bool chunked = false, stream = false;
    char buf[16384];
    for (;;) {
        // complete yet?
        if (headerEnd >= 0) {
            const QByteArray rest = raw.mid(headerEnd);
            if (chunked) {
                if (dechunk(rest, &r.body) || (stream && id >= 0 && findReply(sseMessages(r.body), id))) break;
            } else {
                r.body = rest;
                if (contentLength >= 0 && r.body.size() >= contentLength) {
                    r.body.truncate(contentLength);
                    break;
                }
                if (stream && id >= 0 && findReply(sseMessages(r.body), id)) break;
            }
        }
        waitFor(sock.fd(), POLLIN, cancel, deadline);
        const ssize_t n = ::recv(sock.fd(), buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw ToolError(QStringLiteral("cannot read from the MCP server: %1").arg(sysError()));
        }
        if (n == 0) {  // closed: whatever arrived is the reply
            if (headerEnd < 0) throw ToolError(QStringLiteral("the MCP server closed the connection without replying"));
            if (chunked) dechunk(raw.mid(headerEnd), &r.body);
            else r.body = raw.mid(headerEnd);
            break;
        }
        raw.append(buf, n);
        if (raw.size() > kMaxReply) throw ToolError(QStringLiteral("the MCP server's reply is too large"));
        if (headerEnd < 0) {
            const qsizetype end = raw.indexOf("\r\n\r\n");
            if (end < 0) continue;
            headerEnd = end + 4;
            const QList<QByteArray> lines = raw.left(end).split('\n');
            const QList<QByteArray> status = lines.first().trimmed().split(' ');
            r.status = status.size() > 1 ? status[1].toInt() : 0;
            for (qsizetype i = 1; i < lines.size(); ++i) {
                const qsizetype colon = lines[i].indexOf(':');
                if (colon < 0) continue;
                const QByteArray key = lines[i].left(colon).trimmed().toLower();
                const QByteArray value = lines[i].mid(colon + 1).trimmed();
                if (key == "content-length") contentLength = value.toLongLong();
                else if (key == "transfer-encoding") chunked = value.toLower().contains("chunked");
                else if (key == "content-type") r.contentType = value.toLower();
                else if (key == "mcp-session-id") r.sessionId = value;
            }
            stream = r.contentType.startsWith("text/event-stream");
        }
    }
    if (r.status == 0) throw ToolError(QStringLiteral("the MCP server sent a malformed reply"));
    return r;
}

QJsonObject McpClient::send(const QString& method, const QJsonObject& params, const CancelToken* cancel,
                            qint64 deadline, int* status) {
    const bool notification = method.startsWith(QLatin1String("notifications/"));
    const qint64 id = notification ? -1 : nextId_++;
    QJsonObject msg{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isEmpty()) msg.insert(QStringLiteral("params"), params);
    if (!notification) msg.insert(QStringLiteral("id"), id);
    const Reply r = post(QJsonDocument(msg).toJson(QJsonDocument::Compact), id, cancel, deadline);
    *status = r.status;
    if (!r.sessionId.isEmpty()) sessionId_ = r.sessionId;
    if (r.status == 401 || r.status == 403)
        throw ToolError(QStringLiteral("the MCP server at %1 refused the request (HTTP %2): check its token")
                            .arg(endpoint_.url())
                            .arg(r.status));
    if (r.status < 200 || r.status >= 300)
        throw ToolError(QStringLiteral("the MCP server answered HTTP %1: %2")
                            .arg(r.status)
                            .arg(QString::fromUtf8(r.body.left(300)).trimmed()));
    if (notification) return {};
    const QList<QJsonObject> messages = r.contentType.startsWith("text/event-stream") ? sseMessages(r.body)
                                                                                      : jsonMessages(r.body);
    const std::optional<QJsonObject> reply = findReply(messages, id);
    if (!reply) throw ToolError(QStringLiteral("the MCP server's reply to %1 is not JSON-RPC").arg(method));
    if (reply->contains(QStringLiteral("error"))) {
        const QJsonObject e = reply->value(QStringLiteral("error")).toObject();
        throw ToolError(QStringLiteral("MCP error %1: %2")
                            .arg(e.value(QStringLiteral("code")).toInteger())
                            .arg(e.value(QStringLiteral("message")).toString()));
    }
    return reply->value(QStringLiteral("result")).toObject();
}

QJsonObject McpClient::initialize(const CancelToken* cancel, int timeoutMs) {
    const qint64 deadline = nowMs() + timeoutMs;
    initialized_ = false;
    sessionId_.clear();
    version_.clear();
    int status = 0;
    const QJsonObject result = send(
        QStringLiteral("initialize"),
        QJsonObject{{"protocolVersion", kProtocolVersion},
                    {"capabilities", QJsonObject()},
                    {"clientInfo", QJsonObject{{"name", "rikugan"}, {"version", QStringLiteral(RIKUGAN_VERSION)}}}},
        cancel, deadline, &status);
    // later requests carry the version the server chose
    version_ = result.value(QStringLiteral("protocolVersion")).toString(QString::fromLatin1(kProtocolVersion)).toUtf8();
    send(QStringLiteral("notifications/initialized"), {}, cancel, deadline, &status);
    initialized_ = true;
    return result;
}

QJsonObject McpClient::request(const QString& method, const QJsonObject& params, const CancelToken* cancel,
                               int timeoutMs) {
    if (!initialized_) initialize(cancel, timeoutMs);
    const qint64 deadline = nowMs() + timeoutMs;
    int status = 0;
    try {
        return send(method, params, cancel, deadline, &status);
    } catch (const ToolError&) {
        // a restarted server no longer knows our session: 404 by the spec, 400 from Binary Ninja's
        if (status != 404 && status != 400) throw;
    }
    initialize(cancel, timeoutMs);
    return send(method, params, cancel, nowMs() + timeoutMs, &status);
}

QJsonArray McpClient::listTools(const CancelToken* cancel, int timeoutMs) {
    QJsonArray tools;
    QString cursor;
    do {
        QJsonObject params;
        if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
        const QJsonObject page = request(QStringLiteral("tools/list"), params, cancel, timeoutMs);
        for (const QJsonValue& t : page.value(QStringLiteral("tools")).toArray()) tools.append(t);
        cursor = page.value(QStringLiteral("nextCursor")).toString();
    } while (!cursor.isEmpty());
    return tools;
}

QJsonObject McpClient::callTool(const QString& name, const QJsonObject& arguments, const CancelToken* cancel,
                                int timeoutMs) {
    return request(QStringLiteral("tools/call"), QJsonObject{{"name", name}, {"arguments", arguments}}, cancel,
                   timeoutMs);
}

QString McpClient::resultText(const QJsonObject& result) {
    QStringList parts;
    for (const QJsonValue& v : result.value(QStringLiteral("content")).toArray()) {
        const QJsonObject block = v.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text")) parts << block.value(QStringLiteral("text")).toString();
        else if (type == QLatin1String("resource"))
            parts << block.value(QStringLiteral("resource")).toObject().value(QStringLiteral("text")).toString();
        else if (!type.isEmpty()) parts << QStringLiteral("[%1 content]").arg(type);
    }
    if (parts.isEmpty() && result.contains(QStringLiteral("structuredContent")))
        return QString::fromUtf8(
            QJsonDocument(result.value(QStringLiteral("structuredContent")).toObject()).toJson(QJsonDocument::Compact));
    return parts.join(QLatin1Char('\n'));
}

}  // namespace rk
