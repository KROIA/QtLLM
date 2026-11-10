#include "Pricing.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace QtLLM
{

PricingRegistry& PricingRegistry::instance()
{
    static PricingRegistry registry;
    return registry;
}

PricingRegistry::PricingRegistry(QObject* parent)
    : QObject(parent)
{
}

QString PricingRegistry::normalizeModel(const QString& model)
{
    QString m = model.toLower().trimmed();
    // Strip provider prefixes like "anthropic/" or "openrouter/anthropic/"
    const int slash = m.lastIndexOf('/');
    if (slash >= 0)
        m = m.mid(slash + 1);
    return m;
}

ModelPricing PricingRegistry::builtinPricing(const QString& m)
{
    // USD per 1M tokens; Anthropic list prices as of 2026-11. Last-resort
    // fallback — fetchOnlinePricing()/setPricing() take precedence.
    struct Entry { const char* pattern; ModelPricing pricing; };
    // Substring patterns, most specific first: "opus-5-5" also contains
    // "opus-5", so the first hit must be the longest applicable pattern.
    static const Entry kTable[] = {
        {"fable-5-2",  { 8.0,  40.0,  0.20, -1.0}},
        {"mythos-5-2", { 8.0,  40.0,  0.20, -1.0}},
        {"fable-5-1",  {10.0,  50.0,  0.25, -1.0}},
        {"mythos-5-1", {10.0,  50.0,  0.25, -1.0}},
        {"fable-5",    {10.0,  50.0, -1.0,  -1.0}},
        {"mythos-5",   {10.0,  50.0, -1.0,  -1.0}},
        {"opus-5-5",   { 4.0,  20.0,  0.20, -1.0}},
        {"opus-5",     { 5.0,  25.0, -1.0,  -1.0}},
        {"opus-4-8",   { 5.0,  25.0, -1.0,  -1.0}},
        {"opus-4-7",   { 5.0,  25.0, -1.0,  -1.0}},
        {"opus-4-6",   { 5.0,  25.0, -1.0,  -1.0}},
        {"opus-4-5",   { 5.0,  25.0, -1.0,  -1.0}},
        {"sonnet-5-5", { 2.0,  10.0,  0.10, -1.0}},
        {"sonnet-5",   { 2.0,  10.0, -1.0,  -1.0}},
        {"sonnet-4-6", { 3.0,  15.0, -1.0,  -1.0}},
        {"sonnet-4-5", { 3.0,  15.0, -1.0,  -1.0}},
        {"haiku-5-5",  { 0.10,  0.50, -1.0, -1.0}},
        {"haiku-4-5",  { 1.0,   5.0, -1.0,  -1.0}},
        {"opus-4",     {15.0,  75.0, -1.0,  -1.0}},
        {"sonnet-4",   { 3.0,  15.0, -1.0,  -1.0}},
        {"haiku-4",    { 0.80,  4.0, -1.0,  -1.0}},
        {"opus-3",     {15.0,  75.0, -1.0,  -1.0}},
        {"sonnet-3",   { 3.0,  15.0, -1.0,  -1.0}},
        {"haiku-3",    { 0.25,  1.25, -1.0, -1.0}},
    };
    for (const Entry& e : kTable) {
        if (m.contains(QLatin1String(e.pattern)))
            return e.pricing;
    }
    return {};
}

// True when the longer id is the shorter one plus a release tag (date or
// "latest"), e.g. "claude-x-4-5" vs "claude-x-4-5-20251101" or "...@20251101".
// A version step like "claude-x-5" vs "claude-x-5-5" is a different model.
static bool sameModelModuloDate(const QString& a, const QString& b)
{
    const QString& shorter = a.size() <= b.size() ? a : b;
    const QString& longer  = a.size() <= b.size() ? b : a;
    if (shorter.isEmpty() || !longer.startsWith(shorter))
        return false;
    static const QRegularExpression kTag(
        QStringLiteral("^[-@](\\d{8}|latest)([-@:].*)?$"));
    return kTag.match(longer.mid(shorter.size())).hasMatch();
}

ModelPricing PricingRegistry::lookup(const QMap<QString, ModelPricing>& table,
                                     const QString& normalizedModel,
                                     bool substringPatterns)
{
    // Exact match first
    auto it = table.constFind(normalizedModel);
    if (it != table.constEnd())
        return it.value();

    // Longest key that is the same model up to a date suffix (either
    // direction). Injected entries additionally match as substring patterns.
    ModelPricing best;
    int bestLength = 0;
    for (auto entry = table.constBegin(); entry != table.constEnd(); ++entry) {
        const QString& key = entry.key();
        if (key.size() <= bestLength)
            continue;
        if (sameModelModuloDate(normalizedModel, key)
            || (substringPatterns && !key.isEmpty() && normalizedModel.contains(key))) {
            best = entry.value();
            bestLength = key.size();
        }
    }
    return best;
}

ModelPricing PricingRegistry::pricingFor(const QString& model) const
{
    if (m_resolver) {
        ModelPricing p = m_resolver(model);
        if (p.isValid())
            return p;
    }

    const QString norm = normalizeModel(model);
    if (norm.isEmpty())
        return {};

    ModelPricing p = lookup(m_userTable, norm, true);
    if (p.isValid())
        return p;

    p = lookup(m_loadedTable, norm, false);
    if (p.isValid())
        return p;

    return builtinPricing(norm);
}

double PricingRegistry::estimateCostUsd(const QString& model,
                                        qint64 inputTokens, qint64 outputTokens,
                                        qint64 cacheReadTokens,
                                        qint64 cacheWriteTokens) const
{
    const ModelPricing p = pricingFor(model);
    if (!p.isValid())
        return 0.0;
    return (inputTokens      * p.inputPerMTok
          + cacheReadTokens  * p.effectiveCacheRead()
          + cacheWriteTokens * p.effectiveCacheWrite()
          + outputTokens     * p.outputPerMTok) / 1'000'000.0;
}

void PricingRegistry::setPricing(const QString& modelPattern,
                                 const ModelPricing& pricing)
{
    m_userTable.insert(normalizeModel(modelPattern), pricing);
    emit pricingUpdated(m_loadedTable.size(), QStringLiteral("manual"));
}

void PricingRegistry::setResolver(PricingResolver resolver)
{
    m_resolver = std::move(resolver);
}

void PricingRegistry::clearCustomPricing()
{
    m_userTable.clear();
    m_loadedTable.clear();
}

int PricingRegistry::loadLiteLlmJson(const QJsonObject& root)
{
    int loaded = 0;
    // Resellers (vertex_ai/, bedrock/, openrouter/...) normalise to the same
    // key as the first-party entry but may carry different prices; the
    // first-party price must win regardless of key order.
    QSet<QString> firstPartyKeys;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject entry = it.value().toObject();
        if (!entry.contains("input_cost_per_token")
            && !entry.contains("output_cost_per_token"))
            continue;

        ModelPricing p;
        p.inputPerMTok  = entry["input_cost_per_token"].toDouble()  * 1'000'000.0;
        p.outputPerMTok = entry["output_cost_per_token"].toDouble() * 1'000'000.0;
        if (entry.contains("cache_read_input_token_cost"))
            p.cacheReadPerMTok =
                entry["cache_read_input_token_cost"].toDouble() * 1'000'000.0;
        if (entry.contains("cache_creation_input_token_cost"))
            p.cacheWritePerMTok =
                entry["cache_creation_input_token_cost"].toDouble() * 1'000'000.0;

        if (!p.isValid())
            continue;   // free/local models — keep fallthrough to 0-cost
        const QString key = normalizeModel(it.key());
        if (key.isEmpty())
            continue;
        const QString rawKey = it.key().toLower();
        const bool firstParty = !rawKey.contains('/')
                                || rawKey.startsWith(QLatin1String("anthropic/"));
        if (firstParty)
            firstPartyKeys.insert(key);
        else if (firstPartyKeys.contains(key))
            continue;
        m_loadedTable.insert(key, p);
        ++loaded;
    }
    if (loaded > 0)
        emit pricingUpdated(m_loadedTable.size(), QStringLiteral("json"));
    return loaded;
}

QUrl PricingRegistry::defaultLiteLlmUrl()
{
    return QUrl(QStringLiteral(
        "https://raw.githubusercontent.com/BerriAI/litellm/main/"
        "model_prices_and_context_window.json"));
}

QString PricingRegistry::cacheFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/QtLLM/litellm_pricing.json");
}

void PricingRegistry::fetchOnlinePricing(const QUrl& url, int maxCacheAgeDays)
{
    // 1) Serve from the disk cache immediately (stale data beats fallback).
    const QString cachePath = cacheFilePath();
    bool cacheFresh = false;
    QFileInfo cacheInfo(cachePath);
    if (cacheInfo.exists()) {
        QFile file(cachePath);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (doc.isObject() && loadLiteLlmJson(doc.object()) > 0) {
                emit pricingUpdated(m_loadedTable.size(), QStringLiteral("cache"));
                cacheFresh = cacheInfo.lastModified()
                                 .daysTo(QDateTime::currentDateTime())
                             < maxCacheAgeDays;
            }
        }
    }
    if (cacheFresh)
        return;

    // 2) Refresh from the network.
    if (!m_networkManager)
        m_networkManager = new QNetworkAccessManager(this);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cachePath]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit fetchFailed(reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit fetchFailed(QStringLiteral("pricing response is not a JSON object"));
            return;
        }
        if (loadLiteLlmJson(doc.object()) <= 0) {
            emit fetchFailed(QStringLiteral("pricing response contained no usable models"));
            return;
        }

        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        QFile file(cachePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(body);

        emit pricingUpdated(m_loadedTable.size(), QStringLiteral("network"));
    });
}

} // namespace QtLLM
