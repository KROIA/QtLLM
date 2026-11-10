#pragma once
#include "QtLLM_base.h"
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

namespace QtLLM
{

class Client;

// What the application knows about itself, for the model to look up when it
// needs it: how-to texts, feature descriptions, terminology, limits. Without
// this the model knows the application only from its widget names.
//
// Topics cost no tokens until they are read. The model gets two tools:
// app_help_search(query) finds topics, app_help_read(id) returns one. Both
// exist only while there is at least one topic and the knowledge is enabled.
class QT_LLM_API AppKnowledge : public QObject
{
    Q_OBJECT
public:
    explicit AppKnowledge(Client* client, QObject* parent = nullptr);
    ~AppKnowledge() override;

    // id: short and stable ("export-pdf"); title: one line the model chooses
    // by; keywords: other words users have for the same thing. Adding an id
    // again replaces the topic.
    void addTopic(const QString& id, const QString& title, const QString& text,
                  const QStringList& keywords = QStringList());
    // Every *.md and *.txt file in the directory becomes a topic: the file
    // name without extension is the id, the first line (without a leading
    // '#') the title. Returns the number of topics added.
    int addTopicsFromDirectory(const QString& path);
    void removeTopic(const QString& id);
    void clear();
    QStringList topicIds() const;

    // Off: the tools are withdrawn, the topics are kept. Default on.
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Tool logic.
    QJsonObject search(const QJsonObject& args) const;
    QJsonObject read(const QJsonObject& args) const;

private:
    struct Topic
    {
        QString     id;
        QString     title;
        QString     text;
        QStringList keywords;
    };

    void syncTools();

    QPointer<Client> m_client;
    QList<Topic>     m_topics;
    bool             m_enabled = true;
    bool             m_registered = false;
};

} // namespace QtLLM
