#pragma once
#include "QtLLM_base.h"

namespace QtLLM {

// Token usage and cost statistics reported after each completed turn.
struct QT_LLM_API UsageStats
{
    // Per-turn figures (reset each time beginTurn() is called)
    int    inputTokens  = 0; // prompt tokens consumed this turn (including tool loops)
    int    outputTokens = 0; // completion tokens generated this turn
    int    toolCalls    = 0; // number of tool invocations within this turn
    qint64 durationMs   = 0; // wall-clock time from first HTTP send to final response

    // Session totals (cumulative since construction or clearConversation)
    int    sessionInputTokens  = 0;
    int    sessionOutputTokens = 0;
    int    sessionToolCalls    = 0;
    int    sessionTurnCount    = 0; // number of completed user turns
    double sessionCostUsd      = 0.0; // estimated USD cost; always 0 for local providers

    int    totalTokens()        const { return inputTokens  + outputTokens; }
    int    sessionTotalTokens() const { return sessionInputTokens + sessionOutputTokens; }
};

} // namespace QtLLM
