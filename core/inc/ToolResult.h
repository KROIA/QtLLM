#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QJsonObject>

namespace QtLLM
{

// Helpers for the tool-result convention understood by the protocol layer:
// a result with {"status":"error"} is sent to the model as tool_result with
// is_error = true (see ClaudeProtocol::executeToolCalls). Documented in
// documentation/ToolUse.md.

// {"status":"error","message":<message>, ...extra}
QT_LLM_API QJsonObject toolError(const QString& message,
                                 const QJsonObject& extra = QJsonObject());

// {"status":"ok", ...payload}
QT_LLM_API QJsonObject toolOk(const QJsonObject& payload = QJsonObject());

} // namespace QtLLM
