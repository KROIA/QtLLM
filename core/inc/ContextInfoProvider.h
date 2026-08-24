#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QString>
#include <QJsonArray>

namespace QtLLM
{
    // Generic, per-provider source of *real* context accounting - the model's
    // actual max context window, and an exact token count for a given
    // system prompt + tool schemas + message history, as reported by the
    // provider's own tokenizer/API instead of a chars/4 estimate.
    //
    // Both calls are async (network round-trip). Concrete implementations:
    //  - OllamaContextInfoProvider: both calls are real (POST /api/show,
    //    POST /api/embed).
    //  - ClaudeContextInfoProvider: countTokens() is real (POST
    //    /v1/messages/count_tokens); fetchContextWindowTokens() has no
    //    Anthropic API to call, so it's a mocked constant - swap in a live
    //    lookup there the day Anthropic exposes one, nothing else changes.
    class QT_LLM_API ContextInfoProvider : public QObject
    {
        Q_OBJECT
    public:
        using QObject::QObject;

        // Fetch the model's real max context window size, in tokens.
        virtual void fetchContextWindowTokens(const QString& model) = 0;

        // Count tokens for a full request (system prompt + enabled tool
        // schemas + message history) using the provider's own tokenizer.
        virtual void countTokens(const QString& model,
                                  const QString& systemPrompt,
                                  const QJsonArray& toolSchemas,
                                  const QJsonArray& messages) = 0;

    signals:
        void contextWindowTokensReady(int tokens);
        void tokenCountReady(int tokens);
        void contextInfoErrorOccurred(const QString& message);
    };
}
