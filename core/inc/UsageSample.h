#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QtGlobal>

namespace QtLLM {

// One recorded turn for usage-history persistence and charting.
struct QT_LLM_API UsageSample
{
    qint64  timestampMsEpoch         = 0;  // QDateTime::currentMSecsSinceEpoch() at turn end
    QString model;                         // model id this turn ran on
    QString provider;                      // "claude" | "ollama"
    int     inputTokens              = 0;  // uncached input (Anthropic usage.input_tokens)
    int     outputTokens             = 0;
    int     cacheReadInputTokens     = 0;
    int     cacheCreationInputTokens = 0;  // "cache write"
    int     toolCalls                = 0;
    qint64  durationMs               = 0;
    double  costUsd                  = 0.0; // estimate

    // Total prompt tokens for this turn (uncached + cache-read + cache-write).
    int totalInputTokens() const
    {
        return inputTokens + cacheReadInputTokens + cacheCreationInputTokens;
    }

    int totalTokens() const
    {
        return totalInputTokens() + outputTokens;
    }
};

} // namespace QtLLM
