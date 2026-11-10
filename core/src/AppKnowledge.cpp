#include "AppKnowledge.h"
#include "Client.h"
#include "Tool.h"
#include "ToolResult.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>

namespace QtLLM
{

namespace
{

const char* const kSearchTool = "app_help_search";
const char* const kReadTool   = "app_help_read";
const int kReadChars = 8000;   // one call's worth of a topic; longer ones are paged

} // namespace

AppKnowledge::AppKnowledge(Client* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
{
}

AppKnowledge::~AppKnowledge()
{
    m_enabled = false;
    syncTools();
}

void AppKnowledge::addTopic(const QString& id, const QString& title, const QString& text,
                            const QStringList& keywords)
{
    if (id.isEmpty())
        return;
    removeTopic(id);
    m_topics.append({id, title.isEmpty() ? id : title, text, keywords});
    syncTools();
}

int AppKnowledge::addTopicsFromDirectory(const QString& path)
{
    int added = 0;
    const QDir dir(path);
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.md"), QStringLiteral("*.txt")},
                                                   QDir::Files, QDir::Name)) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        QString title = text.section(QLatin1Char('\n'), 0, 0).trimmed();
        while (title.startsWith(QLatin1Char('#')))
            title.remove(0, 1);
        addTopic(info.completeBaseName(), title.trimmed(), text);
        ++added;
    }
    return added;
}

void AppKnowledge::removeTopic(const QString& id)
{
    for (int i = m_topics.size() - 1; i >= 0; --i) {
        if (m_topics[i].id == id)
            m_topics.removeAt(i);
    }
    syncTools();
}

void AppKnowledge::clear()
{
    m_topics.clear();
    syncTools();
}

QStringList AppKnowledge::topicIds() const
{
    QStringList ids;
    for (const Topic& topic : m_topics)
        ids.append(topic.id);
    return ids;
}

void AppKnowledge::setEnabled(bool enabled)
{
    m_enabled = enabled;
    syncTools();
}

void AppKnowledge::syncTools()
{
    if (!m_client)
        return;
    const bool wanted = m_enabled && !m_topics.isEmpty();
    if (wanted == m_registered)
        return;   // the definitions do not depend on the topics, so nothing to refresh
    m_registered = wanted;
    if (!wanted) {
        m_client->unregisterTool(kSearchTool);
        m_client->unregisterTool(kReadTool);
        return;
    }

    Tool searchTool;
    searchTool.setName(kSearchTool)
        .setTitle("Search application help")
        .setGroup("App knowledge")
        .setStatusText("Searching the application's help...")
        .setReadOnly(true)
        .setDescription(
            "Search what this application documents about itself: how to do things in it, what "
            "its features and terms mean, its limits. Use it before explaining how something "
            "works here or saying it cannot be done - your general knowledge does not cover this "
            "application. Returns matching topics with id, title and an excerpt; read the full "
            "text with app_help_read. Without a query all topics are listed.")
        .addParameter("query", "string", "Words for what you need to know, e.g. \"export pdf\".");
    m_client->registerTool(searchTool, [this](const QJsonObject& args) { return search(args); });

    Tool readTool;
    readTool.setName(kReadTool)
        .setTitle("Read application help")
        .setGroup("App knowledge")
        .setStatusText("Reading the application's help...")
        .setReadOnly(true)
        .setDescription("Return the text of one help topic found with app_help_search. A long "
                        "topic comes in parts: pass the returned nextOffset to continue.")
        .addParameter("id", "string", "Topic id from app_help_search.", true)
        .addParameter("offset", "integer", "Character position to continue from (default 0).");
    m_client->registerTool(readTool, [this](const QJsonObject& args) { return read(args); });
}

QJsonObject AppKnowledge::search(const QJsonObject& args) const
{
    const QStringList words = args.value("query").toString().toLower()
        .split(QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts);

    struct Match { int score; const Topic* topic; int at; };
    QList<Match> matches;
    for (const Topic& topic : m_topics) {
        const QString head = (topic.title + QLatin1Char(' ') + topic.id + QLatin1Char(' ')
                              + topic.keywords.join(QLatin1Char(' '))).toLower();
        const QString body = topic.text.toLower();
        int score = words.isEmpty() ? 1 : 0;
        int at = 0;
        for (const QString& word : words) {
            const int position = body.indexOf(word);
            if (head.contains(word))
                score += 3;
            else if (position >= 0)
                score += 1;
            if (position >= 0 && at == 0)
                at = position;
        }
        if (score > 0)
            matches.append({score, &topic, at});
    }
    if (matches.isEmpty())
        return toolOk({{"topics", QJsonArray()},
                       {"note", "nothing matched; call again without a query to list every topic"}});

    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.score > b.score; });
    QJsonArray topics;
    // ponytail: keyword scoring. Fine for a few hundred topics; beyond that,
    // or for questions phrased unlike the texts, an embedding index is the upgrade.
    const int limit = words.isEmpty() ? 100 : 8;
    for (int i = 0; i < matches.size() && i < limit; ++i) {
        const Topic& topic = *matches[i].topic;
        QJsonObject entry{{"id", topic.id}, {"title", topic.title}};
        if (!words.isEmpty()) {
            const int from = qMax(0, matches[i].at - 60);
            entry["excerpt"] = topic.text.mid(from, 220).simplified();
        }
        topics.append(entry);
    }
    return toolOk({{"topics", topics}});
}

QJsonObject AppKnowledge::read(const QJsonObject& args) const
{
    const QString id = args.value("id").toString();
    for (const Topic& topic : m_topics) {
        if (topic.id.compare(id, Qt::CaseInsensitive) != 0)
            continue;
        const int offset = qBound(0, args.value("offset").toInt(), topic.text.size());
        QJsonObject result{{"id", topic.id}, {"title", topic.title},
                           {"text", topic.text.mid(offset, kReadChars)}};
        if (offset + kReadChars < topic.text.size())
            result["nextOffset"] = offset + kReadChars;
        return toolOk(result);
    }
    return toolError(QStringLiteral("no topic '%1'; find topics with app_help_search").arg(id));
}

} // namespace QtLLM
