#include "Pricing.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
    // USD per 1M tokens; approximate as of mid-2025. Last-resort fallback —
    // fetchOnlinePricing()/setPricing() take precedence.
    if (m.contains("opus-4"))    return {15.0,  75.0, -1.0, -1.0};
    if (m.contains("sonnet-4"))  return { 3.0,  15.0, -1.0, -1.0};
    if (m.contains("haiku-4"))   return { 0.80,  4.0, -1.0, -1.0};
    if (m.contains("opus-3"))    return {15.0,  75.0, -1.0, -1.0};
    if (m.contains("sonnet-3"))  return { 3.0,  15.0, -1.0, -1.0};
    if (m.contains("haiku-3"))   return { 0.25,  1.25, -1.0, -1.0};
    return {};
}

ModelPricing PricingRegistry::lookup(const QMap<QString, ModelPricing>& table,
                                     const QString& normalizedModel)
{
    // Exact match first
    auto it = table.constFind(normalizedModel);
    if (it != table.constEnd())
        return it.value();

    // Longest pattern that matches as substring (either direction — LiteLLM
    // keys often carry date suffixes the app's model id omits, or vice versa)
    ModelPricing best;
    int bestLength = 0;
    for (auto entry = table.constBegin(); entry != table.constEnd(); ++entry) {
        const QString& key = entry.key();
        if (key.size() <= bestLength)
            continue;
        if (normalizedModel.contains(key) || key.contains(normalizedModel)) {
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

    ModelPricing p = lookup(m_userTable, norm);
    if (p.isValid())
        return p;

    p = lookup(m_loadedTable, norm);
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
        m_loadedTable.insert(normalizeModel(it.key()), p);
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
