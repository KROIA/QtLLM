#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QJsonArray>
#include <QJsonObject>

class QImage;

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

// A result that shows the model a picture: {"status":"ok", ...payload} plus the
// image under the reserved key "$images". The protocol layer takes that key
// out of the JSON and sends each image in the provider's own image format, so
// the model sees it rather than reading base64 text.
//
// The image is scaled down to maxEdge pixels on its longer side first (cost
// grows with pixel count) and encoded as PNG, or as JPEG when that is much
// smaller. Several images: call toolAddImage() on the returned object.
QT_LLM_API QJsonObject toolImage(const QImage& image,
                                 const QJsonObject& payload = QJsonObject(),
                                 int maxEdge = 1568);
QT_LLM_API void toolAddImage(QJsonObject& result, const QImage& image, int maxEdge = 1568);

// Removes the images from a result and returns them. Leaves an "images": N
// note in the JSON so that whoever reads the text (the model, a log, the
// tool-call card in the chat) still knows pictures belong to it.
QT_LLM_API QJsonArray toolTakeImages(QJsonObject& result);

// Key under which toolImage() stores its images:
// [{"media_type":"image/png","data":"<base64>","width":W,"height":H}, ...]
QT_LLM_API extern const char* const kToolImagesKey;

} // namespace QtLLM
