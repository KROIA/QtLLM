#include "ToolResult.h"

#include <QBuffer>
#include <QImage>
#include <QJsonArray>

namespace QtLLM
{

const char* const kToolImagesKey = "$images";

void toolAddImage(QJsonObject& result, const QImage& source, int maxEdge)
{
    if (source.isNull())
        return;

    QImage image = source;
    if (maxEdge > 0 && qMax(image.width(), image.height()) > maxEdge)
        image = image.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    QByteArray  data = png;
    const char* mediaType = "image/png";

    // Screenshots of photos and gradients are several times smaller as JPEG;
    // plain UI is usually smaller (and sharper) as PNG. Only opaque images
    // can switch.
    if (png.size() > 200 * 1024 && !image.hasAlphaChannel()) {
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        if (image.save(&buffer, "JPEG", 85) && jpeg.size() < png.size() / 2) {
            data = jpeg;
            mediaType = "image/jpeg";
        }
    }

    QJsonArray images = result[kToolImagesKey].toArray();
    images.append(QJsonObject{{"media_type", mediaType},
                              {"data", QString::fromLatin1(data.toBase64())},
                              {"width", image.width()},
                              {"height", image.height()}});
    result[kToolImagesKey] = images;
}

QJsonArray toolTakeImages(QJsonObject& result)
{
    const QJsonArray images = result.take(kToolImagesKey).toArray();
    if (!images.isEmpty())
        result["images"] = images.size();
    return images;
}

QJsonObject toolImage(const QImage& image, const QJsonObject& payload, int maxEdge)
{
    QJsonObject result = toolOk(payload);
    toolAddImage(result, image, maxEdge);
    return result;
}


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
