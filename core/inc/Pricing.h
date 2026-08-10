#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QString>
#include <QMap>
#include <QJsonObject>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

namespace QtLLM
{

// Token prices for one model, in USD per 1M tokens per category.
// Negative cache prices mean "derive from input": cache read = 10% of the
// input price, cache write = 125% (5-minute ephemeral premium).
struct QT_LLM_API ModelPricing
{
    double inputPerMTok      = 0.0;
    double outputPerMTok     = 0.0;
    double cacheReadPerMTok  = -1.0;
    double cacheWritePerMTok = -1.0;

    bool   isValid() const { return inputPerMTok > 0.0 || outputPerMTok > 0.0; }
    double effectiveCacheRead() const
        { return cacheReadPerMTok  >= 0.0 ? cacheReadPerMTok  : inputPerMTok * 0.10; }
    double effectiveCacheWrite() const
        { return cacheWritePerMTok >= 0.0 ? cacheWritePerMTok : inputPerMTok * 1.25; }
};

// App-supplied pricing source. Return a default-constructed (invalid)
// ModelPricing to fall through to the next resolution layer.
using PricingResolver = std::function<ModelPricing(const QString& model)>;

// Process-wide model-price registry used for all cost estimation.
//
// Resolution order for pricingFor(model):
//   1. custom resolver (setResolver)          — full control, any source
//   2. injected entries (setPricing)          — per model or substring pattern
//   3. loaded table (loadLiteLlmJson /
//      fetchOnlinePricing)                    — LiteLLM community JSON
//   4. built-in fallback table                — hard-coded approximations
//
// Everything is optional: without any injection the behaviour equals the old
// hard-coded table. All methods are GUI-thread only (like the rest of QtLLM).
class QT_LLM_API PricingRegistry : public QObject
{
    Q_OBJECT
public:
    static PricingRegistry& instance();

    // Resolved pricing for a model id (case-insensitive, provider prefixes
    // like "anthropic/" are ignored). Invalid pricing (all zero) if unknown.
    ModelPricing pricingFor(const QString& model) const;

    // Cost estimate in USD across all token categories.
    double estimateCostUsd(const QString& model,
                           qint64 inputTokens, qint64 outputTokens,
                           qint64 cacheReadTokens = 0,
                           qint64 cacheWriteTokens = 0) const;

    // --- Injection points -------------------------------------------------

    // Set/override pricing for a model. modelPattern matches exactly or as
    // substring (e.g. "sonnet-4" matches "claude-sonnet-4-5"). Highest
    // priority after the resolver.
    void setPricing(const QString& modelPattern, const ModelPricing& pricing);

    // Install a custom source consulted first (own backend, config file,
    // different vendor API, ...). Pass nullptr to remove.
    void setResolver(PricingResolver resolver);

    // Remove all injected entries and any loaded online table (built-in
    // fallback stays).
    void clearCustomPricing();

    // --- LiteLLM community pricing -----------------------------------------

    // Load a LiteLLM-format object: {"<model>": {"input_cost_per_token": ...,
    // "output_cost_per_token": ..., "cache_read_input_token_cost": ...,
    // "cache_creation_input_token_cost": ...}, ...}. Costs are per single
    // token and converted to per-1M internally. Returns the number of models
    // loaded. Usable directly for own files in the same format.
    int loadLiteLlmJson(const QJsonObject& root);

    // Async fetch of the LiteLLM community pricing JSON (or any URL serving
    // that format). A previously cached copy on disk is loaded immediately;
    // the network is only hit when the cache is missing or older than
    // maxCacheAgeDays. Success → cache updated + pricingUpdated(); failure →
    // fetchFailed() and the previous data stays active.
    void fetchOnlinePricing(const QUrl& url = defaultLiteLlmUrl(),
                            int maxCacheAgeDays = 7);

    static QUrl defaultLiteLlmUrl();

    // Location of the on-disk cache written by fetchOnlinePricing().
    QString cacheFilePath() const;

signals:
    // modelCount = models in the active loaded table; source is "cache",
    // "network", "json" (loadLiteLlmJson) or "manual" (setPricing).
    void pricingUpdated(int modelCount, const QString& source);
    void fetchFailed(const QString& errorMessage);

private:
    explicit PricingRegistry(QObject* parent = nullptr);

    static QString normalizeModel(const QString& model);
    static ModelPricing builtinPricing(const QString& normalizedModel);
    static ModelPricing lookup(const QMap<QString, ModelPricing>& table,
                               const QString& normalizedModel);

    PricingResolver              m_resolver;
    QMap<QString, ModelPricing>  m_userTable;    // setPricing entries
    QMap<QString, ModelPricing>  m_loadedTable;  // LiteLLM / online data
    QNetworkAccessManager*       m_networkManager = nullptr;
};

} // namespace QtLLM
