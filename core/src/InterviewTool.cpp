#include "InterviewTool.h"
#include "Client.h"
#include "ChatDockWidget.h"

#include <QJsonArray>
#include <QPointer>

namespace QtLLM
{

QString InterviewTool::name()
{
    return QStringLiteral("ask_user_question");
}

QString InterviewTool::description()
{
    return QStringLiteral(
        "Ask the user one or more questions through an interactive form shown in the chat. "
        "Use this when you are blocked on a decision only the user can make: choosing between "
        "approaches, clarifying ambiguous requirements, or confirming preferences. "
        "Each question offers 2-4 predefined options; the user can always type a custom answer "
        "instead, and may skip the whole form. Set multiSelect to true when several options can "
        "be chosen at once. Do not use this for questions you can answer yourself or for "
        "rhetorical confirmation.");
}

QJsonObject InterviewTool::parameterSchema()
{
    QJsonObject labelProp;
    labelProp["type"]        = "string";
    labelProp["description"] = "Display text of this choice; concise (1-5 words).";

    QJsonObject descProp;
    descProp["type"]        = "string";
    descProp["description"] = "What this option means or implies; shown below the label.";

    QJsonObject optionProps;
    optionProps["label"]       = labelProp;
    optionProps["description"] = descProp;

    QJsonObject optionItem;
    optionItem["type"]       = "object";
    optionItem["properties"] = optionProps;
    optionItem["required"]   = QJsonArray{ "label" };

    QJsonObject optionsProp;
    optionsProp["type"]        = "array";
    optionsProp["description"] = "The available choices; 2-4 distinct, mutually exclusive "
                                 "options (unless multiSelect). Do not add an 'Other' option — "
                                 "a free-text field is provided automatically.";
    optionsProp["minItems"]    = 2;
    optionsProp["maxItems"]    = 4;
    optionsProp["items"]       = optionItem;

    QJsonObject questionProp;
    questionProp["type"]        = "string";
    questionProp["description"] = "The complete question to ask; clear, specific, ends with '?'.";

    QJsonObject headerProp;
    headerProp["type"]        = "string";
    headerProp["description"] = "Very short label shown as a chip above the question "
                                "(max 12 chars), e.g. 'Database' or 'Approach'.";

    QJsonObject multiProp;
    multiProp["type"]        = "boolean";
    multiProp["description"] = "true to allow selecting multiple options (checkboxes) "
                               "instead of one (radio buttons). Default false.";

    QJsonObject customProp;
    customProp["type"]        = "boolean";
    customProp["description"] = "false to hide the free-text 'Other' field. Default true.";

    QJsonObject questionProps;
    questionProps["question"]    = questionProp;
    questionProps["header"]      = headerProp;
    questionProps["multiSelect"] = multiProp;
    questionProps["allowCustom"] = customProp;
    questionProps["options"]     = optionsProp;

    QJsonObject questionItem;
    questionItem["type"]       = "object";
    questionItem["properties"] = questionProps;
    questionItem["required"]   = QJsonArray{ "question", "options" };

    QJsonObject questionsProp;
    questionsProp["type"]        = "array";
    questionsProp["description"] = "Questions to ask the user (1-4).";
    questionsProp["minItems"]    = 1;
    questionsProp["maxItems"]    = 4;
    questionsProp["items"]       = questionItem;

    QJsonObject props;
    props["questions"] = questionsProp;

    QJsonObject schema;
    schema["type"]       = "object";
    schema["properties"] = props;
    schema["required"]   = QJsonArray{ "questions" };
    return schema;
}

void InterviewTool::registerOn(Client* client, ChatDockWidget* chat)
{
    QPointer<ChatDockWidget> guard(chat);
    client->registerTool(name(), description(), parameterSchema(),
        [guard](const QJsonObject& input) -> QJsonObject {
            if (!guard)
                return QJsonObject{{"status", "error"},
                                   {"error",  "chat widget no longer available"}};
            return guard->execInterview(input);
        });
}

} // namespace QtLLM
