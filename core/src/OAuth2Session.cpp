#include "OAuth2Session.h"
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif
#endif

namespace QtLLM {

namespace {

QByteArray percentEncoded(const QString& value)
{
    return QUrl::toPercentEncoding(value);
}

QByteArray formEncoded(const QList<QPair<QString, QString>>& fields)
{
    QByteArray out;
    for (const auto& field : fields) {
        if (!out.isEmpty())
            out += '&';
        out += percentEncoded(field.first) + '=' + percentEncoded(field.second);
    }
    return out;
}

// Payload of a JWT, without checking the signature (see the class comment).
QJsonObject jwtPayload(const QString& jwt)
{
    const QStringList parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2)
        return {};
    const QByteArray json = QByteArray::fromBase64(
        parts[1].toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QJsonDocument::fromJson(json).object();
}

QByteArray htmlPage(const QString& title, const QString& text)
{
    return QStringLiteral(
               "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%1</title></head>"
               "<body style=\"font-family:sans-serif;margin:3em\"><h2>%1</h2><p>%2</p></body></html>")
        .arg(title.toHtmlEscaped(), text.toHtmlEscaped())
        .toUtf8();
}

void sendHttp(QTcpSocket* socket, int status, const QByteArray& reason, const QByteArray& body)
{
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n"
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n"
                          "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    socket->write(response);
    socket->disconnectFromHost();   // after the pending data is written
}

// The token store is encrypted for the current Windows user. Elsewhere there
// is no comparable per-user store in reach, so the file stays plain and owner-only.
const QByteArray kStoreMagic = QByteArrayLiteral("QTLLM-OAUTH2-1") + '\n';

#ifdef Q_OS_WIN
const char kDpapiEntropy[] = "QtLLM.OAuth2Session";

QByteArray protect(const QByteArray& plain)
{
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB entropy{DWORD(sizeof(kDpapiEntropy)), reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"QtLLM OAuth2 session", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    const QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return result;
}

QByteArray unprotect(const QByteArray& sealed)
{
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.constData()))};
    DATA_BLOB entropy{DWORD(sizeof(kDpapiEntropy)), reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    const QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}
#else
QByteArray protect(const QByteArray& plain)    { return plain; }
QByteArray unprotect(const QByteArray& sealed) { return sealed; }
#endif

} // namespace

OAuth2Session::OAuth2Session(const OAuth2Config& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_nam(new QNetworkAccessManager(this))
{
    m_signInTimer.setSingleShot(true);
    connect(&m_signInTimer, &QTimer::timeout, this, [this]() {
        failSignIn(QStringLiteral("The sign-in was not completed in time."));
    });
    // Polled rather than one long single-shot timer: after the computer slept
    // a single-shot timer fires late, the poll catches up within one interval.
    m_tickTimer.setInterval(15000);
    connect(&m_tickTimer, &QTimer::timeout, this, &OAuth2Session::onTick);
    m_tickTimer.start();
}

OAuth2Session::~OAuth2Session()
{
    ++m_flowId;
    for (const QPointer<QNetworkReply>& reply : m_replies) {
        if (reply)
            reply->abort();
    }
}

void OAuth2Session::setBrowserOpener(std::function<void(const QUrl&)> opener)
{
    m_browserOpener = std::move(opener);
}

AccessTokenProvider OAuth2Session::accessTokenProvider() const
{
    QPointer<const OAuth2Session> self(this);
    return [self]() { return self ? self->accessToken() : QString(); };
}

QString OAuth2Session::accessToken() const
{
    if (m_accessToken.isEmpty())
        return {};
    if (m_expiresAt.isValid() && QDateTime::currentDateTimeUtc() >= m_expiresAt)
        return {};
    return m_accessToken;
}

bool OAuth2Session::isSignedIn() const
{
    return !accessToken().isEmpty();
}

QString OAuth2Session::displayName() const
{
    const QString name = m_claims["name"].toString();
    if (!name.isEmpty())
        return name;
    const QString given  = m_claims["given_name"].toString();
    const QString family = m_claims["family_name"].toString();
    if (!given.isEmpty() || !family.isEmpty())
        return (given + QLatin1Char(' ') + family).trimmed();
    const QString user = m_claims["preferred_username"].toString();
    if (!user.isEmpty())
        return user;
    const QString mail = email();
    if (!mail.isEmpty())
        return mail;
    return m_claims["sub"].toString();
}

QString OAuth2Session::email() const
{
    const QString mail = m_claims["email"].toString();
    if (!mail.isEmpty())
        return mail;
    // Some providers put the address somewhere else.
    for (const char* key : {"preferred_username", "website", "upn"}) {
        const QString value = m_claims[QLatin1String(key)].toString();
        if (value.contains(QLatin1Char('@')) && !value.contains(QLatin1Char('/')))
            return value;
    }
    return {};
}

QStringList OAuth2Session::groups() const
{
    const QJsonValue value = m_claims["groups"];
    QStringList out;
    if (value.isArray()) {
        for (const QJsonValue& group : value.toArray())
            out.append(group.toString());
    } else if (value.isString()) {
        out.append(value.toString());
    }
    out.removeAll(QString());
    return out;
}

void OAuth2Session::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

QByteArray OAuth2Session::randomUrlSafe(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    for (int i = 0; i < bytes; ++i)
        raw[i] = char(QRandomGenerator::system()->bounded(256));
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

void OAuth2Session::abortFlow()
{
    ++m_flowId;   // first: abort() below delivers finished() synchronously
    m_signInTimer.stop();
    for (QTcpServer* server : m_servers) {
        server->close();
        server->deleteLater();
    }
    m_servers.clear();
    const QList<QPointer<QNetworkReply>> replies = m_replies;
    m_replies.clear();
    for (const QPointer<QNetworkReply>& reply : replies) {
        if (reply)
            reply->abort();
    }
    m_refreshing = false;
    m_pkceVerifier.clear();
    m_stateParam.clear();
}

void OAuth2Session::failSignIn(const QString& error)
{
    abortFlow();
    setState(isSignedIn() ? State::SignedIn : State::SignedOut);
    emit signInFailed(error);
}

void OAuth2Session::signIn()
{
    abortFlow();
    if (m_config.clientId.isEmpty() || !m_config.redirectUri.isValid()) {
        emit signInFailed(QStringLiteral("OAuth2Config needs a clientId and a redirectUri."));
        return;
    }
    setState(State::SigningIn);
    if (m_config.signInTimeoutMs > 0)
        m_signInTimer.start(m_config.signInTimeoutMs);
    if (loadStore()) {
        withEndpoints([this]() {
            if (isSignedIn())
                finishSignIn();
            else
                refreshStoredTokens();
        });
        return;
    }
    withEndpoints([this]() { startBrowserFlow(); });
}

void OAuth2Session::refreshStoredTokens()
{
    postForm(m_config.tokenEndpoint,
             {{QStringLiteral("grant_type"),    QStringLiteral("refresh_token")},
              {QStringLiteral("refresh_token"), m_refreshToken},
              {QStringLiteral("client_id"),     m_config.clientId}},
             [this](const QJsonObject& json, int, const QString& error) {
                 QString message = error;
                 if (message.isEmpty() && acceptTokens(json, &message)) {
                     finishSignIn();
                     return;
                 }
                 // Stored session is over: forget it and sign in in the browser.
                 m_accessToken.clear();
                 m_refreshToken.clear();
                 m_idToken.clear();
                 m_expiresAt = QDateTime();
                 m_claims = QJsonObject();
                 clearStore();
                 startBrowserFlow();
             });
}

void OAuth2Session::setTokenStorePath(const QString& path)
{
    m_tokenStorePath = path;
}

bool OAuth2Session::loadStore()
{
    if (m_tokenStorePath.isEmpty())
        return false;
    QFile file(m_tokenStorePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QByteArray data = file.readAll();
    if (!data.startsWith(kStoreMagic))
        return false;
    data = unprotect(data.mid(kStoreMagic.size()));
    const QJsonObject store = QJsonDocument::fromJson(data).object();
    if (store["issuer"].toString() != m_config.issuer.toString()
        || store["client_id"].toString() != m_config.clientId)
        return false;

    const QString refresh = store["refresh_token"].toString();
    const QString access  = store["access_token"].toString();
    const QDateTime expires = QDateTime::fromString(store["expires_at"].toString(), Qt::ISODate);
    const bool accessValid = !access.isEmpty()
        && (!expires.isValid() || QDateTime::currentDateTimeUtc() < expires);
    if (refresh.isEmpty() && !accessValid)
        return false;

    m_accessToken  = access;
    m_refreshToken = refresh;
    m_idToken      = store["id_token"].toString();
    m_expiresAt    = expires;
    m_claims       = store["claims"].toObject();
    return true;
}

void OAuth2Session::saveStore() const
{
    if (m_tokenStorePath.isEmpty())
        return;
    const QJsonObject store{
        {"issuer",        m_config.issuer.toString()},
        {"client_id",     m_config.clientId},
        {"access_token",  m_accessToken},
        {"refresh_token", m_refreshToken},
        {"id_token",      m_idToken},
        {"expires_at",    m_expiresAt.isValid() ? m_expiresAt.toUTC().toString(Qt::ISODate) : QString()},
        {"claims",        m_claims}};
    const QByteArray sealed = protect(QJsonDocument(store).toJson(QJsonDocument::Compact));
    if (sealed.isEmpty()) {
        qWarning() << "OAuth2Session: could not encrypt the token store; not saved";
        return;
    }
    QDir().mkpath(QFileInfo(m_tokenStorePath).absolutePath());
    QSaveFile file(m_tokenStorePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "OAuth2Session: cannot write the token store" << m_tokenStorePath;
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(kStoreMagic + sealed);
    if (!file.commit())
        qWarning() << "OAuth2Session: cannot write the token store" << m_tokenStorePath;
}

void OAuth2Session::clearStore() const
{
    if (!m_tokenStorePath.isEmpty())
        QFile::remove(m_tokenStorePath);
}

void OAuth2Session::cancelSignIn()
{
    if (m_state == State::SigningIn)
        failSignIn(QStringLiteral("Sign-in cancelled."));
}

void OAuth2Session::signOut()
{
    abortFlow();
    const bool had = !m_accessToken.isEmpty() || !m_refreshToken.isEmpty();
    m_accessToken.clear();
    m_refreshToken.clear();
    m_idToken.clear();
    m_expiresAt = QDateTime();
    m_claims = QJsonObject();
    clearStore();
    setState(State::SignedOut);
    if (had)
        emit signedOut();
}

void OAuth2Session::withEndpoints(std::function<void()> next)
{
    const bool complete = m_config.authorizationEndpoint.isValid() && m_config.tokenEndpoint.isValid();
    if (complete || m_discovered) {
        next();
        return;
    }
    if (!m_config.issuer.isValid()) {
        failSignIn(QStringLiteral("OAuth2Config needs an issuer, or the authorization and token endpoints."));
        return;
    }

    QString base = m_config.issuer.toString();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QNetworkRequest request(QUrl(base + QStringLiteral("/.well-known/openid-configuration")));
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = m_nam->get(request);
    m_replies.append(reply);
    const int flow = m_flowId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, flow, next]() {
        reply->deleteLater();
        if (flow != m_flowId)
            return;
        m_replies.removeAll(reply);
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || doc.isEmpty()) {
            failSignIn(QStringLiteral("Could not read the identity provider's configuration: %1")
                           .arg(reply->errorString()));
            return;
        }
        if (!m_config.authorizationEndpoint.isValid())
            m_config.authorizationEndpoint = QUrl(doc["authorization_endpoint"].toString());
        if (!m_config.tokenEndpoint.isValid())
            m_config.tokenEndpoint = QUrl(doc["token_endpoint"].toString());
        if (!m_config.userInfoEndpoint.isValid() && doc.contains("userinfo_endpoint"))
            m_config.userInfoEndpoint = QUrl(doc["userinfo_endpoint"].toString());
        if (!m_config.authorizationEndpoint.isValid() || !m_config.tokenEndpoint.isValid()) {
            failSignIn(QStringLiteral("The identity provider's configuration lacks the authorization or token endpoint."));
            return;
        }
        m_discovered = true;
        next();
    });
}

bool OAuth2Session::startListening()
{
    const QString host = m_config.redirectUri.host();
    const int port = m_config.redirectUri.port();
    if (port <= 0) {
        failSignIn(QStringLiteral("The redirectUri needs an explicit port."));
        return false;
    }

    // "localhost" may resolve to either address family in the browser.
    QList<QHostAddress> required, optional;
    if (host == QLatin1String("localhost")) {
        required.append(QHostAddress(QHostAddress::LocalHost));
        optional.append(QHostAddress(QHostAddress::LocalHostIPv6));
    } else if (host == QLatin1String("127.0.0.1")) {
        required.append(QHostAddress(QHostAddress::LocalHost));
    } else if (host == QLatin1String("::1")) {
        required.append(QHostAddress(QHostAddress::LocalHostIPv6));
    } else {
        failSignIn(QStringLiteral("The redirectUri must point to localhost."));
        return false;
    }

    auto listen = [this, port](const QHostAddress& address) {
        auto* server = new QTcpServer(this);
        if (!server->listen(address, quint16(port))) {
            const QString error = server->errorString();
            delete server;
            return error;
        }
        connect(server, &QTcpServer::newConnection, this, [this, server]() {
            while (QTcpSocket* socket = server->nextPendingConnection()) {
                // Outlives the server, which is closed as soon as the code is in.
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onCallbackData(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        m_servers.append(server);
        return QString();
    };

    for (const QHostAddress& address : required) {
        const QString error = listen(address);
        if (!error.isEmpty()) {
            failSignIn(QStringLiteral("Cannot listen on port %1 for the sign-in callback: %2").arg(port).arg(error));
            return false;
        }
    }
    for (const QHostAddress& address : optional)
        listen(address);
    return true;
}

void OAuth2Session::startBrowserFlow()
{
    if (!startListening())
        return;

    m_pkceVerifier = QString::fromLatin1(randomUrlSafe(32));
    m_stateParam   = QString::fromLatin1(randomUrlSafe(16));
    m_nonce        = QString::fromLatin1(randomUrlSafe(16));
    const QByteArray challenge = QCryptographicHash::hash(m_pkceVerifier.toLatin1(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QList<QPair<QString, QString>> params = {
        {QStringLiteral("response_type"),         QStringLiteral("code")},
        {QStringLiteral("client_id"),             m_config.clientId},
        {QStringLiteral("redirect_uri"),          m_config.redirectUri.toString()},
        {QStringLiteral("scope"),                 m_config.scopes.join(QLatin1Char(' '))},
        {QStringLiteral("state"),                 m_stateParam},
        {QStringLiteral("code_challenge"),        QString::fromLatin1(challenge)},
        {QStringLiteral("code_challenge_method"), QStringLiteral("S256")},
    };
    if (m_config.scopes.contains(QLatin1String("openid")))
        params.append({QStringLiteral("nonce"), m_nonce});

    QUrl url = m_config.authorizationEndpoint;
    QByteArray query = url.query(QUrl::FullyEncoded).toLatin1();
    if (!query.isEmpty())
        query += '&';
    query += formEncoded(params);
    url.setQuery(QString::fromLatin1(query), QUrl::StrictMode);

    if (m_browserOpener) {
        m_browserOpener(url);
    } else if (!QDesktopServices::openUrl(url)) {
        failSignIn(QStringLiteral("Could not open the web browser for the sign-in."));
    }
}

void OAuth2Session::onCallbackData(QTcpSocket* socket)
{
    QByteArray buffer = socket->property("qtllm_buffer").toByteArray() + socket->readAll();
    if (!buffer.contains("\r\n\r\n")) {
        if (buffer.size() > 64 * 1024)
            socket->abort();
        else
            socket->setProperty("qtllm_buffer", buffer);
        return;
    }
    socket->setProperty("qtllm_buffer", QByteArray());

    // "GET /path?query HTTP/1.1"
    const QList<QByteArray> requestLine = buffer.left(buffer.indexOf("\r\n")).split(' ');
    const QUrl target(QStringLiteral("http://localhost") + QString::fromLatin1(requestLine.value(1)));
    if (requestLine.value(0) != "GET" || target.path() != m_config.redirectUri.path()) {
        sendHttp(socket, 404, "Not Found", htmlPage(QStringLiteral("Not found"), QString()));
        return;
    }
    if (m_state != State::SigningIn || m_stateParam.isEmpty()) {
        sendHttp(socket, 400, "Bad Request",
                 htmlPage(QStringLiteral("No sign-in in progress"),
                          QStringLiteral("Start the sign-in from the application again.")));
        return;
    }

    const QUrlQuery query(target);
    const QString state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
    if (state != m_stateParam) {
        // A stale tab from an earlier attempt, or not ours at all.
        sendHttp(socket, 400, "Bad Request",
                 htmlPage(QStringLiteral("Sign-in failed"),
                          QStringLiteral("This answer does not belong to the current sign-in.")));
        return;
    }

    const QString error = query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
    const QString code  = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    if (!error.isEmpty() || code.isEmpty()) {
        QString message = error.isEmpty() ? QStringLiteral("no code received") : error;
        const QString description = query.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded);
        if (!description.isEmpty())
            message += QStringLiteral(": ") + description;
        sendHttp(socket, 400, "Bad Request", htmlPage(QStringLiteral("Sign-in failed"), message));
        failSignIn(QStringLiteral("Sign-in failed: %1").arg(message));
        return;
    }

    sendHttp(socket, 200, "OK",
             htmlPage(QStringLiteral("Signed in"),
                      QStringLiteral("You can close this tab and return to the application.")));
    for (QTcpServer* server : m_servers) {
        server->close();
        server->deleteLater();
    }
    m_servers.clear();
    exchangeCode(code);
}

void OAuth2Session::postForm(const QUrl& url, const QList<QPair<QString, QString>>& form,
                             std::function<void(const QJsonObject&, int, const QString&)> done)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/x-www-form-urlencoded"));
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = m_nam->post(request, formEncoded(form));
    m_replies.append(reply);
    const int flow = m_flowId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, flow, done]() {
        reply->deleteLater();
        if (flow != m_flowId)
            return;
        m_replies.removeAll(reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::NoError && status < 400) {
            done(json, status, QString());
            return;
        }
        QString error = json["error_description"].toString();
        if (error.isEmpty())
            error = json["error"].toString();
        if (error.isEmpty())
            error = reply->errorString();
        done(json, status, error);
    });
}

bool OAuth2Session::acceptTokens(const QJsonObject& json, QString* error)
{
    const QString access = json["access_token"].toString();
    if (access.isEmpty()) {
        *error = QStringLiteral("The token answer contains no access token.");
        return false;
    }
    m_accessToken = access;
    const QString refresh = json["refresh_token"].toString();
    if (!refresh.isEmpty())
        m_refreshToken = refresh;   // a refresh answer may keep the old one valid
    const qint64 expiresIn = json["expires_in"].toVariant().toLongLong();
    m_expiresAt = expiresIn > 0 ? QDateTime::currentDateTimeUtc().addSecs(expiresIn) : QDateTime();
    const QString id = json["id_token"].toString();
    if (!id.isEmpty()) {
        m_idToken = id;
        const QJsonObject payload = jwtPayload(id);
        for (auto it = payload.begin(); it != payload.end(); ++it)
            m_claims.insert(it.key(), it.value());
    }
    return true;
}

void OAuth2Session::exchangeCode(const QString& code)
{
    const QString verifier = m_pkceVerifier;
    postForm(m_config.tokenEndpoint,
             {{QStringLiteral("grant_type"),    QStringLiteral("authorization_code")},
              {QStringLiteral("code"),          code},
              {QStringLiteral("redirect_uri"),  m_config.redirectUri.toString()},
              {QStringLiteral("client_id"),     m_config.clientId},
              {QStringLiteral("code_verifier"), verifier}},
             [this](const QJsonObject& json, int, const QString& error) {
                 if (!error.isEmpty()) {
                     failSignIn(QStringLiteral("Could not get a token: %1").arg(error));
                     return;
                 }
                 // A new sign-in may be a different user: start the claims afresh.
                 const QJsonObject previousClaims = m_claims;
                 m_claims = QJsonObject();
                 QString acceptError;
                 if (!acceptTokens(json, &acceptError)) {
                     m_claims = previousClaims;
                     failSignIn(acceptError);
                     return;
                 }
                 const QString nonce = m_claims["nonce"].toString();
                 if (!nonce.isEmpty() && nonce != m_nonce) {
                     signOut();
                     emit signInFailed(QStringLiteral("The ID token does not belong to this sign-in."));
                     return;
                 }
                 fetchUserInfo();
             });
}

void OAuth2Session::fetchUserInfo()
{
    if (!m_config.userInfoEndpoint.isValid()) {
        finishSignIn();
        return;
    }
    QNetworkRequest request(m_config.userInfoEndpoint);
    request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = m_nam->get(request);
    m_replies.append(reply);
    const int flow = m_flowId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, flow]() {
        reply->deleteLater();
        if (flow != m_flowId)
            return;
        m_replies.removeAll(reply);
        // Only extra detail: a failure here does not fail the sign-in.
        const QJsonObject info = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::NoError) {
            for (auto it = info.begin(); it != info.end(); ++it)
                m_claims.insert(it.key(), it.value());
        }
        finishSignIn();
    });
}

void OAuth2Session::finishSignIn()
{
    m_signInTimer.stop();
    m_pkceVerifier.clear();
    m_stateParam.clear();
    saveStore();
    setState(State::SignedIn);
    emit signedIn();
}

void OAuth2Session::refresh()
{
    if (m_refreshToken.isEmpty() || m_refreshing || !m_config.tokenEndpoint.isValid()
        || m_state != State::SignedIn)
        return;
    m_refreshing = true;
    postForm(m_config.tokenEndpoint,
             {{QStringLiteral("grant_type"),    QStringLiteral("refresh_token")},
              {QStringLiteral("refresh_token"), m_refreshToken},
              {QStringLiteral("client_id"),     m_config.clientId}},
             [this](const QJsonObject& json, int status, const QString& error) {
                 m_refreshing = false;
                 QString message = error;
                 if (message.isEmpty() && acceptTokens(json, &message)) {
                     saveStore();
                     emit tokenRefreshed();
                     return;
                 }
                 if (status == 0)
                     return;   // no answer (offline, asleep): the next tick tries again
                 // The provider refused: the refresh token has run out or was revoked.
                 signOut();
                 emit signInRequired(QStringLiteral("The sign-in has expired: %1").arg(message));
             });
}

void OAuth2Session::onTick()
{
    if (m_state != State::SignedIn || !m_expiresAt.isValid())
        return;
    const qint64 secsLeft = QDateTime::currentDateTimeUtc().secsTo(m_expiresAt);
    if (secsLeft > m_config.refreshMarginSecs)
        return;
    if (!m_refreshToken.isEmpty()) {
        refresh();
    } else if (secsLeft <= 0) {
        signOut();
        emit signInRequired(QStringLiteral("The sign-in has expired."));
    }
}

} // namespace QtLLM
