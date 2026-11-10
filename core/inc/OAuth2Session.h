#pragma once
#include "QtLLM_base.h"
#include "ProtocolBase.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTcpSocket;

namespace QtLLM {

// Where and how to sign in. Nothing in here is a secret: a desktop app is a
// public client, its client ID may be compiled into the binary.
struct QT_LLM_API OAuth2Config
{
    // The identity provider. Its endpoints are read from
    // <issuer>/.well-known/openid-configuration on the first sign-in.
    QUrl        issuer;
    // Set these to skip the discovery document (or to override what it says).
    QUrl        authorizationEndpoint;
    QUrl        tokenEndpoint;
    QUrl        userInfoEndpoint;     // optional: claims are merged into claims()
    QString     clientId;
    // "openid" is what makes the provider return an ID token with the user's
    // name; add whatever else the provider offers ("profile", "email", ...).
    QStringList scopes = {QStringLiteral("openid")};
    // Must be registered with the identity provider exactly as written here,
    // port included, e.g. "http://localhost:12000/auth/callback". The session
    // listens on that port of the loopback interface during a sign-in.
    QUrl        redirectUri;
    // A sign-in the user does not complete in the browser within this time fails.
    int         signInTimeoutMs = 5 * 60 * 1000;
    // The access token is refreshed this long before it expires.
    int         refreshMarginSecs = 120;
};

// A user's sign-in with an OAuth2 / OpenID Connect identity provider, for a
// gateway that authenticates users instead of handing out API keys: an OAuth2
// proxy checks the user's token and puts the real API key into the request.
//
// Uses the Authorization Code flow with PKCE (RFC 7636) and a loopback
// redirect (RFC 8252): signIn() opens the system browser at the provider's
// login page - usually passed straight through when the user is already
// signed in to the company account - the provider redirects to the local
// redirectUri with a code, and the code is exchanged for tokens. No client
// secret is involved. Tokens are refreshed in the background while the
// refresh token lasts; after that signInRequired() asks for a new sign-in.
// They are kept in memory only, unless setTokenStorePath() names a file to
// keep them in between runs.
//
//   OAuth2Config cfg;
//   cfg.issuer      = QUrl("https://idp.example.com/oauth");
//   cfg.clientId    = "...";
//   cfg.scopes      = {"openid", "profile"};
//   cfg.redirectUri = QUrl("http://localhost:12000/auth/callback");
//   auto* session = new OAuth2Session(cfg, this);
//   client->setAccessTokenProvider(session->accessTokenProvider());
//   connect(session, &OAuth2Session::signedIn, ...);
//   session->signIn();
//
// The ID token is decoded without checking its signature: it comes straight
// from the token endpoint over TLS and claims() is meant for display. The
// gateway is what verifies the access token.
class QT_LLM_API OAuth2Session : public QObject
{
    Q_OBJECT
public:
    enum class State {
        SignedOut,
        SigningIn,    // waiting for the browser, or exchanging the code
        SignedIn
    };
    Q_ENUM(State)

    explicit OAuth2Session(const OAuth2Config& config, QObject* parent = nullptr);
    ~OAuth2Session() override;

    const OAuth2Config& config() const { return m_config; }

    // Sign in. Ends in signedIn() or signInFailed(). With a token store, the
    // stored tokens are tried first: a still valid access token signs in at
    // once, an expired one is refreshed silently, and only when neither works
    // the browser opens. Called while a sign-in is already waiting, the old
    // one is dropped and the browser opened again - the user may have closed
    // the first tab.
    void signIn();

    // Keep the tokens in this file between runs, so a restart signs in
    // without the browser as long as the refresh token lasts. The application
    // chooses a per-user location, e.g.
    //   QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
    //       + "/oauth2-session.dat"
    // The file holds a credential: on Windows it is encrypted for the current
    // Windows user (DPAPI) and unreadable for anyone else, elsewhere it is
    // created readable for the owner only. Written after every sign-in and
    // refresh, deleted by signOut() and when the session expires. Tokens of
    // another issuer or client ID in the file are ignored. Empty = memory only.
    void setTokenStorePath(const QString& path);
    QString tokenStorePath() const { return m_tokenStorePath; }
    // Abandon a sign-in in progress; signInFailed("cancelled") follows.
    void cancelSignIn();
    // Forget all tokens. Does not end the browser's session with the provider.
    void signOut();

    State state() const { return m_state; }
    // A valid access token is present.
    bool isSignedIn() const;

    // Empty when signed out or expired.
    QString   accessToken() const;
    QString   idToken() const { return m_idToken; }
    QDateTime expiresAt() const { return m_expiresAt; }   // invalid = not known

    // ID token claims, merged with the userinfo answer when an endpoint is
    // known. Which claims exist depends on the provider and the scopes.
    QJsonObject claims() const { return m_claims; }
    // Best name to show: "name", given + family name, "preferred_username",
    // the e-mail address, "sub" - the first one present.
    QString     displayName() const;
    // "email", else the first claim among "preferred_username", "website"
    // and "upn" that holds an e-mail address.
    QString     email() const;
    // "groups" claim, as array or as a single string.
    QStringList groups() const;

    // For Client::setAccessTokenProvider() / AgentConfig: hands out the
    // current access token, empty once this session is gone.
    AccessTokenProvider accessTokenProvider() const;

    // Replaces opening the system browser - for tests, or to show the login
    // page in an embedded web view. Called with the authorization URL.
    void setBrowserOpener(std::function<void(const QUrl&)> opener);

    // Refresh the access token now (it is refreshed automatically before it
    // expires). No-op without a refresh token or while a refresh runs.
    void refresh();

signals:
    void stateChanged(QtLLM::OAuth2Session::State state);
    void signedIn();
    void signInFailed(const QString& error);
    void tokenRefreshed();
    void signedOut();
    // The session ended on its own: the refresh token expired or was revoked.
    // Call signIn() again.
    void signInRequired(const QString& reason);

private:
    void setState(State state);
    void abortFlow();
    void failSignIn(const QString& error);
    void withEndpoints(std::function<void()> next);
    void startBrowserFlow();
    bool startListening();
    void onCallbackData(QTcpSocket* socket);
    void exchangeCode(const QString& code);
    void fetchUserInfo();
    void finishSignIn();
    void onTick();
    void refreshStoredTokens();       // signIn() with an expired stored access token
    bool loadStore();                 // true: usable tokens were read
    void saveStore() const;
    void clearStore() const;

    // POST a form; `done` gets the JSON answer or an error message.
    // httpStatus is 0 when no response arrived at all.
    void postForm(const QUrl& url, const QList<QPair<QString, QString>>& form,
                  std::function<void(const QJsonObject& json, int httpStatus, const QString& error)> done);
    // Takes access/refresh/id token and expiry from a token response.
    bool acceptTokens(const QJsonObject& json, QString* error);

    static QByteArray randomUrlSafe(int bytes);

    OAuth2Config           m_config;
    State                  m_state = State::SignedOut;
    QNetworkAccessManager* m_nam;
    QList<QTcpServer*>     m_servers;           // loopback listeners during a sign-in
    QList<QPointer<QNetworkReply>> m_replies;   // in flight, aborted with the flow
    std::function<void(const QUrl&)> m_browserOpener;
    QTimer                 m_signInTimer;
    QTimer                 m_tickTimer;         // refresh check; survives sleep
    int                    m_flowId = 0;        // stale callbacks compare against this
    bool                   m_refreshing = false;
    bool                   m_discovered = false;

    QString    m_tokenStorePath;

    QString    m_pkceVerifier;
    QString    m_stateParam;
    QString    m_nonce;

    QString    m_accessToken;
    QString    m_refreshToken;
    QString    m_idToken;
    QDateTime  m_expiresAt;
    QJsonObject m_claims;
};

} // namespace QtLLM
