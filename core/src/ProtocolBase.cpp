#include "ProtocolBase.h"

namespace QtLLM {

QString effortName(Effort effort)
{
    switch (effort) {
    case Effort::Low:     return QStringLiteral("low");
    case Effort::Medium:  return QStringLiteral("medium");
    case Effort::High:    return QStringLiteral("high");
    case Effort::XHigh:   return QStringLiteral("xhigh");
    case Effort::Max:     return QStringLiteral("max");
    case Effort::Default: break;
    }
    return QString();
}

} // namespace QtLLM
namespace QtLLM {
ProtocolBase::ProtocolBase(QObject* parent) : QObject(parent) {}
ProtocolBase::~ProtocolBase() = default;
} // namespace QtLLM
