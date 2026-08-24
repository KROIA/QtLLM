#pragma once
#include "QtLLM_base.h"

namespace QtLLM {

// Token usage and cost statistics reported after each completed turn.
struct QT_LLM_API UsageStats
{
    // Per-turn figures (reset each time beginTurn() is called)
    int    inputTokens              = 0; // prompt tokens consumed this turn (including tool loops)
    int    outputTokens             = 0; // completion tokens generated this turn
    int    cacheCreationInputTokens = 0; // tokens written to prompt cache this turn
    int    cacheReadInputTokens     = 0; // tokens read from prompt cache this turn
    int    toolCalls                = 0; // number of tool invocations within this turn
    qint64 durationMs               = 0; // wall-clock time from first HTTP send to final response

    // Session totals (cumulative since construction or clearConversation)
    int    sessionInputTokens              = 0;
    int    sessionOutputTokens             = 0;
    int    sessionCacheCreationInputTokens = 0;
    int    sessionCacheReadInputTokens     = 0;
    int    sessionToolCalls                = 0;
    int    sessionTurnCount                = 0; // number of completed user turns
    double sessionCostUsd                  = 0.0; // estimated USD cost; always 0 for local providers

    int    totalTokens()        const { return inputTokens  + outputTokens; }
    int    sessionTotalTokens() const { return sessionInputTokens + sessionOutputTokens; }
};

// Rough (chars/4) breakdown of what currently makes up the next request's
// context, by category. Not exact token counts — the real tokenizer runs
// server-side — but close enough to visualize where the context budget goes.
struct QT_LLM_API ContextBreakdown
{
    int systemPromptTokens = 0;
    int toolsTokens        = 0;
    int messagesTokens     = 0;
    int contextWindowTokens = 200000; // model's max context size; real value once fetched, static estimate until then

    // Real token count for the whole request (system + tools + messages),
    // as reported by the provider's own tokenizer via ContextInfoProvider.
    // -1 until the first async count comes back; segments above stay
    // chars/4 estimates regardless (categorizing by section isn't something
    // any tokenizer API reports, only the aggregate is real).
    int exactUsedTokens = -1;

    int usedTokens() const { return systemPromptTokens + toolsTokens + messagesTokens; }
    int bestUsedTokens() const { return exactUsedTokens >= 0 ? exactUsedTokens : usedTokens(); }
};

} // namespace QtLLM
