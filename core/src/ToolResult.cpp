#include "ToolResult.h"

namespace QtLLM
{

QJsonObject toolError(const QString& message, const QJsonObject& extra)
{
    QJsonObject result = extra;
    result["status"]  = QStringLiteral("error");
    result["message"] = message;
    return result;
}

QJsonObject toolOk(const QJsonObject& payload)
{
    QJsonObject result = payload;
    result["status"] = QStringLiteral("ok");
    return result;
}

} // namespace QtLLM
